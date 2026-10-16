/*
Touch input for the SDL2 build path.

SDL2 already turns touches into mouse input by itself: SDLSurface.onTouch() ->
SDLActivity.onNativeTouch() -> SDL_SendTouch(), and SDL_touch.c then synthesises
an absolute mouse motion plus a *left* button press/release for the first finger
(SDL_HINT_TOUCH_MOUSE_EVENTS defaults to on). That is why a stock SDL2 app taps
and drags without a single line of Java, and it is the behaviour this class
reproduces instead of inventing a cursor of its own - a tap clicks exactly where
the finger is and a drag starts scrolling exactly where the finger went down.
On top of that it only adds what SDL has no answer for:

  one finger tap          -> left click at the touch position
  one finger drag         -> left button held while moving: map scrolling,
                             window dragging, drag-to-build, ...
  two finger tap          -> right click
  two finger swipe up/down-> mouse wheel (zoom), one notch per ~1/20 screen height
  two finger pinch        -> mouse wheel (zoom), one notch per ~1.4x distance;
                             the gesture is locked into pinch or swipe, which
                             ever commits first, so they never fight

Everything goes through SDLActivity.onNativeMouse(), which is a public static
native, so no SDL2 or game changes are needed. Two details of that interface
matter (both in src/video/android/SDL_androidmouse.c):

  * the second argument is not a button but the *bitmask of buttons currently
    held*, which Android_OnMouse() diffs against its own remembered state, so a
    release must pass 0, not the button again, otherwise "changes" comes out
    empty and the button is never released;
  * coordinates are sent absolute (relative=false). With relative=true they are
    deltas accumulated onto SDL's own position, which starts at (0,0) and is not
    clamped to the window.

The left button is only pressed once the finger has moved past a slop, so a tap
cannot accidentally turn into a one-pixel drag, and a second finger arriving
cancels the pending press instead of leaving a click behind.

Map scrolling with the left button needs the game to be in "map scrolls with the
left mouse button" mode. OpenTTD's default for that is the right button (Unix),
which a finger cannot reach without also firing a right click, so
migrateScrollMode() switches the setting once - see the comment there.

ChangeAppSettings.sh rewrites the package line to the app's package.
*/
package net.sourceforge.clonekeenplus;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.util.ArrayList;
import java.util.List;

public class TouchpadInput
{
	/* Action codes / button bits understood by SDL2's Android_OnMouse. */
	private static final int ACTION_DOWN = 0;
	private static final int ACTION_UP = 1;
	private static final int ACTION_MOVE = 2;
	private static final int ACTION_SCROLL = 8;
	private static final int BUTTON_PRIMARY = 1;
	private static final int BUTTON_SECONDARY = 2;

	/* Vertical two-finger travel per wheel notch, as a fraction of the view
	 * height (pelya's SDL 1.2 input layer used height/20). */
	private static final int WHEEL_TRAVEL_DIVISOR = 20;
	/* Distance ratio between the two fingers per pinch-zoom notch. OpenTTD's
	 * zoom is discrete (one wheel notch = one ZoomLevel = 2x), so this must not
	 * be too small, or a single pinch would climb several levels at once. */
	private static final float PINCH_NOTCH_RATIO = 1.4f;
	/* How far the gesture has to commit (px in dp units) before it is locked
	 * into pinch or swipe; below that it can still become either. */
	private static final float GESTURE_LOCK_DP = 24f;
	/* How far a touch may wander before it counts as a drag instead of a tap.
	 * Small enough that a drag starts as soon as the finger really moves, large
	 * enough that the jitter of a tap cannot turn into one. */
	private static final float SLOP_DP = 8f;
	/* A tap has to be over within this long. */
	private static final long TAP_TIMEOUT_MS = 500;

	/* ViewportScrollMode::MapLMB - see src/settings_type.h. */
	private static final int SCROLL_MODE_MAP_LMB = 3;

	private static final String PREFS_NAME = "openttd_input";
	private static final String PREF_SCROLL_MODE_MIGRATED = "scroll_mode_migrated";
	private static final String TAG = "SDL";

	private final View view;
	private final float density;

	/* One-finger state. */
	private float downX, downY, lastX, lastY;
	private long downTime;
	private boolean moved = false;     /* far enough to be a drag */
	private boolean leftDown = false;  /* we are holding the left button */

	/* Two-finger state. Kept until the last finger is lifted, so the finger
	 * that stays behind cannot turn into a stray click or drag. */
	private boolean twoFingers = false;
	private boolean gestureMoved = false;
	private float lastGestureY, wheelAccum;

	/* Two-finger gesture discrimination: while both fingers are down, the
	 * gesture is undecided until either the distance between them or their
	 * parallel movement exceeds GESTURE_LOCK_DP - then it is locked into
	 * pinch-zoom or swipe-to-wheel for the rest of the touch. */
	private static final int GESTURE_UNDECIDED = 0;
	private static final int GESTURE_PINCH = 1;
	private static final int GESTURE_SWIPE = 2;
	private int gestureType = GESTURE_UNDECIDED;
	private float gestureDist0, gestureAvgY0, gestureAccum;

	private TouchpadInput(Context context)
	{
		this.density = context.getResources().getDisplayMetrics().density;

		this.view = new View(context);
		/* Transparent but still visible, so it can receive touches. */
		this.view.setBackgroundColor(0);
		this.view.setOnTouchListener(new View.OnTouchListener() {
			@Override
			public boolean onTouch(View v, MotionEvent event)
			{
				return handle(event);
			}
		});
	}

	/**
	 * Adds the touch view above the SDL surface. It must be added before the
	 * modifier key overlay so that the overlay's buttons stay on top and keep
	 * receiving their own touches.
	 */
	public static void attach(Activity activity)
	{
		View content = SDLActivity.getContentView();
		if (!(content instanceof ViewGroup)) {
			Log.i(TAG, "TouchpadInput: no SDL content view, touch input disabled");
			return;
		}
		TouchpadInput input = new TouchpadInput(activity);
		((ViewGroup) content).addView(input.view, new ViewGroup.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
		Log.i(TAG, "TouchpadInput: attached");
	}

	/* @return true when the touch was consumed as touch input. */
	private boolean handle(MotionEvent event)
	{
		switch (event.getActionMasked())
		{
			case MotionEvent.ACTION_DOWN:
				this.downX = this.lastX = event.getX(0);
				this.downY = this.lastY = event.getY(0);
				this.downTime = event.getEventTime();
				this.moved = false;
				this.twoFingers = false;
				this.gestureMoved = false;
				this.gestureType = GESTURE_UNDECIDED;
				this.wheelAccum = 0f;
				/* Put the mouse under the finger right away, but press nothing
				 * yet: a tap is only turned into a click when the finger is
				 * lifted again, and a second finger can still cancel it. */
				sendCursor(this.lastX, this.lastY);
				return true;

			case MotionEvent.ACTION_POINTER_DOWN:
				if (this.leftDown)
				{
					/* A drag that grew a second finger is a gesture now, and a
					 * held button would be stuck until the finger is lifted
					 * again - which may never happen. */
					this.leftDown = false;
					SDLActivity.onNativeMouse(0, ACTION_UP, 0f, 0f, true);
				}
				if (event.getPointerCount() >= 2)
				{
					this.twoFingers = true;
					this.gestureMoved = false;
					this.gestureType = GESTURE_UNDECIDED;
					this.wheelAccum = 0f;
					this.lastGestureY = averageY(event);
					this.gestureAvgY0 = this.lastGestureY;
					this.gestureDist0 = spread(event);
				}
				return true;

			case MotionEvent.ACTION_MOVE:
				if (this.twoFingers)
				{
					if (event.getPointerCount() >= 2)
					{
						twoFingerMove(event);
					}
				}
				else if (event.getPointerCount() == 1)
				{
					float x = event.getX(0), y = event.getY(0);
					this.lastX = x;
					this.lastY = y;
					sendCursor(x, y);
					if (!this.moved
							&& (Math.abs(x - this.downX) > SLOP_DP * this.density
								|| Math.abs(y - this.downY) > SLOP_DP * this.density))
					{
						this.moved = true;
						this.leftDown = true;
						SDLActivity.onNativeMouse(BUTTON_PRIMARY, ACTION_DOWN, 0f, 0f, true);
					}
				}
				return true;

			case MotionEvent.ACTION_POINTER_UP:
				/* Ignore the lift; the remaining finger must not be able to
				 * click or drag any more. */
				return true;

			case MotionEvent.ACTION_UP:
				if (this.leftDown)
				{
					this.leftDown = false;
					SDLActivity.onNativeMouse(0, ACTION_UP, 0f, 0f, true);
				}
				else if (!this.moved && event.getEventTime() - this.downTime <= TAP_TIMEOUT_MS)
				{
					if (this.twoFingers)
					{
						if (!this.gestureMoved) click(BUTTON_SECONDARY);
					}
					else
					{
						click(BUTTON_PRIMARY);
					}
				}
				return true;

			case MotionEvent.ACTION_CANCEL:
				if (this.leftDown)
				{
					this.leftDown = false;
					SDLActivity.onNativeMouse(0, ACTION_UP, 0f, 0f, true);
				}
				return true;
		}
		return true;
	}

	private static float averageY(MotionEvent event)
	{
		float sum = 0f;
		for (int i = 0; i < event.getPointerCount(); i++) sum += event.getY(i);
		return sum / event.getPointerCount();
	}

	/* Current distance between the first two fingers. */
	private static float spread(MotionEvent event)
	{
		float dx = event.getX(0) - event.getX(1);
		float dy = event.getY(0) - event.getY(1);
		return (float)Math.sqrt(dx * dx + dy * dy);
	}

	/**
	 * Both fingers down and moving. The gesture is undecided until either the
	 * distance between the fingers (pinch-zoom) or their parallel movement
	 * (swipe-to-wheel) commits past GESTURE_LOCK_DP; real fingers never move
	 * perfectly, so committing on the dominant dimension keeps a pinch from
	 * spraying wheel notches and a swipe from zooming.
	 */
	private void twoFingerMove(MotionEvent event)
	{
		float y = averageY(event);
		float dist = spread(event);

		if (this.gestureType == GESTURE_UNDECIDED)
		{
			float lock = GESTURE_LOCK_DP * this.density;
			/* Parallel movement relative to where the gesture started ... */
			float movedY = Math.abs(y - this.gestureAvgY0);
			/* ... or distance change relative to where it started. */
			float pinch = Math.abs(dist - this.gestureDist0);
			if (movedY < lock && pinch < lock) return;
			this.gestureType = (pinch > movedY) ? GESTURE_PINCH : GESTURE_SWIPE;
			/* A committed gesture is never a tap, even if it is so close to a
			 * notch boundary that no wheel output comes out of it. */
			this.gestureMoved = true;
			/* Anchor the winning dimension here so that the movement already
			 * made does not double up as wheel output. */
			this.lastGestureY = y;
			this.wheelAccum = 0f;
		}

		if (this.gestureType == GESTURE_SWIPE)
		{
			/* Fingers moving up (decreasing y) scroll one way. */
			this.wheelAccum += this.lastGestureY - y;
			this.lastGestureY = y;
			float travel = Math.max(this.view.getHeight(), 1) / (float)WHEEL_TRAVEL_DIVISOR;
			if (Math.abs(this.wheelAccum) >= travel) this.gestureMoved = true;
			while (this.wheelAccum >= travel)
			{
				this.wheelAccum -= travel;
				sendWheel(1);
			}
			while (this.wheelAccum <= -travel)
			{
				this.wheelAccum += travel;
				sendWheel(-1);
			}
		}
		else /* GESTURE_PINCH */
		{
			/* Anchored multiplicatively: after every notch the reference
			 * distance moves, so a long steady pinch zooms at a constant rate
			 * instead of running away. Spread out (dist > ref) is zoom in. */
			if (dist <= 0f || this.gestureDist0 <= 0f) return;
			if (dist > this.gestureDist0 * PINCH_NOTCH_RATIO)
			{
				this.gestureDist0 = dist;
				this.gestureMoved = true;
				sendWheel(1);
			}
			else if (dist < this.gestureDist0 / PINCH_NOTCH_RATIO)
			{
				this.gestureDist0 = dist;
				this.gestureMoved = true;
				sendWheel(-1);
			}
		}
	}

	/* Absolute, so the game's cursor lands exactly where the finger is. */
	private void sendCursor(float x, float y)
	{
		SDLActivity.onNativeMouse(0, ACTION_MOVE, x, y, false);
	}

	private void click(int button)
	{
		/* The press carries the held-button bitmask, the release must carry an
		 * empty one - that is what Android_OnMouse() diffs. */
		SDLActivity.onNativeMouse(button, ACTION_DOWN, 0f, 0f, true);
		SDLActivity.onNativeMouse(0, ACTION_UP, 0f, 0f, true);
	}

	private void sendWheel(int direction)
	{
		/* ACTION_SCROLL becomes SDL_SendMouseWheel(): the x argument is the
		 * horizontal wheel, y the vertical one. OpenTTD decrements
		 * _cursor.wheel for positive y and zooms in for negative values, so
		 * +1 is "one notch towards zoom in". */
		SDLActivity.onNativeMouse(0, ACTION_SCROLL, 0f, direction, false);
	}

	/**
	 * One-off switch of gui.scroll_mode to "map scrolls with the left mouse
	 * button" (ViewportScrollMode::MapLMB).
	 *
	 * The map cannot be dragged with a finger otherwise: OpenTTD's default for
	 * Unix is MapRMB, and holding the right button is not something a touch can
	 * do without also producing a right click (which opens context menus). The
	 * change is applied at most once - a scroll mode the player picks in the
	 * game options afterwards is left alone - and before the native thread
	 * starts, so it takes effect without a restart.
	 */
	static void migrateScrollMode(Context context)
	{
		SharedPreferences prefs = null;
		try
		{
			prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
			if (prefs.getBoolean(PREF_SCROLL_MODE_MIGRATED, false)) return;
		}
		catch (Exception e) {}

		boolean done = true;
		/* The packaged default config lives in .openttd/, which is where
		 * OpenTTD looks first (personal directory). */
		File config = new File(Globals.DataDir, ".openttd/openttd.cfg");
		if (!config.isFile()) config = new File(Globals.DataDir, "openttd.cfg");
		if (config.isFile())
		{
			try
			{
				done = setScrollMode(config, SCROLL_MODE_MAP_LMB);
			}
			catch (Exception e)
			{
				done = false;
				Log.i(TAG, "TouchpadInput: cannot update " + config + ": " + e);
			}
		}
		else
		{
			/* No config yet: the packaged default (which ships the same value)
			 * will be installed on this run. */
			Log.i(TAG, "TouchpadInput: no config to migrate yet");
		}

		if (done && prefs != null)
		{
			try
			{
				prefs.edit().putBoolean(PREF_SCROLL_MODE_MIGRATED, true).commit();
			}
			catch (Exception e) {}
		}
	}

	/* @return true when the file is now (or already was) set to mode. */
	private static boolean setScrollMode(File config, int mode) throws Exception
	{
		List<String> lines = new ArrayList<String>();
		BufferedReader in = new BufferedReader(new InputStreamReader(new FileInputStream(config), "UTF-8"));
		try
		{
			String line;
			while ((line = in.readLine()) != null) lines.add(line);
		}
		finally
		{
			in.close();
		}

		final String value = "scroll_mode = " + mode;
		boolean inGui = false, found = false;
		int guiHeader = -1;
		for (int i = 0; i < lines.size() && !found; i++)
		{
			String line = lines.get(i).trim();
			if (line.startsWith("["))
			{
				inGui = line.equals("[gui]");
				if (inGui && guiHeader < 0) guiHeader = i;
			}
			else if (inGui)
			{
				int eq = line.indexOf('=');
				if (eq > 0 && line.substring(0, eq).trim().equals("scroll_mode"))
				{
					if (line.substring(eq + 1).trim().equals(String.valueOf(mode))) return true;
					lines.set(i, value);
					found = true;
				}
			}
		}
		if (!found)
		{
			if (guiHeader < 0)
			{
				lines.add("[gui]");
				lines.add(value);
			}
			else
			{
				lines.add(guiHeader + 1, value);
			}
		}

		/* Write next to the original and only then move it into place, so a
		 * failure in the middle cannot leave a truncated config behind. */
		File temp = new File(config.getAbsolutePath() + ".tmp");
		OutputStreamWriter out = new OutputStreamWriter(new FileOutputStream(temp), "UTF-8");
		try
		{
			for (int i = 0; i < lines.size(); i++)
			{
				out.write(lines.get(i));
				out.write("\n");
			}
		}
		finally
		{
			out.close();
		}
		if (!temp.renameTo(config))
		{
			temp.delete();
			Log.i(TAG, "TouchpadInput: cannot replace " + config);
			return false;
		}
		Log.i(TAG, "TouchpadInput: " + config + " set to " + value);
		return true;
	}
}
