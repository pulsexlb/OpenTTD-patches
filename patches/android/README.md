# Android build patches

These files are applied by `build-android-local.sh` to the pelya
`commandergenius` checkout (`$ANDROID_BUILD_DIR/sdl-android`, branch
`sdl_android`). They are kept in this repository, rather than generated inline
by the script, so they can be read, reviewed and diffed. Nothing here changes
the OpenTTD source itself.

## Manifest bits the build script injects

`project/jni/sdl2/android-project/app/src/main/AndroidManifest.xml` is the
template changeAppSettings.sh renders into `project/AndroidManifest.xml`, which
in turn is what gradle packages (`project/app/src/main/AndroidManifest.xml` is a
symlink to it). Four things are added there, all idempotently:

* `INTERNET` and `ACCESS_LOCAL_NETWORK` -- see the `AccessInternet` note below.
* `MANAGE_EXTERNAL_STORAGE` for the data directory picker.
* `android:screenOrientation`, which changeAppSettings.sh rewrites from the
  app's `ScreenOrientation` setting. Without the attribute present in the
  template its `sed` is a no-op, so that setting used to be silently ignored.
* `<meta-data android:name="SDL_ENV.SDL_IOS_ORIENTATIONS">`, because the
  attribute alone is not enough: OpenTTD opens a 640x480 SDL window, and SDL2's
  Android backend then calls `setRequestedOrientation()` with whatever
  `SDL_GetHint(SDL_HINT_ORIENTATIONS)` returns. With no hint it asks for
  `FULL_USER`, which *overrides* `android:screenOrientation` and hands control
  back to the device rotation -- observed as the app flipping to portrait
  mid-session, rendering into a 1080x2340 surface with half the screen black
  (`BLASTBufferQueue ... transform=7`). SDL2's
  `SDLActivity.getManifestEnvironmentVariables()` turns every
  `<meta-data android:name="SDL_ENV.*">` into an environment variable and
  `SDL_GetHint()` falls back to the environment, so the meta-data (which must sit
  inside `<application>`) is enough; the value follows the same
  `ScreenOrientation` setting.

* `android:allowNativeHeapPointerTagging="false"`. On arm64 Android 11+ bionic
  stores a tag (`0xB4`) in the top byte of heap pointers and aborts inside
  `free()` when it finds the tag stripped (`MaybeUntagAndCheckPointer()` in
  `malloc_tagged_pointers.h`, reported as `F libc: Pointer tag for 0x... was
  truncated`). That killed the game shortly after the base graphics set had been
  downloaded; the failing address had `0xB4` reduced to `0x04`, so something in
  the stack keeps a pointer in a field that cannot hold the top byte. The
  attribute is Android's supported opt-out (requires targetSdk >= 30, ignored
  below that) and only affects this app. **Remove it once the offending library
  is identified** -- `adb logcat -b crash` prints the tombstone naming it; the
  captured logs so far are tag-filtered and stop at the libc line, so the culprit
  is still unknown. A vendor hook library (`libMEOW_gift.so`) is loaded into the
  process on this device and is worth suspecting.

## Why patches instead of config switches

The app is built on the **SDL2** path (`LibSdlVersion=2.0` in
`AndroidAppSettings.cfg`, which OpenTTD requires -- upstream jgrpp has no SDL
1.2 video driver any more). On that path pelya's template is only a thin
wrapper around stock SDL2:

* `project/javaSDL2/Stubs.java` stubs `SettingsMenu.showConfig()` and
  `SetupTouchscreenKeyboardGraphics()` as empty methods,
* `project/javaSDL2/MainActivity.java` never calls `Settings.ProcessConfig()`,
* the on-screen keyboard renderer (`SDL_touchscreenkeyboard.c`) only exists in
  `jni/sdl-1.2/`, which is not linked.

So the settings that look like they should enable input features
(`ScreenOrientation`, `ImmersiveMode`, the `RedefinedKeys*` block,
`SettingsMenuKeyboard.*`) are either dead or unreachable here. The same applies
to the manifest: `project/app/build.gradle` has no `sourceSets` override, so
gradle packages SDL2's own
`project/jni/sdl2/android-project/app/src/main/AndroidManifest.xml`, not the
manifest generated into `project/AndroidManifest.xml` from
`AndroidAppSettings.cfg`.

## Files

| File | Applied as |
| --- | --- |
| `ModifierKeysOverlay.java` | copied to `project/javaSDL2/` |
| `TouchpadInput.java` | copied to `project/javaSDL2/` |
| `DataDirPicker.java` | copied to `project/javaSDL2/` |
| `app-icon.png` | copied over `project/jni/application/openttd-jgrpp/icon.png` |
| `javaSDL2-MainActivity.patch` | `patch -p1` against `project/javaSDL2/MainActivity.java` |

`app-icon.png` is the OpenTTD logo, converted from
<https://www.openttd.org/static/img/layout/openttd-128.gif> (the only size the
project publishes at that path) to a 128x128 RGBA PNG so transparency is kept.
It replaces pelya's JGRPP icon; `res/drawable/icon.png` (used by the in-app SDL
UI) and `res/mipmap-nodpi/ic_launcher.png` (what the manifest points at) are
symlinks to that one file, so a single copy is enough.

`changeAppSettings.sh` rewrites the `package` line of every application Java
file to `AppFullName` (here `org.openttd.pxp`), so the package declarations in
these files do not matter.

### javaSDL2-MainActivity.patch

Four things: attach the touch view and the modifier overlay, apply immersive
mode, split the tail of `onCreate()` into a `startup()` method, and branch on the
first-run directory choice. `startup()` also calls
`TouchpadInput.migrateScrollMode()`, which has to happen before the native thread
starts (see the `gui.scroll_mode` section below).

Immersive mode is applied from Java because SDL only enters it when the game asks
for a fullscreen window (`SDLActivity.mFullscreenModeActive` is set from
`COMMAND_CHANGE_WINDOW_STYLE`, which the native side only sends for
`SDL_WINDOW_FULLSCREEN`), and OpenTTD defaults to a windowed 640x480 window -- so
the status and navigation bars were never hidden. The flags are re-applied in
`onWindowFocusChanged()`, because swiping the bars out gives focus back. The
shipped default config also sets `fullscreen = true`, which makes SDL take that
route by itself; the Java code covers installations that already have a config
file (the shipped one is only written when it is missing).

The important subtlety is the `waitingForDataDirChoice` flag. The native thread
is normally held back by `dataDownloader` (`pauseNativeThread()` /
`resumeNativeThread()` only call `super` while it is non-null), and that object
is created in `startup()`. Deferring `startup()` until the dialog is answered
would therefore leave the gate open and the game would start immediately against
an undecided, still empty data directory -- which showed up as a silent exit
right after `Running main function SDL_main`. The flag keeps the thread paused
while the dialog is up; `startup()` clears it.

The build script restores this file from git before applying the patch, so it
always matches the revision the patch was written against (a "already applied"
marker cannot tell an older patch revision from the current one).

### TouchpadInput.java

Touch input for the SDL2 path. The name is historical: this started out as a
laptop-style touchpad (cursor moved by a relative drag, clicks placed at the
cursor) and that turned out to be the wrong model -- it made taps land somewhere
else than the finger, made a double-tap-drag start only after a slop, and made
map dragging impossible. SDL2 synthesises the sensible behaviour on its own, so
this class now reproduces it instead of replacing it.

What SDL2 already does: `SDLSurface.onTouch()` -> `SDLActivity.onNativeTouch()`
-> `SDL_SendTouch()` in `src/events/SDL_touch.c`, which (with
`SDL_HINT_TOUCH_MOUSE_EVENTS`, on by default) emits an absolute mouse motion plus
a **left** button press/release for the first finger. That is why a stock SDL2
app taps and drags without any Java code, and it is exactly what this class
mirrors: motion is sent absolute (`relative=false`), the left button is pressed
only after the touch has moved past a slop (so a tap cannot turn into a
one-pixel drag and the press lands on the touch-down position), and the press is
not sent at all until then, so a second finger can cancel it silently.

| gesture | result |
| --- | --- |
| single finger tap | left click at the touch position |
| single finger drag | left button held while moving: map scrolling, moving windows, drag-to-build, ... |
| two finger vertical swipe | wheel, one notch per ~1/20 of the screen height |
| two finger pinch / spread | wheel (zoom), one notch per ~1.4x change in finger distance |
| two finger tap | right click |

The two-finger swipe and the pinch are the same wheel channel (which is what
desktop zooming uses, one notch = one discrete ZoomLevel = 2x), so they have to
be told apart: while both fingers are down the gesture is undecided until either
the parallel movement or the distance change commits past a threshold (~24dp,
like pelya's SDL 1.2 multitouch gesture detection) and is then locked into swipe
or pinch for the rest of the touch. Real fingers never move perfectly, so the
losing dimension's jitter is ignored; anchoring the winning dimension at commit
keeps pre-lock movement from double counting. A committed gesture never
degenerates into a tap, even if it sits so close to a notch boundary that no
wheel notch comes out of it.

There is deliberately no double-tap-drag: the left button is already held by the
first drag, so nothing had to be invented for it (and its "first tap still
clicks" behaviour was the source of the confusion above). Two-finger gestures are
recognised as soon as the second finger touches down; if the left button is held
at that moment it is released, so the finger that is left behind after a
gesture cannot keep a button pressed.

No SDL2 or game change is needed: `SDLActivity.onNativeMouse()` is a public
static native and `Android_OnMouse()` in `src/video/android/SDL_androidmouse.c`
already maps `ACTION_SCROLL` (8) to `SDL_SendMouseWheel()`. Two details of that
interface are easy to get wrong and both cost a debugging round:

* The second argument is the **bitmask of buttons currently held**, not the
  button: `Android_OnMouse()` diffs it against the state it remembers
  (`changes = state & ~last_state` for a press, `last_state & ~state` for a
  release) and translates the difference. A release therefore has to pass `0`.
  Passing the button again yields an empty difference, so the press is delivered
  and the release is not -- the game sees a button held down forever and no click
  ever happens (this is exactly how taps were broken once).
* With `relative=true` the coordinates are deltas accumulated onto SDL's own
  position, which starts at `(0,0)` and is not clamped to the window. Motion is
  therefore sent absolute (`relative=false`), which is also what SDL's own touch
  synthesis does.

Wheel `+1` is "zoom in", because OpenTTD does `_cursor.wheel--` for positive
`wheel.y` and zooms in for negative values; a swipe up therefore sends `+1`. What
the wheel does is OpenTTD's `scrollwheel_scrolling` setting (zoom by default, map
scrolling if set to ScrollMap).

#### gui.scroll_mode

Dragging the map with the left button only works in
`ViewportScrollMode::MapLMB`; OpenTTD's default on Unix is `MapRMB`, i.e. the map
follows the *right* button. A finger cannot hold the right button without also
producing a right click (which opens or closes things), so a touch build has to
start on `MapLMB`:

* the default config the build packages gets `scroll_mode = 3` (see the comment
  block in `build-android-local.sh`), which covers fresh installations, and
* `TouchpadInput.migrateScrollMode()`, called from the patched `startup()`,
  rewrites the line in an already existing `<datadir>/.openttd/openttd.cfg`
  before the native thread starts (so no restart is needed).

The migration runs **once** per installation (a `SharedPreferences` flag), so a
scroll mode the player picks in the game options afterwards is left alone. The
file is written to a temporary file first and then renamed, so a failure cannot
truncate the config.

The layer is always active; there is no switch for it, because SDL's own
handling and this one now produce the same single-finger behaviour and only the
two-finger gestures are added on top.

### ModifierKeysOverlay.java

On-screen sticky `Ctrl` / `Shift` / `Alt` buttons plus `KEY`, `ESC` and `DEL`, on
the right edge and vertically centered. Ported from the same-named class in the
OpenTTD JGRPP Android port, with the injection path changed for SDL2:

* keys go through `SDLActivity.onNativeKeyDown()` / `onNativeKeyUp()`, which are
  `public static` natives in stock SDL2 and feed the same queue as the hardware
  keyboard and the IME;
* `Ctrl` / `Shift` / `Alt` are sticky: tap to hold (button highlighted), tap
  again to release, so they can be combined with a game action. `ESC` and `DEL`
  are one-shot press+release instead - they trigger an action by themselves and
  must not stay held. Their Android keycodes (111, 112) are what SDL2's
  `Android_Keycodes` table turns into `SDL_SCANCODE_ESCAPE` (OpenTTD's
  `WKC_ESC`: cancel the current tool) and `SDL_SCANCODE_DELETE`
  (`WKC_DELETE`: close the open windows; Shift/Ctrl variants exist in the
  game's hotkeys);
* `KEY` toggles the soft keyboard. Whether the keyboard is up is decided from
  the window insets (on Android 11+ `WindowInsets.isVisible(Type.ime())`), not
  from SDL's `isScreenKeyboardShown()`;
* the overlay is added to `SDLActivity.getContentView()` inside a
  non-clickable full-screen `FrameLayout`, so touches outside the buttons fall
  through to the SDL surface.

`KEY` calls `SDLActivity.showTextInput()`, which is SDL's own route: it focuses
the hidden text view SDL keeps in the layout and asks the IME for the keyboard,
so what is typed arrives as SDL text input - the same call the game makes when a
text box gains focus. Two things about it are worth knowing:

* `SDLActivity.isScreenKeyboardShown()` is *not* a reliable "is the keyboard up"
  signal: besides SDL's own flag it asks `InputMethodManager.isAcceptingText()`,
  which stays true once the app has an input connection. A toggle built on it
  can therefore decide to hide a keyboard that is not on screen - the button
  then appeared to do nothing at all. The window insets are used instead, with
  the overlay's own flag as a fallback while the view is not attached yet.
* the request can race with the layout/focus of that view, so it is repeated
  400 ms later, and if the platform still reports no keyboard the IME is forced
  up with `toggleSoftInput()`. Both steps log their outcome under the `SDL` tag.

Known limitation: characters typed on the soft keyboard only reach the game
while SDL is in text-input mode, which OpenTTD enters when a text box is
focused (naming a vehicle or station). For in-game commands use the modifier
buttons, which work unconditionally.

### DataDirPicker.java

Asks once, on the first run, where game data should live, and applies the
answer to `Globals.DataDir` before `Settings.setEnvVars()` /
`Settings.nativeChdir()` run.

* The choice is kept in `SharedPreferences` (`openttd_datadir` / `user_dir`),
  because it has to be readable before the native thread starts.
* "Choose folder" requests `MANAGE_EXTERNAL_STORAGE` on Android 11+ (required
  for a real filesystem path outside the app sandbox), then opens the Storage
  Access Framework picker and maps the returned tree URI back to a real path
  (`primary` -> `/storage/emulated/0`, other volumes -> `/storage/<volume>`).
* The picked directory is verified by writing and deleting a probe file; if
  that fails the internal directory is used and the user is told.
* Declining stores the internal directory, so the question is asked only once.

Deliberate simplifications against the original (which the JGRPP port used in a
settings-menu flow): no data migration and no process restart, because the
choice happens before anything is downloaded; and no menu entry to change the
directory later, because the SDL2 path has no settings menu. Add those later if
a "change directory" flow is wanted.

The accompanying manifest permission is injected by the build script (it also
has to be idempotent for the orientation attribute, so both live together
there).

## Upstream drift

`javaSDL2-MainActivity.patch` is a plain unified diff against pelya's file. The
build script applies it with `patch --forward` and **aborts the build** if it no
longer applies, so an upstream change surfaces as an explicit error instead of a
silently missing feature.
