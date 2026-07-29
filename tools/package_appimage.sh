#!/usr/bin/env bash
# Package the app as a single-file, self-contained Linux AppImage in downloads/.
#
#   tools/package_appimage.sh [--skip-build]
#
# What it does
#   1. Release build (build-appimage/, separate tree so it cannot inherit the
#      Debug/coverage/sanitizer flags of the others),
#   2. `cmake --install` into an AppDir with the /usr prefix layout AppImages
#      expect, plus packaging/TradingApp.desktop and packaging/tradingapp.png,
#   3. linuxdeploy + its Qt plugin bundle the Qt libraries, the xcb platform
#      plugin, the TLS backend the eToro API needs and the image formats,
#   4. output: downloads/TradingApp-<version>-x86_64.AppImage
#
# The Qt kit is taken from QT_PREFIX, else the newest ~/Qt/*/gcc_64 that has the
# Charts module — a kit installed without Charts cannot build this app, and
# picking "newest" blindly is how you get a configure error instead of a package.
# The AppImage tooling comes from tools/fetch_linuxdeploy.sh (run by ./setup.sh,
# or on demand here).
#
# Glibc: an AppImage is only as portable as the glibc it was linked against, so
# a build on Ubuntu 24.04 needs glibc >= 2.39 on the target machine. The release
# workflow (.github/workflows/release.yml) therefore builds on ubuntu-22.04.
# OpenSSL is deliberately NOT bundled: Qt loads the system libssl at run time,
# and shipping a copy would freeze its CVE state into the download.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKIP_BUILD=0
for a in "$@"; do
    case "$a" in
    --skip-build) SKIP_BUILD=1 ;;
    *)
        echo "unknown argument: $a" >&2
        exit 2
        ;;
    esac
done

BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"
OUT_DIR="$ROOT/downloads"
JOBS="$(nproc)"

if [ -z "${QT_PREFIX:-}" ]; then
    for kit in $(ls -d "$HOME"/Qt/*/gcc_64 2>/dev/null | sort -Vr); do
        if [ -d "$kit/lib/cmake/Qt6Charts" ]; then
            QT_PREFIX="$kit"
            break
        fi
    done
fi
if [ -z "${QT_PREFIX:-}" ] || [ ! -d "$QT_PREFIX" ]; then
    echo "no Qt kit with the Charts module found — set QT_PREFIX or run" >&2
    echo "./setup.sh install (it installs Qt with -m qtcharts)" >&2
    exit 1
fi
if [ ! -d "$QT_PREFIX/lib/cmake/Qt6Charts" ]; then
    echo "QT_PREFIX=$QT_PREFIX has no Charts module — the app links against it" >&2
    exit 1
fi

VERSION="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' "$ROOT/CMakeLists.txt" | awk '{print $2}')"
VERSION="${VERSION:-0.0.0}"

echo "== AppImage: TradingApp $VERSION (Qt at $QT_PREFIX) =="

# A tree cached against a different source dir or a different Qt kit keeps its
# old Qt6_DIR even when -DCMAKE_PREFIX_PATH says otherwise (that is how a kit
# without Charts kept being picked), so discard it rather than reconfigure it.
if [ -f "$BUILD/CMakeCache.txt" ]; then
    cached_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt" | head -1)"
    cached_qt="$(sed -n 's|^Qt6_DIR:PATH=||p' "$BUILD/CMakeCache.txt" | head -1)"
    if { [ -n "$cached_home" ] && [ "$cached_home" != "$ROOT" ]; } ||
        { [ -n "$cached_qt" ] && [ "${cached_qt#"$QT_PREFIX"}" = "$cached_qt" ]; }; then
        echo "discarding $BUILD — cached for source '$cached_home', Qt '$cached_qt'"
        rm -rf "$BUILD"
    fi
fi

if [ "$SKIP_BUILD" -eq 0 ]; then
    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DTRADINGAPP_WARNINGS_AS_ERRORS=ON \
        -DTRADINGAPP_SKIP_QT_DEPLOY=ON
    cmake --build "$BUILD" --target TradingApp -j"$JOBS"
fi

rm -rf "$APPDIR"
# No --component: the install rules in CMakeLists.txt are uncomponentised, and
# `--install --component Runtime` would install NOTHING while still exiting 0 —
# leaving an empty usr/bin that the Qt plugin then reports as "no Qt modules".
DESTDIR="$APPDIR" cmake --install "$BUILD"
if [ ! -x "$APPDIR/usr/bin/TradingApp" ]; then
    echo "install produced no $APPDIR/usr/bin/TradingApp — nothing to package" >&2
    exit 1
fi

# The desktop entry and icon are the AppImage's identity: linuxdeploy reads the
# entry, and the icon is what a desktop shows after integration.
install -Dm644 "$ROOT/packaging/TradingApp.desktop" \
    "$APPDIR/usr/share/applications/TradingApp.desktop"
install -Dm644 "$ROOT/packaging/tradingapp.png" \
    "$APPDIR/usr/share/icons/hicolor/256x256/apps/tradingapp.png"

# Ship the example key file next to the binary so a first-time user can see the
# shape of the file the app looks for. The real one is never packaged.
install -Dm644 "$ROOT/apiKeyEtoro.example.json" \
    "$APPDIR/usr/share/TradingApp/apiKeyEtoro.example.json"
install -Dm644 "$ROOT/config.json" "$APPDIR/usr/share/TradingApp/config.json"

LD="$ROOT/tools/third-party/linuxdeploy-x86_64.AppImage"
if [ ! -x "$LD" ] || [ ! -x "$ROOT/tools/third-party/linuxdeploy-plugin-qt-x86_64.AppImage" ]; then
    echo "-- fetching the AppImage tooling (pinned) --"
    "$ROOT/tools/fetch_linuxdeploy.sh"
fi

mkdir -p "$OUT_DIR"
# QMAKE tells the Qt plugin WHICH Qt to bundle — without it the plugin picks
# whatever qmake is on PATH, which is how a 6.10 binary ends up shipping 6.2
# libraries. EXTRA_QT_PLUGINS adds the ones no symbol reference reveals: the TLS
# backend (HTTPS to the API) and the styles/iconengines the UI asks for by name.
# APPIMAGE_EXTRACT_AND_RUN: no FUSE in containers or on CI runners.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$QT_PREFIX/bin/qmake"
# linuxdeploy resolves the binary's dependencies through the normal loader
# search path, and the Qt kit is not on it (the app finds Qt via RUNPATH at
# run time), so without this it stops at "Could not find dependency:
# libQt6Charts.so.6".
export LD_LIBRARY_PATH="$QT_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export EXTRA_QT_PLUGINS="tls;imageformats;iconengines;styles;platformthemes"
# The Qt plugin bundles only the xcb platform plugin. offscreen costs ~30 KB
# and is what makes the AppImage runnable on a headless box — including the
# smoke test in the release workflow, which starts it with
# QT_QPA_PLATFORM=offscreen. Wayland is deliberately not bundled: it would
# drag in the whole libwayland stack, and XWayland serves xcb fine.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so"
export OUTPUT="$OUT_DIR/TradingApp-$VERSION-x86_64.AppImage"
export VERSION

"$LD" --appdir "$APPDIR" \
    --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/TradingApp.desktop" \
    --icon-file "$ROOT/packaging/tradingapp.png" \
    --output appimage

chmod +x "$OUTPUT"
echo
echo "AppImage: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
sha256sum "$OUTPUT" | tee "$OUTPUT.sha256"
