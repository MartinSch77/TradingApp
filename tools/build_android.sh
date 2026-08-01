#!/usr/bin/env bash
# Build the Android APK (and optionally run it on an emulator) from the same
# CMake project the desktops use — REQ-N-001.
#
#   tools/build_android.sh [--abi <abi>] [--release] [--run] [--sdk <dir>]
#
#   --abi      android_x86_64 (default: what an emulator on an x86_64 host runs
#              natively) or android_arm64_v8a (phones/tablets). One Qt kit per
#              ABI, so the kit must exist for the ABI you ask for.
#   --release  release build (unsigned APK; a signed one needs your keystore)
#   --run      after building: boot an AVD, install, launch, screenshot
#   --sdk      Android SDK root (default $ANDROID_HOME, else ~/Android/Sdk)
#
# Output: downloads/TradingApp-<version>-<abi>-<type>.apk
#
# What this needs, and why the script checks instead of assuming:
#   * a Qt for Android kit — ~/Qt/<ver>/<abi>. aqt installs it from the `all_os`
#     host (NOT `linux`) for Qt >= 6.8:
#       aqt install-qt all_os android <ver> <abi> -m qtcharts -O ~/Qt
#   * the Android SDK + the NDK VERSION QT WAS BUILT AGAINST. A mismatched NDK is
#     the classic "it configures but links wrong" trap, so the version is read out
#     of the kit itself (below) rather than hardcoded here.
#   * a JDK (17+) for androiddeployqt's gradle run.
#
# Exit 3 = SKIPPED (a prerequisite is missing), so this can sit in a pipeline
# without failing it on a machine that has no Android toolchain — the same
# contract as the Axivion and coverage stages.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXIT_SKIPPED=3
ABI="android_x86_64"
BUILD_TYPE="Debug"
RUN=0
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"

while [ $# -gt 0 ]; do
    case "$1" in
    --abi) shift; ABI="${1:?--abi needs a value}" ;;
    --release) BUILD_TYPE="Release" ;;
    --run) RUN=1 ;;
    --sdk) shift; SDK="${1:?--sdk needs a value}" ;;
    -h | --help) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

skip() {
    echo "SKIPPED: $1" >&2
    exit $EXIT_SKIPPED
}

# --- the Qt kit for this ABI ------------------------------------------------
# Newest kit that actually contains the ABI asked for, and Charts (this app cannot
# build without it — picking "newest" blindly yields a configure error instead).
QT_ANDROID=""
for kit in $(ls -d "$HOME/Qt"/*/"$ABI" 2>/dev/null | sort -V -r); do
    if [ -f "$kit/lib/cmake/Qt6Charts/Qt6ChartsConfig.cmake" ]; then
        QT_ANDROID="$kit"
        break
    fi
done
[ -n "$QT_ANDROID" ] ||
    skip "no Qt for Android kit with Charts for $ABI (aqt install-qt all_os android <ver> $ABI -m qtcharts -O ~/Qt)"
QT_VER="$(basename "$(dirname "$QT_ANDROID")")"

# The host (desktop) kit of the SAME Qt version builds the tools androiddeployqt runs.
QT_HOST="$HOME/Qt/$QT_VER/gcc_64"
[ -d "$QT_HOST" ] || skip "the Android kit needs the matching desktop kit as its host: $QT_HOST"

# --- SDK / NDK -------------------------------------------------------------
[ -x "$SDK/platform-tools/adb" ] || skip "no Android SDK at $SDK (see ./setup.sh android)"

# The NDK version Qt was BUILT with is recorded in the kit's own module metadata as
# "ndk_version" (modules/*.json) — read it back instead of hardcoding, so this script
# stays correct across Qt upgrades. `|| true` is load-bearing: grep exits 1 when it
# matches nothing and `set -o pipefail` would then kill the script on the assignment.
NDK_WANT="$(grep -rhoE '"ndk_version"[^"]*"[0-9.]+"' "$QT_ANDROID/modules" 2>/dev/null |
    grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort | uniq -c | sort -rn | head -1 |
    awk '{print $2}' || true)"
if [ -n "$NDK_WANT" ] && [ -d "$SDK/ndk/$NDK_WANT" ]; then
    NDK="$SDK/ndk/$NDK_WANT"
else
    NDK="$(ls -d "$SDK/ndk"/* 2>/dev/null | sort -V | tail -1 || true)"
    [ -n "$NDK" ] || skip "no NDK under $SDK/ndk (Qt $QT_VER expects ${NDK_WANT:-unknown})"
    if [ -n "$NDK_WANT" ] && [ "$(basename "$NDK")" != "$NDK_WANT" ]; then
        echo "WARNING: Qt $QT_VER was built with NDK $NDK_WANT, using $(basename "$NDK") — " \
             "a mismatch can link an APK that fails to load" >&2
    fi
fi

command -v java >/dev/null || skip "no JDK on PATH (androiddeployqt runs gradle)"

BUILD_DIR="$ROOT/build-android-${ABI#android_}"
echo "Qt kit   : $QT_ANDROID"
echo "host kit : $QT_HOST"
echo "SDK / NDK: $SDK  /  $(basename "$NDK")"
echo "build    : $BUILD_DIR ($BUILD_TYPE)"

# --- configure + build -----------------------------------------------------
# The kit's qt-cmake wrapper sets the NDK toolchain file, the ABI and the host
# tools; calling plain cmake here is the single most common way to get a build
# that looks fine and produces an unloadable APK.
"$QT_ANDROID/bin/qt-cmake" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DQT_HOST_PATH="$QT_HOST" \
    -DANDROID_SDK_ROOT="$SDK" \
    -DANDROID_NDK_ROOT="$NDK"

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
# apk_all also runs androiddeployqt; the plain build only produces the .so.
cmake --build "$BUILD_DIR" --target apk

APK="$(find "$BUILD_DIR/android-build/build/outputs/apk" -name '*.apk' -type f 2>/dev/null |
    sort | tail -1)"
[ -n "$APK" ] || { echo "androiddeployqt produced no APK — see the log above" >&2; exit 1; }

VERSION="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' "$ROOT/CMakeLists.txt" | awk '{print $2}')"
mkdir -p "$ROOT/downloads"
OUT="$ROOT/downloads/TradingApp-${VERSION:-0.0.0}-${ABI#android_}-$(echo "$BUILD_TYPE" | tr 'A-Z' 'a-z').apk"
cp "$APK" "$OUT"
echo "APK: ${OUT#"$ROOT"/}  ($(du -h "$OUT" | cut -f1))"

[ "$RUN" -eq 1 ] || exit 0

# --- run it on an emulator -------------------------------------------------
exec "$ROOT/tools/run_android.sh" --apk "$OUT" --sdk "$SDK"
