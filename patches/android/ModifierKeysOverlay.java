/*
On-screen modifier keys (Ctrl / Shift / Alt) plus a button for the Android soft
keyboard, for the SDL2 build path.

Adapted from the OpenTTD JGRPP Android port's ModifierKeysOverlay: pelya's SDL2
template is only a thin wrapper around stock SDL2 (see project/javaSDL2/), so
the SDL 1.2 DemoGLSurfaceView/nativeKey path is unavailable. Instead keys are
injected through SDLActivity.onNativeKeyDown()/onNativeKeyUp(), which are
public static natives in stock SDL2 (SDLActivity.java) and feed the same event
queue the hardware keyboard and the IME use. No C or game code is involved.

Modifier buttons are sticky: tap once to hold the key down (button highlighted),
tap again to release it, so they can be combined with a game action. ESC and DEL
are plain one-shot presses (cancel the current tool, close the open windows).
The KEY button toggles the Android soft keyboard.

ChangeAppSettings.sh rewrites the package line to the app's package.
*/
package net.sourceforge.clonekeenplus;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import org.libsdl.app.SDLActivity;

public class ModifierKeysOverlay extends LinearLayout
{
	/* Android key codes (KeyEvent.KEYCODE_*) */
	private static final int KEYCODE_CTRL_LEFT = 113;
	private static final int KEYCODE_SHIFT_LEFT = 59;
	private static final int KEYCODE_ALT_LEFT = 57;
	/* SDL2's Android keyboard table maps these to SDL_SCANCODE_ESCAPE and
	 * SDL_SCANCODE_DELETE, i.e. OpenTTD's WKC_ESC (cancel the current tool) and
	 * WKC_DELETE (close the open windows). */
	private static final int KEYCODE_ESCAPE = 111;
	private static final int KEYCODE_FORWARD_DEL = 112;

	private static final String TAG = "SDL";

	/* Our own idea of whether the soft keyboard is up. SDL's
	 * isScreenKeyboardShown() also asks InputMethodManager whether the app
	 * accepts text, which stays true once an input connection exists - it can
	 * therefore report a keyboard while there is none on screen, and a toggle
	 * built on it would hide instead of show. */
	private static boolean keyboardShown = false;

	public ModifierKeysOverlay(Context context)
	{
		super(context);
		setOrientation(VERTICAL);

		addModifierButton("Ctrl", KEYCODE_CTRL_LEFT);
		addModifierButton("Shift", KEYCODE_SHIFT_LEFT);
		addModifierButton("Alt", KEYCODE_ALT_LEFT);
		addKeyboardButton();
		addActionButton("ESC", KEYCODE_ESCAPE);
		addActionButton("DEL", KEYCODE_FORWARD_DEL);
	}

	/**
	 * Adds the overlay to the SDL activity's content view, on the right edge and
	 * vertically centered. The host FrameLayout spans the screen but is neither
	 * clickable nor focusable, so touches outside the buttons fall through to the
	 * SDL surface below instead of being swallowed.
	 */
	public static void attach(Activity activity)
	{
		View content = SDLActivity.getContentView();
		if (!(content instanceof ViewGroup)) return;

		FrameLayout host = new FrameLayout(activity);
		host.setClickable(false);
		host.setFocusable(false);

		FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
				Gravity.RIGHT | Gravity.CENTER_VERTICAL);
		lp.rightMargin = dp(activity, 4);
		host.addView(new ModifierKeysOverlay(activity), lp);

		((ViewGroup) content).addView(host, new ViewGroup.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
	}

	private void addModifierButton(final String label, final int keyCode)
	{
		final Button b = makeButton(label);
		b.setTag(Boolean.FALSE);
		b.setOnClickListener(new OnClickListener() {
			@Override
			public void onClick(View v)
			{
				boolean down = !((Boolean) b.getTag());
				b.setTag(down);
				if (down) {
					SDLActivity.onNativeKeyDown(keyCode);
				} else {
					SDLActivity.onNativeKeyUp(keyCode);
				}
				updateStyle(b, down);
			}
		});
		addView(b);
	}

	/*
	 * A key that is pressed and released right away - used for the ones that
	 * trigger an action (ESC cancels the current tool, DEL closes the open
	 * windows) instead of modifying the next key, so they must not stick.
	 */
	private void addActionButton(String label, final int keyCode)
	{
		final Button b = makeButton(label);
		b.setOnClickListener(new OnClickListener() {
			@Override
			public void onClick(View v)
			{
				SDLActivity.onNativeKeyDown(keyCode);
				SDLActivity.onNativeKeyUp(keyCode);
			}
		});
		addView(b);
	}

	private void addKeyboardButton()
	{
		final Button b = makeButton("KEY");
		b.setOnClickListener(new OnClickListener() {
			@Override
			public void onClick(View v)
			{
				if (isImeVisible())
				{
					keyboardShown = false;
					InputMethodManager imm = imm();
					if (imm != null) imm.hideSoftInputFromWindow(getWindowToken(), 0);
					Log.i(TAG, "ModifierKeysOverlay: KEY hides the soft keyboard");
					return;
				}

				keyboardShown = true;
				Log.i(TAG, "ModifierKeysOverlay: KEY shows the soft keyboard (SDL says "
						+ SDLActivity.isScreenKeyboardShown() + ")");
				showSoftKeyboard();
				/* The request can race with the layout and focus of the view SDL
				 * asks the IME for, so try again once that has settled. */
				b.postDelayed(new Runnable() {
					@Override
					public void run()
					{
						if (!keyboardShown) return;
						showSoftKeyboard();
						if (!SDLActivity.isScreenKeyboardShown())
						{
							/* Still nothing: force the IME up for whatever view
							 * has focus, which is SDL's text view. */
							InputMethodManager imm = imm();
							if (imm != null) imm.toggleSoftInput(InputMethodManager.SHOW_IMPLICIT, 0);
						}
						Log.i(TAG, "ModifierKeysOverlay: KEY, keyboard shown = "
								+ SDLActivity.isScreenKeyboardShown());
					}
				}, 400);
			}
		});
		addView(b);
	}

	/*
	 * Goes through SDL's own route: SDLActivity.showTextInput() focuses the
	 * hidden text view it keeps in the layout and asks the IME for the keyboard,
	 * so what is typed arrives as SDL text input, exactly like in a text box.
	 */
	private static void showSoftKeyboard()
	{
		SDLActivity.showTextInput(0, 0, 0, 0);
	}

	private InputMethodManager imm()
	{
		return (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
	}

	/*
	 * Whether the IME is really on screen. SDL's own answer is not usable for
	 * the toggle (see the comment on keyboardShown), so the window insets are
	 * asked instead: with the navigation bar hidden by immersive mode, a bottom
	 * inset can only be the keyboard. Below Android 11 there is no
	 * WindowInsets.Type.ime(), so the inset height is compared against a
	 * threshold instead; either way falls back to our own flag when the view is
	 * not attached yet and there are no insets to look at.
	 */
	private boolean isImeVisible()
	{
		WindowInsets insets = getRootWindowInsets();
		if (insets == null) return keyboardShown;
		if (android.os.Build.VERSION.SDK_INT >= 30) return insets.isVisible(WindowInsets.Type.ime());
		return insets.getSystemWindowInsetBottom() >= dp(getContext(), 100);
	}

	private Button makeButton(String label)
	{
		Button b = new Button(getContext());
		b.setText(label);
		b.setTextSize(10f);
		b.setTypeface(Typeface.DEFAULT_BOLD);
		/* Never take keyboard focus away from the game view. */
		b.setFocusable(false);
		b.setFocusableInTouchMode(false);
		b.setMinimumWidth(0);
		b.setMinimumHeight(0);
		b.setPadding(0, 0, 0, 0);
		LayoutParams lp = new LayoutParams(dp(getContext(), 44), dp(getContext(), 30));
		lp.topMargin = dp(getContext(), 6);
		b.setLayoutParams(lp);
		updateStyle(b, false);
		return b;
	}

	private void updateStyle(Button b, boolean active)
	{
		if (active) {
			b.setBackgroundColor(Color.argb(0xDD, 0x1A, 0x8C, 0xFF));
			b.setTextColor(Color.WHITE);
		} else {
			b.setBackgroundColor(Color.argb(0x99, 0x00, 0x00, 0x00));
			b.setTextColor(Color.argb(0xE6, 0xFF, 0xFF, 0xFF));
		}
	}

	private static int dp(Context ctx, int value)
	{
		return Math.round(ctx.getResources().getDisplayMetrics().density * value);
	}
}
