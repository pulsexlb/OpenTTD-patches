#!/usr/bin/env bash
# Local Android APK build, mirroring the "Android APK Release" GitHub Actions
# workflow. arm64-v8a only.
#
# Usage:
#   ./build-android-local.sh [--fresh] [version]
#     --fresh    wipe the packaging tree (recompiles ICU/boost, ~1h)
#     version    APK version name (default: current git tag, e.g. pxp-2610.3;
#                a dirty worktree gets a -dirty suffix)
#
# Environment:
#   ANDROID_BUILD_DIR        where the pelya/commandergenius tree is kept
#                            (default ~/Projects/openttd-android-build)
#   ANDROID_PREBUILT_CACHE   directory holding cached prebuilt dependencies
#                            (iconv/ICU/obj), restored after the checkout; used
#                            by CI to skip the ~1h ICU/Boost build
#   BUNDLE_OPENGFX=1         also bundle OpenGFX instead of offering the
#                            in-game download on first run
#
# Requirements: java 17, Android SDK ($ANDROID_SDK_ROOT or /opt/android-sdk),
# Android NDK ($ANDROID_NDK_LATEST_HOME or /opt/android-ndk/<latest>).
#
# CI notes (.github/workflows/android.yml): the game data package built in step
# 3b comes from the native build outputs of step 7, so a *fresh* tree has none.
# CI therefore runs this script twice - the first run produces those outputs,
# the second one packs them into the APK.

set -euo pipefail

# Fail loudly: with `set -e` an unguarded failing command exits silently, which
# makes build failures very hard to place (this bit us repeatedly).
trap 'echo "ERROR: build script failed at line $LINENO (exit $?)" >&2' ERR

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"

DEFAULT_VERSION="$(git -C "$REPO_DIR" describe --tags --dirty --always 2>/dev/null || echo pxp-local)"
VERSION_NAME="${1:-$DEFAULT_VERSION}"
if [ "$VERSION_NAME" = "--fresh" ]; then
    FRESH=1
    VERSION_NAME="${2:-$DEFAULT_VERSION}"
else
    FRESH=0
fi

ARCH_LIST="arm64-v8a"
ANDROID_BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-35.0.0}"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
if [ -z "${ANDROID_NDK_LATEST_HOME:-}" ]; then
    # only version-numbered directories; /opt/android-ndk may contain stray
    # files (e.g. wrap.sh) that sort after any version string
    ANDROID_NDK_LATEST_HOME="$(find /opt/android-ndk -maxdepth 1 -mindepth 1 -type d -name '[0-9]*' 2>/dev/null | sort -V | tail -1)"
fi
export ANDROID_SDK_ROOT ANDROID_NDK_LATEST_HOME

# Keep the packaging tree (and the built ICU/boost libs) between runs; the CI
# uses actions/cache for the same purpose. ICU compilation is the long pole.
BUILD_DIR="${ANDROID_BUILD_DIR:-$HOME/Projects/openttd-android-build}"
SDL="$BUILD_DIR/sdl-android"

APP_CFG="project/jni/application/openttd-jgrpp/AndroidAppSettings.cfg"

echo "== Toolchain =="
java -version
echo "SDK: $ANDROID_SDK_ROOT  NDK: $ANDROID_NDK_LATEST_HOME  build-dir: $BUILD_DIR"

if [ "$FRESH" = 1 ]; then
    echo "== Wiping $BUILD_DIR =="
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# --- 1. Check out pelya/commandergenius (Android packaging) -----------------
if [ ! -d "$SDL/project/jni" ]; then
    git clone --depth 1 -b sdl_android https://github.com/pelya/commandergenius.git "$SDL"
fi
cd "$SDL"
# jpeg/png modules are built from the sdl2_image submodule (recursively:
# their sources live in SDL_image's nested submodules)
git submodule update --init --recursive --depth=1 project/jni/iconv/src \
    project/jni/sdl2 project/jni/sdl2_image \
    project/jni/sdl2_mixer project/jni/sdl2_ttf

# --- 1b. Restore cached prebuilt dependencies (CI) --------------------------
# Building ICU and Boost takes about an hour per architecture, so the CI caches
# their outputs (see .github/workflows/android.yml) and hands them over in
# $ANDROID_PREBUILT_CACHE. Copying them in after the checkout is what makes this
# work: the copied files are newer than the just-checked-out sources, so
# `make -f Makefile.prebuilt` leaves them alone (pelya's CI does the same with
# its own cache, only there it has to touch the files explicitly). Only
# restored into a tree that has no ICU libraries yet, so a second run on the
# same tree does not overwrite what the first one built.
if [ -n "${ANDROID_PREBUILT_CACHE:-}" ] && [ -d "$ANDROID_PREBUILT_CACHE" ]; then
    if [ -f "project/jni/icuuc/lib/${ARCH_LIST%% *}/libicuuc.a" ]; then
        echo "== Prebuilt cache present, but this tree already has ICU libraries =="
    else
        for D in project/jni/iconv/include project/jni/iconv/lib \
                 project/jni/icuuc/include project/jni/icuuc/lib \
                 project/jni/icuuc/share project/obj; do
            if [ -e "$ANDROID_PREBUILT_CACHE/$D" ]; then
                # Copy the *contents* into place: the checkout may already have
                # these directories (and creating them again would nest the copy).
                mkdir -p "$D"
                cp -a "$ANDROID_PREBUILT_CACHE/$D/." "$D/"
                echo "== Restored $D from $ANDROID_PREBUILT_CACHE =="
            fi
        done
    fi
fi

# --- 2. Materialize libpng config header ------------------------------------
PNG_DIR="$SDL/project/jni/sdl2_image/external/libpng"
test -f "$PNG_DIR/png.h"
if [ ! -f "$PNG_DIR/pnglibconf.h" ]; then
    cp "$PNG_DIR/scripts/pnglibconf.h.prebuilt" "$PNG_DIR/pnglibconf.h" 2>/dev/null || \
        curl -fsSL https://raw.githubusercontent.com/pnggroup/libpng/v1.6.44/scripts/pnglibconf.h.prebuilt \
            -o "$PNG_DIR/pnglibconf.h"
fi
test -f "$PNG_DIR/pnglibconf.h"

# --- 3. Install this repo's source into the packaging tree ------------------
rm -rf "$SDL/project/jni/application/openttd-jgrpp/src"
mkdir -p "$SDL/project/jni/application/openttd-jgrpp/src"
# rsync instead of the CI's plain cp -r: skip local build dirs and junk
rsync -a --delete \
    --exclude .git --exclude .zcode --exclude build --exclude 'build-*' \
    --exclude 'repro-*' --exclude subagent-artifacts --exclude '*.bak' \
    "$REPO_DIR/" "$SDL/project/jni/application/openttd-jgrpp/src/"

# Upstream jgrpp uses O_RDWR/fdatasync in src/ini.cpp without including
# <fcntl.h>/<unistd.h>; glibc pulls those in transitively but bionic does not,
# so the Android build fails with "undeclared identifier 'O_RDWR'".
# Patch the copied source, not the repo.
INI_CPP="$SDL/project/jni/application/openttd-jgrpp/src/src/ini.cpp"
if ! grep -q '#include <fcntl.h>' "$INI_CPP"; then
    sed -i 's|^#include <fstream>$|#include <fstream>\n#include <fcntl.h>\n#include <unistd.h>|' "$INI_CPP"
fi

# Keep the game version clean. The copied tree has no .git, so jgrpp's
# FindVersion.cmake takes the version from the committed .ottdrev-vc (written
# when the release tag was made) and then verifies it: version_utils.sh hashes
# CMakeLists.txt + src/** and appends a "-H<hash>" suffix to the version when
# the hash differs. The bionic include patch above touches src/ini.cpp, so
# without this the in-game version of a tag build would be
# "pxp-2610.3-Hxxxxxxxx" instead of "pxp-2610.3". Recompute the hash over the
# patched tree - the patch only adds two includes and changes no behaviour -
# so the file stays self-consistent and the version stays the plain tag name.
SRC_TREE="$SDL/project/jni/application/openttd-jgrpp/src"
if [ -f "$SRC_TREE/.ottdrev-vc" ] && [ -f "$SRC_TREE/version_utils.sh" ]; then
    NEWHASH="$( cd "$SRC_TREE" && ./version_utils.sh -s )" || NEWHASH=""
    if [ -n "$NEWHASH" ]; then
        sed -i "2s|.*|$NEWHASH|" "$SRC_TREE/.ottdrev-vc"
        echo "== .ottdrev-vc source hash updated for the patched tree =="
    else
        echo "WARNING: could not recompute the .ottdrev-vc source hash;" \
             "the in-game version will carry a -H suffix" >&2
    fi
fi

# --- 3b. Game data package ---------------------------------------------------
# On first run the SDL wrapper unpacks the game's runtime data from
# AndroidData/openttd-data-<ver>.zip.xz (see AppDataDownloadUrl in the app
# settings) into the app's private dir — Android assets cannot be read as
# regular seekable files, so the data is shipped packed and extracted once.
# That archive is built by the app's pack-data.sh from an untracked data/ dir,
# so a fresh checkout has none and the APK would start to a black screen.
#
# The contents mirror the desktop distribution: OpenTTD's own build output
# (baseset/ incl. the fonts, lang/, ai/, game/, scripts/) ships in the package,
# but OpenGFX, the third-party base graphics set, does not. Set BUNDLE_OPENGFX=1
# to bundle OpenGFX and skip the in-game download prompt.
#
# CRITICAL — the data is versioned against the *code*, so it has to come from the
# same build as the native library: lang/*.lng carry LANGUAGE_PACK_VERSION (a hash
# of the string table) and baseset/openttd.grf carries the sprite layout. The
# archive is therefore assembled twice: here (the tree must be complete before
# build.sh touches it) and again in step 7 right after the native build, which
# regenerates both. Shipping the previous build's data makes OpenTTD reject every
# language pack on start and quit. assemble_game_data() is idempotent.
APP_DIR="$SDL/project/jni/application/openttd-jgrpp"
BUILD_ROOT="$APP_DIR/openttd-build-${ARCH_LIST%% *}"

assemble_game_data() {
    rm -rf "$APP_DIR/data"
    mkdir -p "$APP_DIR/data"
    local from_build=0
    for sub in baseset lang ai game scripts; do
        if [ -d "$BUILD_ROOT/$sub" ]; then
            cp -r "$BUILD_ROOT/$sub" "$APP_DIR/data/"
            from_build=1
        fi
    done
    if [ "$from_build" = 0 ]; then
        # No build outputs yet: fall back to a complete local installation. That
        # only makes a first run produce a bootable APK; the data belongs to
        # another revision, so step 7 re-assembles it once the native build has
        # produced the matching output.
        for d in /usr/share/openttd /usr/share/games/openttd "$HOME/.local/share/openttd"; do
            if [ -d "$d/baseset" ] && [ -n "$(ls -A "$d/baseset" 2>/dev/null)" ]; then
                echo "WARNING: no native build output yet; game data taken from $d." \
                     "It may not match this source; step 7 will refresh it." >&2
                for sub in baseset lang ai game scripts; do
                    [ -d "$d/$sub" ] && cp -r "$d/$sub" "$APP_DIR/data/"
                done
                break
            fi
        done
    fi

    if [ "${BUNDLE_OPENGFX:-0}" = 1 ]; then
        OGFX_DIR="${OPENTTD_OPENGFX_DIR:-}"
        if [ -z "$OGFX_DIR" ]; then
            for d in /usr/share/openttd/baseset/opengfx /usr/share/games/openttd/baseset/opengfx \
                     "$HOME/.local/share/openttd/baseset/opengfx"; do
                if [ -f "$d/opengfx.obg" ]; then
                    OGFX_DIR="$d"
                    break
                fi
            done
        fi
        if [ -n "$OGFX_DIR" ]; then
            echo "== Bundling OpenGFX from $OGFX_DIR =="
            mkdir -p "$APP_DIR/data/baseset/opengfx"
            cp -r "$OGFX_DIR/." "$APP_DIR/data/baseset/opengfx/"
        else
            echo "WARNING: BUNDLE_OPENGFX=1 but no OpenGFX found; set" \
                 "OPENTTD_OPENGFX_DIR to a directory containing opengfx.obg." >&2
        fi
    fi

    if [ ! -d "$APP_DIR/data/baseset" ] || [ -z "$(ls -A "$APP_DIR/data/baseset" 2>/dev/null)" ]; then
        echo "WARNING: no base data assembled; the APK will start to a black screen." >&2
        return
    fi

    # Tune the default config that gets written to .openttd/openttd.cfg on the
    # first run:
    #  * pelya's copy sets osk_activation = disabled, and OpenTTD only starts
    #    text input when that allows it - on the SDL2 path (no screen keyboard
    #    of its own) that also stops the soft keyboard from reaching the game.
    #    "double" is OpenTTD's own default (double-tap a text box to type).
    #  * fullscreen = true makes SDL enter immersive mode by itself, which is
    #    the more robust route for hiding the system bars.
    #  * scroll_mode = 3 is ViewportScrollMode::MapLMB: the map moves while the
    #    left mouse button is held. OpenTTD's default on Unix is the *right*
    #    button, which a finger cannot hold without also firing a right click
    #    (context menus), so a touch build has to start on MapLMB. TouchpadInput
    #    migrates an already existing config for the same reason.
    DEFAULT_CFG="$(ls "$APP_DIR"/AndroidData/openttd-jgr-*.cfg 2>/dev/null | head -1 || true)"
    if [ -n "$DEFAULT_CFG" ]; then
        sed -i 's/^osk_activation *=.*/osk_activation = double/' "$DEFAULT_CFG"
        grep -q '^fullscreen' "$DEFAULT_CFG" || sed -i '/^\[misc\]/a fullscreen = true' "$DEFAULT_CFG"
        grep -q '^scroll_mode' "$DEFAULT_CFG" || sed -i '/^\[gui\]/a scroll_mode = 3' "$DEFAULT_CFG"
        echo "== Default config: $(basename "$DEFAULT_CFG") =="
    fi
    # The wrapper looks the data up by file name and remembers that name in a flag
    # file beside the extracted files; while the name is unchanged it skips the
    # extraction entirely ("download_unneeded"), so a device that already unpacked
    # an older build keeps its old lang/ and baseset/ forever. The desktop has no
    # such cache: it reads lang/ and baseset/ straight from the run directory, which
    # every build replaces.
    #
    # Name the archive after the game version. Exactly at a clean tag that version is
    # the tag, and the data for a release is fixed, so the bare tag is enough. Any
    # other build appends a hash of the data: the version string alone does not change
    # for local edits (git describe only adds -dirty), and the name is what makes the
    # wrapper re-check the files (CRC) instead of trusting the extracted copy.
    if git -C "$REPO_DIR" describe --exact-match --tags HEAD >/dev/null 2>&1 &&
            [ -z "$(git -C "$REPO_DIR" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        DATA_VERSION="${VERSION_NAME}"
    else
        DATA_VERSION="${VERSION_NAME}-$(game_data_fingerprint | cut -c1-8)"
    fi
    sed -i "s@^VER=.*@VER=${DATA_VERSION}@" "$APP_DIR/pack-data.sh"
    sed -i "s@openttd-data-[^|\"]*\.zip\.xz@openttd-data-${DATA_VERSION}.zip.xz@g" \
        "$APP_DIR/AndroidAppSettings.cfg"

    # pack-data.sh zips data/ into AndroidData/openttd-data-<VER>.zip.xz
    ( cd "$APP_DIR" && ./pack-data.sh )
    ls -l "$APP_DIR/AndroidData/"
}

# Fingerprint of the assembled data/. Step 7 compares it before and after the
# native build to decide whether the APK has to be packaged again.
game_data_fingerprint() {
    { find "$APP_DIR/data" -type f -print0 2>/dev/null | sort -z | xargs -0 -r md5sum || true; } | md5sum
}

assemble_game_data
GAME_DATA_FINGERPRINT="$(game_data_fingerprint)"

# --- 3c. Android UI patches --------------------------------------------------
# See patches/android/README.md. On the SDL2 path pelya's template is a thin
# wrapper around stock SDL2 (no settings menu, no native on-screen keyboard),
# so the touch input, the on-screen modifier keys and the first-run data
# directory choice ship as three Java sources plus a small patch to
# project/javaSDL2/MainActivity.java.
# changeAppSettings.sh copies project/javaSDL2/*.java into project/src/ for
# compilation, so they have to be in place before it runs.
for F in ModifierKeysOverlay.java TouchpadInput.java DataDirPicker.java; do
    cp -f "$REPO_DIR/patches/android/$F" "$SDL/project/javaSDL2/$F"
done
# App icon: the OpenTTD logo instead of pelya's JGRPP one. Both
# res/drawable/icon.png and res/mipmap-nodpi/ic_launcher.png (what the manifest
# points at) are symlinks to this single file, so replacing it is enough.
cp -f "$REPO_DIR/patches/android/app-icon.png" "$APP_DIR/icon.png"
MAINACTIVITY="$SDL/project/javaSDL2/MainActivity.java"
UI_PATCH="$REPO_DIR/patches/android/javaSDL2-MainActivity.patch"
# Restore the file first, so the patch is always applied to the same revision it
# was written against. A "already patched?" marker cannot distinguish an older
# revision of the patch from the current one, which would silently skip updates.
if git -C "$SDL" rev-parse --git-dir >/dev/null 2>&1; then
    git -C "$SDL" checkout -- project/javaSDL2/MainActivity.java
fi
if patch -p1 -d "$SDL" --forward < "$UI_PATCH"; then
    echo "== Applied patches/android/javaSDL2-MainActivity.patch =="
else
    echo "ERROR: $UI_PATCH does not apply to $MAINACTIVITY any more." >&2
    echo "ERROR: upstream MainActivity.java changed; refresh the patch" >&2
    echo "ERROR: (see patches/android/README.md)." >&2
    exit 1
fi

# --- 4. Configure app settings ----------------------------------------------
sed -i "s/^MultiABI=.*/MultiABI='${ARCH_LIST}'/" "$APP_CFG"
sed -i "s/^AppVersionName=.*/AppVersionName=\"${VERSION_NAME}\"/" "$APP_CFG"
sed -i "s/^AppVersionCode=.*/AppVersionCode=7310/" "$APP_CFG"
# Rebrand: use pxp instead of jgrpp for the app name and package id.
# (Must run before changeAppSettings.sh, which regenerates the Java package
# tree and gradle applicationId from these values.)
sed -i 's/^AppName=.*/AppName="OpenTTD PXP"/; s/^AppFullName=.*/AppFullName=org.openttd.pxp/' "$APP_CFG"
# Enable the online content service so the first run can offer to download the
# base graphics set, like the desktop build does. curl is built by the
# template (module libcurl -> libcurl-sdl.so) and needs its TLS backend, so
# crypto+ssl (OpenSSL) come along; changeAppSettings.sh compiles OpenSSL
# whenever crypto or ssl appears in CompiledLibraries.
if ! grep -q '^CompiledLibraries=.*\bcurl\b' "$APP_CFG"; then
    sed -i 's/^\(CompiledLibraries=".*\)"/\1 crypto ssl curl"/' "$APP_CFG"
fi
# Drop the download entries we cannot satisfy. Anything prefixed with '!' is
# fetched automatically at first run, and DataDownloader.run() bails out
# (never calling initParent(), so the native thread never starts and the app
# just shows black) if such an entry fails. The ICU entry is obsolete now that
# the data is linked into libapplication.so; the MIDI music set is optional
# and not shipped. Re-add the timidity entry if you ever host that archive.
if grep -q 'Internationalization files' "$APP_CFG"; then
    sed -i 's@\^!!MIDI music support [^"^]*@@; s@\^!!Internationalization files[^"^]*@@' "$APP_CFG"
fi
# The base graphics are downloaded on first run, so the app needs network
# access (the SDL2 manifest template ignores this setting, see below).
sed -i 's/^AccessInternet=.*/AccessInternet=y/' "$APP_CFG"
ln -sfn openttd-jgrpp project/jni/application/src

# --- 5. Patch Java files and build Boost, ICU, and OpenSSL ------------------
# getifaddrs()/freeifaddrs() require Android API 24; pelya's template leaves
# APP_PLATFORM empty (NDK default android-21). Bump both the pelya ndk-build
# platform and the gradle minSdk to 24.
sed -i 's/^APP_PLATFORM=.*/APP_PLATFORM=android-24/' "$APP_CFG"
sed -i 's/minSdk 21/minSdk 24/' project/app/build-template.gradle
# jgrpp uses the SDL2 API; pelya's openttd-jgrpp template still targets his
# SDL-1.2 compat layer, and AndroidBuild.sh generates CMake variables only for
# that. Switch the packaging tree to SDL 2.0 (ndk module SDL2 -> libSDL2.so).
sed -i 's/^LibSdlVersion=.*/LibSdlVersion=2.0/' "$APP_CFG"
# Pelya's current gradle template wants compileSdk/targetSdk 37. Pin to 36
# (installed locally): otherwise gradle tries to auto-install android-37 into
# the root-owned SDK dir and dies with "SDK directory is not writable".
sed -i 's/compileSdk 37/compileSdk 36/; s/targetSdk 37/targetSdk 36/' project/app/build-template.gradle

# OpenSSL's compile.sh ends by copying the first ABI's include/ tree and
# rewriting the hardcoded 32-bit word size in opensslconf.h / bn_conf.h into an
# "#ifdef __LP64__" switch, so one header set serves both 32- and 64-bit ABIs.
# The bundled patch was generated from a 32-bit tree (pelya lists armeabi-v7a
# first); for a 64-bit-only ARCH_LIST the header is already the 64-bit variant,
# the hunks do not match, and `patch ... || exit 1` aborts the whole build even
# though nothing is wrong. Route it through a helper that accepts exactly that
# case. With any 32-bit ABI in the list the arch-neutral form is genuinely
# required, so the patch must still succeed there.
case " ${ARCH_LIST} " in
    *" armeabi-v7a "* | *" x86 "*)
        OPENSSL_ALL_64=0 ;;
    *)
        OPENSSL_ALL_64=1 ;;
esac
if [ "$OPENSSL_ALL_64" = 1 ]; then
    cat > /tmp/apply-opensslconf-patch.sh <<'EOF'
#!/bin/sh
# Apply OpenSSL's word-size patch, tolerating the redundant case: if the
# header already selects the 64-bit configuration (64-bit-only builds), the
# patch has nothing to do and its hunks cannot match.
if patch -p1 < opensslconf.h.patch; then exit 0; fi

rm -f include/openssl/opensslconf.h.rej include/crypto/bn_conf.h.rej
for F in include/openssl/opensslconf.h include/crypto/bn_conf.h; do
    grep -q '__LP64__' "$F" && continue
    grep -qE '^# *define SIXTY_FOUR_BIT_LONG' "$F" || exit 1
    grep -qE '^# *define THIRTY_TWO_BIT' "$F" && exit 1
done
exit 0
EOF
    for C in project/jni/openssl/compile.sh project/jni/crypto/compile.sh project/jni/ssl/compile.sh; do
        [ -f "$C" ] || continue
        cp -f /tmp/apply-opensslconf-patch.sh "$(dirname "$C")/apply-opensslconf-patch.sh"
        if ! grep -q 'apply-opensslconf-patch.sh' "$C"; then
            sed -i 's@^patch -p1 < opensslconf.h.patch.*@sh ./apply-opensslconf-patch.sh || exit 1@' "$C"
        fi
    done
fi

# The AccessInternet setting only affects the non-SDL2 manifest template
# (project/AndroidManifestTemplate.xml carries ==INTERNET== markers that
# changeAppSettings.sh strips when the value is 'n'). With SDL2 the manifest is
# generated from SDL2's own template instead, which has no INTERNET permission
# at all — so every socket() fails with EPERM, OpenTTD cannot initialise its
# network core (_network_available stays false) and HandleBootstrap() gives up
# instead of offering the base graphics download. Add the same pair of
# permissions pelya's template declares.
SDL2_MANIFEST=project/jni/sdl2/android-project/app/src/main/AndroidManifest.xml
if [ -f "$SDL2_MANIFEST" ] && ! grep -q 'permission.INTERNET' "$SDL2_MANIFEST"; then
    sed -i 's@^\([[:space:]]*\)<application @\1<uses-permission android:name="android.permission.INTERNET" />\n\1<uses-permission android:name="android.permission.ACCESS_LOCAL_NETWORK" />\n\1<application @' \
        "$SDL2_MANIFEST"
    grep -q 'permission.INTERNET' "$SDL2_MANIFEST" \
        || { echo "ERROR: could not add the INTERNET permission to $SDL2_MANIFEST" >&2; exit 1; }
fi

# MANAGE_EXTERNAL_STORAGE lets the player keep game data in a folder they pick
# (DataDirPicker), which needs real filesystem access to it on Android 11+.
# No marker on the line, so changeAppSettings.sh never strips it.
if ! grep -q 'permission.MANAGE_EXTERNAL_STORAGE' "$SDL2_MANIFEST"; then
    sed -i 's@^\([[:space:]]*\)<application @\1<uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />\n\1<application @' \
        "$SDL2_MANIFEST"
    grep -q 'permission.MANAGE_EXTERNAL_STORAGE' "$SDL2_MANIFEST" \
        || { echo "ERROR: could not add MANAGE_EXTERNAL_STORAGE to $SDL2_MANIFEST" >&2; exit 1; }
fi

# Screen orientation. changeAppSettings.sh rewrites
# android:screenOrientation to $ScreenOrientation1, but only if the attribute
# already exists — and SDL2's template has none, so ScreenOrientation=h in the
# app settings was silently ignored and the app followed the device rotation.
# Add the attribute (value is a placeholder; the sed below sets the real one).
if ! grep -q 'android:screenOrientation' "$SDL2_MANIFEST"; then
    sed -i 's@^\([[:space:]]*\)<activity android:name="SDLActivity"@\1<activity android:name="SDLActivity"\n\1    android:screenOrientation="sensorLandscape"@' \
        "$SDL2_MANIFEST"
    grep -q 'android:screenOrientation' "$SDL2_MANIFEST" \
        || { echo "ERROR: could not add android:screenOrientation to $SDL2_MANIFEST" >&2; exit 1; }
fi

# The manifest attribute alone is not enough: OpenTTD creates a 640x480 SDL
# window, and SDL2's Android backend then calls setRequestedOrientation() with
# whatever SDL_GetHint(SDL_HINT_ORIENTATIONS) returns. With no hint it asks for
# FULL_USER, which overrides android:screenOrientation and lets the device
# rotation take over - the app flipped to portrait and rendered into a
# 1080x2340 surface mid-session (half the screen black). The hint's string value
# is "SDL_IOS_ORIENTATIONS", and SDLActivity.getManifestEnvironmentVariables()
# turns every <meta-data android:name="SDL_ENV.*"> into that environment
# variable, which SDL_GetHint() falls back to. Must live inside <application>.
case "$(sed -n 's/^ScreenOrientation=\(.\).*/\1/p' "$APP_CFG")" in
    h|H|l|L) SDL_ORIENTATION_HINT="LandscapeLeft LandscapeRight" ;;
    *)       SDL_ORIENTATION_HINT="Portrait PortraitUpsideDown" ;;
esac
if grep -q 'SDL_ENV.SDL_IOS_ORIENTATIONS' "$SDL2_MANIFEST"; then
    sed -i "s@\(android:name=\"SDL_ENV.SDL_IOS_ORIENTATIONS\" android:value=\"\)[^\"]*@\1$SDL_ORIENTATION_HINT@" "$SDL2_MANIFEST"
else
    sed -i "s@^\([[:space:]]*\)<activity @\1<meta-data android:name=\"SDL_ENV.SDL_IOS_ORIENTATIONS\" android:value=\"$SDL_ORIENTATION_HINT\" />\n\1<activity @" \
        "$SDL2_MANIFEST"
fi
grep -q 'SDL_ENV.SDL_IOS_ORIENTATIONS' "$SDL2_MANIFEST" \
    || { echo "ERROR: could not add the SDL_ENV.SDL_IOS_ORIENTATIONS hint to $SDL2_MANIFEST" >&2; exit 1; }

# Turn off heap pointer tagging for this app. On arm64 Android 11+ bionic puts a
# tag in the top byte of heap pointers (0xB4) and aborts in free() when it finds
# the tag stripped ("F libc: Pointer tag for 0x... was truncated"), which is what
# killed the game after the base graphics set had been fetched. Some library in
# the stack stores a pointer in a field too narrow to keep the top byte; this
# attribute is Android's supported opt-out (it needs targetSdk >= 30, and is
# simply ignored on older releases). Drop this line once the offending library is
# identified - `adb logcat -b crash` gives the tombstone that names it.
if ! grep -q 'allowNativeHeapPointerTagging' "$SDL2_MANIFEST"; then
    sed -i '/^[[:space:]]*<application /a\        android:allowNativeHeapPointerTagging="false"' "$SDL2_MANIFEST"
    grep -q 'allowNativeHeapPointerTagging' "$SDL2_MANIFEST" \
        || { echo "ERROR: could not add allowNativeHeapPointerTagging to $SDL2_MANIFEST" >&2; exit 1; }
fi

export PATH="$ANDROID_NDK_LATEST_HOME:$PATH"
./changeAppSettings.sh

# changeAppSettings.sh compiles OpenSSL (into project/jni/openssl/lib/<abi>/),
# and Makefile.prebuilt copies those into obj/local/. The ndk-build modules
# named crypto/ssl read from their own dirs, so make sure they are populated
# too: the app's libcurl module links ssl and crypto, and is only defined at
# all when ssl appears in APP_MODULES. In the template these paths already
# resolve to the same files (openssl/lib, crypto/lib and ssl/lib share the
# inode), so this must not copy onto itself.
for A in ${ARCH_LIST}; do
    for D in crypto ssl; do
        SRC="project/jni/openssl/lib/$A/lib$D.so.sdl.1.so"
        DST="project/jni/$D/lib/$A/lib$D.so.sdl.1.so"
        if [ -f "$SRC" ] && [ ! -e "$DST" ]; then
            mkdir -p "project/jni/$D/lib/$A"
            cp -f "$SRC" "$DST"
        fi
        if [ -d project/jni/openssl/include ] && [ ! -d "project/jni/$D/include" ]; then
            cp -r project/jni/openssl/include "project/jni/$D/include"
        fi
    done
done
ls project/jni/openssl/lib/*/ 2>/dev/null | head -4 || true

# The actual openttd native lib is built by CMake, not pelya's ndk-build.
# AndroidBuild.sh never sets ANDROID_PLATFORM, so CMake defaults to android-21
# and fails with "undeclared identifier 'getifaddrs'". Inject it.
if ! grep -q 'ANDROID_PLATFORM=android-24' project/jni/application/openttd-jgrpp/AndroidBuild.sh; then
    awk '1; /^[[:space:]]*cmake[[:space:]]/{print "\t-DANDROID_PLATFORM=android-24 \\"}' \
        project/jni/application/openttd-jgrpp/AndroidBuild.sh > /tmp/ab.tmp \
        && mv /tmp/ab.tmp project/jni/application/openttd-jgrpp/AndroidBuild.sh \
        && chmod +x project/jni/application/openttd-jgrpp/AndroidBuild.sh
fi
# AndroidBuild.sh derives the SDL2 include dir from the ndk module name
# (SDL2), but the on-disk dir is lowercase sdl2. Fix the generated
# AndroidSDL.cmake (regenerated on every configure, so patch the generator).
if ! grep -q '/SDL2/include@/sdl2/include' project/jni/application/openttd-jgrpp/AndroidBuild.sh; then
    # The outer delimiter has to be a character that appears neither in the
    # pattern nor in the replacement - the replacement contains '@' (the inner
    # sed) and '#' (the comment), so '|' it is.
    sed -i 's|^\tcmake \\$|\t# Module name SDL2 vs on-disk dir sdl2: fix generated include path.\n\tsed -i "s@/SDL2/include@/sdl2/include@" $CMAKE_SDL\n\tcmake \\|' \
        project/jni/application/openttd-jgrpp/AndroidBuild.sh
    # The pattern depends on the exact shape of upstream's cmake invocation; if
    # it stops matching, fail here instead of shipping the include-path bug.
    if ! grep -q '/SDL2/include@/sdl2/include' project/jni/application/openttd-jgrpp/AndroidBuild.sh; then
        echo "ERROR: could not patch the SDL2 include path into AndroidBuild.sh (upstream changed?)" >&2
        exit 1
    fi
fi
# jgrpp's CMake links the `openttd` target as a PIE executable, but pelya's
# Android runtime loads the game as a shared library (libapplication.so) and
# resolves its entry point by name: SDLActivity.java asks for "SDL_main".
# OpenTTD's source defines a plain main() (src/os/unix/unix_main.cpp) and no
# SDL_main at all, so relink-app.sh links the CMake objects into a shared
# library together with a small wrapper exporting SDL_main.
cat > project/jni/application/openttd-jgrpp/sdl_main_wrapper.cpp <<'EOF'
/*
 * pelya's SDL wrapper looks the application entry point up by name and asks
 * for "SDL_main" (see SDLActivity.java, getMainFunction). OpenTTD defines a
 * normal main(), so export the name the wrapper expects as well. Both symbols
 * are global and default-visibility, hence also in the shared library's
 * dynamic symbol table where dlsym() can find them.
 */
extern int main(int argc, char **argv);

extern "C" int SDL_main(int argc, char **argv)
{
	return main(argc, argv);
}
EOF
cat > project/jni/application/openttd-jgrpp/relink-app.sh <<'EOF'
#!/bin/sh
# usage: relink-app.sh <build-dir> <arch>; run from the app dir
# The link itself runs with the build dir as working directory, so resolve
# every path to an absolute one first: a relative build dir would make the
# injected object path resolve against the wrong directory.
APP_DIR="$(pwd)"
BUILD_DIR="$(cd "$1" && pwd)" || exit 1
ARCH="$2"
WRAPPER_SRC="$APP_DIR/sdl_main_wrapper.cpp"
WRAPPER_OBJ="$BUILD_DIR/sdl_main_wrapper.o"

if [ ! -f "$WRAPPER_OBJ" ] && [ -f "$WRAPPER_SRC" ]; then
    # the cross compiler, as set up by pelya's setEnvironment-<arch>.sh
    CXX="$(sh -c ". ../setEnvironment-$ARCH.sh true >/dev/null 2>&1; echo \$CXX" 2>/dev/null)"
    [ -n "$CXX" ] || CXX=clang++
    "$CXX" -fPIC -c "$WRAPPER_SRC" -o "$WRAPPER_OBJ" || exit 1
fi
if [ ! -f "$WRAPPER_OBJ" ]; then
    echo "ERROR: $WRAPPER_OBJ was not built; libapplication.so would have no SDL_main entry point" >&2
    exit 1
fi

# pelya's CustomBuildScript.mk requires SONAME == libapplication.so
soname_ok() { objdump -p "$1" 2>/dev/null | grep -q 'SONAME *libapplication\.so'; }
# ...and SDLActivity.java needs the SDL_main entry point to be exported.
has_sdl_main() { objdump -T "$1" 2>/dev/null | grep -q 'SDL_main'; }
# Relink when the lib is missing, has a wrong SONAME, lacks SDL_main, or cmake
# produced a newer `openttd` than the last relink (e.g. after a recompile).
if [ ! -f "$BUILD_DIR/libapplication.so" ] || ! soname_ok "$BUILD_DIR/libapplication.so" \
        || ! has_sdl_main "$BUILD_DIR/libapplication.so" \
        || [ "$BUILD_DIR/openttd" -nt "$BUILD_DIR/libapplication.so" ]; then
    sed -e "s| -o openttd | $WRAPPER_OBJ -shared -Wl,-soname=libapplication.so -o libapplication.so |" \
        -e 's/-rdynamic //' \
        "$BUILD_DIR/CMakeFiles/openttd.dir/link.txt" > "$BUILD_DIR/link-app.sh" || exit 1
    ( cd "$BUILD_DIR" && sh link-app.sh ) || exit 1
fi
cp -f "$BUILD_DIR/libapplication.so" "libapplication-$ARCH.so" || exit 1
EOF
chmod +x project/jni/application/openttd-jgrpp/relink-app.sh
if ! grep -q 'relink-app.sh' project/jni/application/openttd-jgrpp/AndroidBuild.sh; then
    sed -i 's|cp -f openttd-\$VER-\$1/libapplication.so libapplication-\$1\.so|./relink-app.sh openttd-$VER-$1 $1|' \
        project/jni/application/openttd-jgrpp/AndroidBuild.sh
fi

# pelya's AndroidSDL.cmake variables are consumed by his own OpenTTD fork; an
# unadapted jgrpp CMakeLists cannot see them (every find_package fails and the
# configure dies on "SDL2 or Allegro is required"). Generate self-contained
# Find modules into the CMAKE_MODULE_PATH dir that map the prebuilt ndk
# libraries onto the variables/targets jgrpp's CMakeLists expects. The paths
# are relative to the src dir and arch comes from ANDROID_ABI, so they work
# for any ABI.
FINDMOD_DIR="project/jni/application/openttd-jgrpp/openttd-build-arm64-v8a/cmake"
mkdir -p "$FINDMOD_DIR"
cat > "$FINDMOD_DIR/FindSDL2.cmake" <<'EOF'
set(SDL2_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../..//sdl2/include")
set(SDL2_LIBRARY "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libSDL2.so")
if(EXISTS "${SDL2_LIBRARY}" AND EXISTS "${SDL2_INCLUDE_DIR}/SDL.h")
    set(SDL2_FOUND TRUE)
    if(NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 UNKNOWN IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES IMPORTED_LOCATION "${SDL2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}")
    endif()
endif()
EOF
cat > "$FINDMOD_DIR/FindFreetype.cmake" <<'EOF'
set(FREETYPE_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/../../..//freetype/include")
set(FREETYPE_LIBRARY "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libfreetype.a")
if(EXISTS "${FREETYPE_LIBRARY}" AND EXISTS "${FREETYPE_INCLUDE_DIRS}/ft2build.h")
    # CMakeLists.txt gates find_package(Fontconfig) behind the mixed-case
    # ${Freetype_FOUND} while link_package(FREETYPE ...) tests the upper-case
    # spelling, so both have to be set or fontconfig is never searched and
    # WITH_FONTCONFIG stays undefined (which silently disables OpenTTD's
    # first-run "download base graphics" UI on Android).
    set(FREETYPE_FOUND TRUE)
    set(Freetype_FOUND TRUE)
    set(Freetype_INCLUDE_DIRS "${FREETYPE_INCLUDE_DIRS}")
    set(Freetype_LIBRARIES "${FREETYPE_LIBRARY}")
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype UNKNOWN IMPORTED)
        set_target_properties(Freetype::Freetype PROPERTIES IMPORTED_LOCATION "${FREETYPE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${FREETYPE_INCLUDE_DIRS}")
    endif()
endif()
EOF
cat > "$FINDMOD_DIR/FindHarfbuzz.cmake" <<'EOF'
set(Harfbuzz_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/../../..//harfbuzz/include")
set(Harfbuzz_LIBRARIES "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libharfbuzz.a")
set(Harfbuzz_LIBRARY "${Harfbuzz_LIBRARIES}")
if(EXISTS "${Harfbuzz_LIBRARY}" AND EXISTS "${Harfbuzz_INCLUDE_DIRS}/harfbuzz/hb.h")
    set(Harfbuzz_FOUND TRUE)
    if(NOT TARGET harfbuzz::harfbuzz)
        add_library(harfbuzz::harfbuzz UNKNOWN IMPORTED)
        set_target_properties(harfbuzz::harfbuzz PROPERTIES IMPORTED_LOCATION "${Harfbuzz_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Harfbuzz_INCLUDE_DIRS}")
    endif()
endif()
EOF
cat > "$FINDMOD_DIR/FindFontconfig.cmake" <<'EOF'
set(FONTCONFIG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../..//fontconfig/include")
set(FONTCONFIG_LIBRARY "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libfontconfig.a"
    "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libexpat-sdl.so")
if(EXISTS "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libfontconfig.a"
        AND EXISTS "${FONTCONFIG_INCLUDE_DIR}/fontconfig/fontconfig.h")
    # link_package(Fontconfig ...) and the WITH_FONTCONFIG define test
    # ${Fontconfig_FOUND}, not the upper-case spelling, so set both.
    set(Fontconfig_FOUND TRUE)
    set(FONTCONFIG_FOUND TRUE)
    set(Fontconfig_INCLUDE_DIRS "${FONTCONFIG_INCLUDE_DIR}")
    set(Fontconfig_LIBRARIES "${FONTCONFIG_LIBRARY}")
    if(NOT TARGET Fontconfig::Fontconfig)
        add_library(Fontconfig::Fontconfig UNKNOWN IMPORTED)
        set_target_properties(Fontconfig::Fontconfig PROPERTIES
            IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libfontconfig.a"
            INTERFACE_INCLUDE_DIRECTORIES "${FONTCONFIG_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libexpat-sdl.so")
    endif()
endif()
EOF
cat > "$FINDMOD_DIR/FindCURL.cmake" <<'EOF'
# pelya builds curl as a shared lib named libcurl-sdl (module name is libcurl,
# LOCAL_MODULE_FILENAME overrides the output name) into obj/local/. OpenTTD's
# src/network/core/CMakeLists.txt picks http_curl.cpp over the http_none.cpp
# stub purely on CURL_FOUND, which is what enables the content service
# (base graphics download on first run).
set(CURL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../..//curl/include")
set(CURL_LIBRARY "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libcurl-sdl.so")
set(CURL_INCLUDE_DIRS "${CURL_INCLUDE_DIR}")
set(CURL_LIBRARIES "${CURL_LIBRARY}")
if(EXISTS "${CURL_LIBRARY}" AND EXISTS "${CURL_INCLUDE_DIR}/curl/curl.h")
    set(CURL_FOUND TRUE)
endif()
EOF
cat > "$FINDMOD_DIR/FindLibLZMA.cmake" <<'EOF'
set(LIBLZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../..//lzma/include")
set(LIBLZMA_LIBRARY "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/liblzma.so")
set(LIBLZMA_INCLUDE_DIRS "${LIBLZMA_INCLUDE_DIR}")
set(LIBLZMA_LIBRARIES "${LIBLZMA_LIBRARY}")
set(LIBLZMA_HAS_AUTO_DECODER TRUE)
set(LIBLZMA_HAS_EASY_ENCODER TRUE)
set(LIBLZMA_HAS_LZMA_PRESET TRUE)
if(EXISTS "${LIBLZMA_LIBRARY}" AND EXISTS "${LIBLZMA_INCLUDE_DIR}/lzma.h")
    set(LIBLZMA_FOUND TRUE)
    if(NOT TARGET LibLZMA::LibLZMA)
        add_library(LibLZMA::LibLZMA UNKNOWN IMPORTED)
        set_target_properties(LibLZMA::LibLZMA PROPERTIES IMPORTED_LOCATION "${LIBLZMA_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBLZMA_INCLUDE_DIR}")
    endif()
endif()
EOF
cat > "$FINDMOD_DIR/FindLZO.cmake" <<'EOF'
set(LZO_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../..//lzo2/include")
set(LZO_LIBRARY "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/liblzo2.so")
if(EXISTS "${LZO_LIBRARY}" AND EXISTS "${LZO_INCLUDE_DIR}/lzo/lzo1x.h")
    set(LZO_FOUND TRUE)
endif()
EOF
cat > "$FINDMOD_DIR/FindPNG.cmake" <<'EOF'
set(PNG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../..//png/include")
set(PNG_LIBRARY "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}/libpng.a")
if(EXISTS "${PNG_LIBRARY}" AND EXISTS "${PNG_INCLUDE_DIR}/png.h")
    set(PNG_FOUND TRUE)
    if(NOT TARGET PNG::PNG)
        add_library(PNG::PNG UNKNOWN IMPORTED)
        set_target_properties(PNG::PNG PROPERTIES IMPORTED_LOCATION "${PNG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PNG_INCLUDE_DIR}")
    endif()
endif()
EOF
cat > "$FINDMOD_DIR/FindICU.cmake" <<'EOF'
set(ICU_UC_LIBDIR "${CMAKE_SOURCE_DIR}/../../../..//obj/local/${ANDROID_ABI}")
set(ICU_UC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/../../..//icuuc/include")
set(ICU_I18N_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/../../..//icui18n/include")
set(ICU_UC_LIBRARY "${ICU_UC_LIBDIR}/libicuuc.a" "${ICU_UC_LIBDIR}/libicudata.a")
set(ICU_I18N_LIBRARY "${ICU_UC_LIBDIR}/libicui18n.a" "${ICU_UC_LIBDIR}/libicu-le-hb.a"
    "${ICU_UC_LIBDIR}/libharfbuzz.a" "${ICU_UC_LIBRARY}")
if(EXISTS "${ICU_UC_LIBDIR}/libicuuc.a")
    set(ICU_UC_FOUND TRUE)
    if(NOT TARGET ICU::uc)
        add_library(ICU::uc UNKNOWN IMPORTED)
        set_target_properties(ICU::uc PROPERTIES IMPORTED_LOCATION "${ICU_UC_LIBDIR}/libicuuc.a"
            INTERFACE_INCLUDE_DIRECTORIES "${ICU_UC_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${ICU_UC_LIBDIR}/libicudata.a"
            # Static ICU: without this udata looks for a shared icudata at
            # runtime, finds none on Android, and the first ICU user (a global
            # Textbuf built while dlopening libapplication.so) segfaults.
            INTERFACE_COMPILE_DEFINITIONS "U_STATIC_IMPLEMENTATION")
    endif()
endif()
if(EXISTS "${ICU_UC_LIBDIR}/libicui18n.a")
    set(ICU_I18N_FOUND TRUE)
    if(NOT TARGET ICU::i18n)
        add_library(ICU::i18n UNKNOWN IMPORTED)
        set_target_properties(ICU::i18n PROPERTIES IMPORTED_LOCATION "${ICU_UC_LIBDIR}/libicui18n.a"
            INTERFACE_INCLUDE_DIRECTORIES "${ICU_I18N_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "ICU::uc;${ICU_UC_LIBDIR}/libicu-le-hb.a;${ICU_UC_LIBDIR}/libharfbuzz.a"
            INTERFACE_COMPILE_DEFINITIONS "U_STATIC_IMPLEMENTATION")
    endif()
endif()
set(ICU_LIBRARIES "${ICU_I18N_LIBRARY}" "${ICU_UC_LIBRARY}")
set(ICU_INCLUDE_DIRS "${ICU_I18N_INCLUDE_DIRS}" "${ICU_UC_INCLUDE_DIRS}")
set(ICU_FOUND TRUE)
set(ICU_VERSION "75.1")
EOF

# pelya builds harfbuzz into iconv/src/<arch>/lib/, which the CI caches
# separately; preserve it inside the icuuc/lib tree so freetype can link hb_*.
for A in ${ARCH_LIST}; do
    SRC="project/jni/iconv/src/$A/lib/libharfbuzz.a"
    DST="project/jni/icuuc/lib/$A/libharfbuzz.a"
    if [ -f "$SRC" ]; then
        mkdir -p "project/jni/icuuc/lib/$A"
        cp -f "$SRC" "$DST"
    fi
done
ls project/jni/icuuc/lib/*/libharfbuzz.a

# pelya builds ICU with --with-data-packaging=archive, which installs a stub
# libicudata.a (~4KB of stubdata) instead of the real ICU data. At runtime
# udata's header check then fails, BreakIterator::create*Instance returns
# NULL, and the first IcuStringIterator::SetString — a global Textbuf
# constructed while dlopening libapplication.so — dereferences it and
# segfaults. Convert the real icudt62l.dat into an object carrying the
# symbol udata looks for (icudt62_dat) and rebuild libicudata.a from it.
GENCCODE="$(ls project/jni/iconv/src/*/icu/source/cross/bin/genccode 2>/dev/null | head -1 || true)"
NDKCLANG="$(ls "$ANDROID_NDK_LATEST_HOME"/toolchains/llvm/prebuilt/*/bin/clang 2>/dev/null | head -1 || true)"
NDKAR="$(ls "$ANDROID_NDK_LATEST_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-ar 2>/dev/null | head -1 || true)"
NDKNM="$(ls "$ANDROID_NDK_LATEST_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-nm 2>/dev/null | head -1 || true)"
for A in ${ARCH_LIST}; do
    LIBICUDATA="project/jni/icuuc/lib/$A/libicudata.a"
    DAT="$(ls project/jni/iconv/src/$A/icu/source/data/out/icudt62*.dat 2>/dev/null | head -1 || true)"
    OBJDIR="project/jni/iconv/src/$A/icudata"
    OBJ="$OBJDIR/icudt62_dat.o"

    # 1. object providing the real data under the expected symbol.
    #    genccode appends "_dat" to the -e argument (pkg_genc.cpp), and udata
    #    references U_ICUDATA_ENTRY_POINT == icudt62_dat, so -e takes icudt62.
    #    (Passing -e icudt62_dat yields icudt62_dat_dat, which nothing links to.)
    if [ -n "$NDKNM" ] && ! ( [ -f "$OBJ" ] && "$NDKNM" -g "$OBJ" 2>/dev/null | grep -qE ' icudt62_dat$' ); then
        rm -f "$OBJDIR"/icudt62_dat.S "$OBJ"
        mkdir -p "$OBJDIR"
        if [ -f "$DAT" ] && [ -x "$GENCCODE" ]; then
            # -a gcc: emit assembly (.S with .balign 16 in .rodata). The
            # default C output is an ~80MB array that clang cannot compile
            # reliably (it was killed silently by the OOM killer).
            # genccode links the host ICU libs from the cross-build directory.
            LD_LIBRARY_PATH="$(dirname "$GENCCODE")/../lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                "$GENCCODE" -a gcc -e icudt62 -f icudt62_dat -d "$OBJDIR" "$DAT" || true
        fi
    fi
    if [ ! -f "$OBJ" ] && [ -d "$OBJDIR" ]; then
        ASRC="$(ls "$OBJDIR"/*.S 2>/dev/null | head -1 || true)"
        [ -n "$ASRC" ] && "$NDKCLANG" --target=aarch64-linux-android24 -c "$ASRC" -o "$OBJ"
    fi

    # 2. archive containing only our object. `ar r` would merely add to the
    #    archive, leaving stubdata.ao behind — the linker then resolves
    #    icudt62_dat from that stub (smaller member, first in the symbol map).
    if [ -f "$OBJ" ]; then
        if [ ! -f "$LIBICUDATA" ] \
                || [ "$(ar t "$LIBICUDATA" 2>/dev/null | wc -l)" != "1" ] \
                || [ "$(ar t "$LIBICUDATA" 2>/dev/null | head -1)" != "icudt62_dat.o" ] \
                || [ "$(stat -c%s "$LIBICUDATA")" -lt 1000000 ]; then
            rm -f "$LIBICUDATA"
            "$NDKAR" rcs "$LIBICUDATA" "$OBJ"
            echo "ICU data: rebuilt libicudata.a with real data for $A"
        fi
    fi

    # 3. ndk-build won't re-copy an "up to date" prebuilt module, so keep its
    #    copy under obj/local/ in sync — the final link uses that one. Any
    #    change there invalidates binaries previously linked against the stub.
    #    (obj/local/<abi> normally appears only when ndk-build runs in step 7,
    #    so it does not exist yet on a fresh tree - create it.)
    OBJARCHIVE="project/obj/local/$A/libicudata.a"
    if [ -f "$LIBICUDATA" ] && [ "$(stat -c%s "$LIBICUDATA")" -ge 1000000 ]; then
        if [ ! -f "$OBJARCHIVE" ] || ! cmp -s "$LIBICUDATA" "$OBJARCHIVE"; then
            mkdir -p "$(dirname "$OBJARCHIVE")"
            cp -f "$LIBICUDATA" "$OBJARCHIVE"
            rm -f project/jni/application/openttd-jgrpp/openttd-build-*/libapplication.so \
                  project/jni/application/openttd-jgrpp/libapplication-*.so \
                  project/libs/*/libapplication.so
            echo "ICU data: refreshed obj/local copy for $A (forcing relink)"
        fi
    fi

    # The archive the final link uses must carry the real data. If it is still
    # the ~4KB stub (or was never built), fail here: the APK would start and
    # then crash inside the first IcuStringIterator::SetString, which is much
    # harder to diagnose than a failed build.
    if [ ! -f "$OBJARCHIVE" ] || [ "$(stat -c%s "$OBJARCHIVE")" -lt 1000000 ]; then
        echo "ERROR: $OBJARCHIVE is missing or is the ICU data stub." >&2
        echo "       The real icudt62 data could not be produced:" >&2
        echo "       dat file: ${DAT:-not found}" >&2
        echo "       genccode: ${GENCCODE:-not found}" >&2
        exit 1
    fi
done

# --- 6. Gradle, SDK licenses, debug keystore --------------------------------
cd project
# The SDK is usually root-owned in CI, where sudo is available; locally it may
# be writable already, in which case sudo would just prompt for a password.
SDKMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
if [ -w "$ANDROID_SDK_ROOT" ] || [ "$(id -u)" = 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi
for Y in $(seq 20); do echo y; done | $SUDO "$SDKMANAGER" --licenses
./gradlew assembleRelease || true
mkdir -p "$HOME/.android"
keytool -genkey -v -keystore "$HOME/.android/debug.keystore" -storepass android \
    -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Debug, OU=Debug, O=Debug, L=Debug, ST=Debug, C=Debug" 2>/dev/null || true
echo "sdk.dir=$ANDROID_SDK_ROOT" > local.properties
echo "proguard.config=proguard.cfg;proguard-local.cfg" >> local.properties

# --- 7. Build APK ------------------------------------------------------------
cd "$SDL"
export PATH="$ANDROID_NDK_LATEST_HOME:$ANDROID_SDK_ROOT/build-tools/$ANDROID_BUILD_TOOLS:$PATH"
./build.sh
# build.sh first compiles the native library — which regenerates lang/*.lng and
# baseset/openttd.grf in the build output — and only then packs the APK with the
# data/ assembled in step 3b, i.e. the *previous* build's data. That data no
# longer matches the code whenever the string table or the sprites changed (a new
# LANGUAGE_PACK_VERSION makes OpenTTD reject every language pack and quit on
# start). Re-assemble from the output just built and repackage if it differs.
assemble_game_data
if [ "$(game_data_fingerprint)" != "$GAME_DATA_FINGERPRINT" ]; then
    echo "== Game data changed after the native build; repackaging the APK =="
    ./build.sh
fi

# --- 8. Sign ------------------------------------------------------------------
mkdir -p "$REPO_DIR/upload"
./sign.sh
OUT_APK="$(ls -t *.apk 2>/dev/null | head -1 || true)"
if [ -z "$OUT_APK" ]; then
    echo "ERROR: no APK produced; check the build output above" >&2
    exit 1
fi
cp -f "$OUT_APK" "$REPO_DIR/upload/"

# --- 9. Verify the packaged manifest -----------------------------------------
# Cheap pre-install check: these are the settings whose plumbing proved easy to
# lose silently (they only reach the manifest when the right template is patched).
AAPT2="$(ls "$ANDROID_SDK_ROOT"/build-tools/"$ANDROID_BUILD_TOOLS"/aapt2 2>/dev/null | head -1 || true)"
if [ -n "$AAPT2" ]; then
    PERMS="$("$AAPT2" dump permissions "$REPO_DIR/upload/$OUT_APK" 2>/dev/null || true)"
    XMLTREE="$("$AAPT2" dump xmltree --file AndroidManifest.xml "$REPO_DIR/upload/$OUT_APK" 2>/dev/null || true)"
    for P in android.permission.INTERNET android.permission.MANAGE_EXTERNAL_STORAGE; do
        case "$PERMS" in
            *"$P"*) ;;
            *) echo "ERROR: $P missing from the APK manifest" >&2; exit 1 ;;
        esac
    done
    case "$XMLTREE" in
        *screenOrientation*) ;;
        *) echo "ERROR: no screenOrientation in the APK manifest (landscape lock lost)" >&2; exit 1 ;;
    esac
    echo "== Manifest check OK: INTERNET, MANAGE_EXTERNAL_STORAGE, screenOrientation =="
else
    echo "WARNING: aapt2 not found; skipping the manifest check" >&2
fi

echo
echo "== Done =="
echo "APK: $REPO_DIR/upload/$OUT_APK"
