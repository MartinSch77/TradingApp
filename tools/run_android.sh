#!/usr/bin/env bash
# Boot an Android emulator, install the APK, launch it and grab a screenshot —
# the "does it actually run" counterpart of tools/build_android.sh.
#
#   tools/run_android.sh [--apk <file>] [--avd <name>] [--sdk <dir>]
#                        [--keep] [--shot <png>]
#
#   --apk   APK to install (default: newest downloads/*.apk)
#   --avd   AVD name (created on first use from the installed system image)
#   --keep  leave the emulator running afterwards (default: shut it down)
#   --shot  where to write the screenshot (default: downloads/android-screenshot.png)
#
# WSL specifics, measured rather than assumed:
#   * the emulator needs /dev/kvm. WSL2 exposes it only with nested virtualisation
#     enabled, and the device is root:kvm 0660 — so the user must be in the `kvm`
#     group (`sudo usermod -aG kvm $USER`, then a NEW login shell). Without that
#     the emulator either refuses or falls back to software emulation that takes
#     tens of minutes to boot; this script checks and says which.
#   * headless (-no-window) on purpose: WSLg can show the emulator, but a
#     screenshot pulled with `adb exec-out screencap` is reproducible, needs no
#     display at all, and is what CI could keep as evidence.
#   * -gpu swiftshader_indirect: software GL. WSLg's GL is not something the
#     emulator can pass through reliably, and this app only needs 2D.
#
# Exit 3 = SKIPPED (no SDK, no system image, or no KVM access) so a pipeline stays
# green on a machine that cannot run an emulator.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXIT_SKIPPED=3
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
APK=""
AVD="tradingapp_x86_64"
KEEP=0
SHOT="$ROOT/downloads/android-screenshot.png"

while [ $# -gt 0 ]; do
    case "$1" in
    --apk) shift; APK="${1:?--apk needs a value}" ;;
    --avd) shift; AVD="${1:?--avd needs a value}" ;;
    --sdk) shift; SDK="${1:?--sdk needs a value}" ;;
    --shot) shift; SHOT="${1:?--shot needs a value}" ;;
    --keep) KEEP=1 ;;
    -h | --help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

skip() {
    echo "SKIPPED: $1" >&2
    exit $EXIT_SKIPPED
}

ADB="$SDK/platform-tools/adb"
EMU="$SDK/emulator/emulator"
[ -x "$ADB" ] || skip "no adb at $ADB (see ./setup.sh android)"
[ -x "$EMU" ] || skip "no emulator at $EMU (sdkmanager --install emulator)"

IMAGE="$(ls -d "$SDK"/system-images/*/*/x86_64 2>/dev/null | sort -V | tail -1)"
[ -n "$IMAGE" ] || skip "no x86_64 system image (sdkmanager --install 'system-images;android-35;google_apis;x86_64')"

# KVM: presence AND access. A readable /dev/kvm we cannot open is the failure mode
# that otherwise shows up as a 20-minute "boot" that never completes.
[ -e /dev/kvm ] || skip "/dev/kvm missing — enable nested virtualisation for WSL2 (.wslconfig: nestedVirtualization=true)"
if ! { [ -r /dev/kvm ] && [ -w /dev/kvm ]; }; then
    skip "/dev/kvm exists but is not accessible to $(id -un) — run: sudo usermod -aG kvm $(id -un)   (then start a NEW shell)"
fi

if [ -z "$APK" ]; then
    APK="$(ls -t "$ROOT"/downloads/*.apk 2>/dev/null | head -1)"
fi
[ -n "$APK" ] && [ -f "$APK" ] || skip "no APK given and none in downloads/ (tools/build_android.sh)"

# --- the AVD ---------------------------------------------------------------
SYS_IMAGE_ID="system-images;$(basename "$(dirname "$(dirname "$IMAGE")")");$(basename "$(dirname "$IMAGE")");x86_64"
if ! "$SDK/cmdline-tools/latest/bin/avdmanager" list avd -c 2>/dev/null | grep -qx "$AVD"; then
    echo "creating AVD $AVD from $SYS_IMAGE_ID"
    echo no | "$SDK/cmdline-tools/latest/bin/avdmanager" create avd -n "$AVD" \
        -k "$SYS_IMAGE_ID" --device pixel_5 --force >/dev/null
fi

cleanup() {
    if [ "$KEEP" -eq 0 ]; then
        "$ADB" emu kill >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

echo "booting $AVD (headless, swiftshader)…"
"$EMU" -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot \
    -gpu swiftshader_indirect -accel on -netdelay none -netspeed full \
    > "${TMPDIR:-/tmp}/emulator-$AVD.log" 2>&1 &

"$ADB" start-server >/dev/null 2>&1 || true
"$ADB" wait-for-device
# wait-for-device returns as soon as adbd answers; the framework is only usable
# once sys.boot_completed flips, which on a cold boot is another minute or two.
for _ in $(seq 1 180); do
    [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] && break
    sleep 2
done
[ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] ||
    { echo "emulator did not finish booting — see ${TMPDIR:-/tmp}/emulator-$AVD.log" >&2; exit 1; }
echo "booted: $("$ADB" shell getprop ro.build.version.release | tr -d '\r') (API $("$ADB" shell getprop ro.build.version.sdk | tr -d '\r'))"

echo "installing $(basename "$APK")"
"$ADB" install -r -g "$APK"

PKG="$("$SDK"/cmdline-tools/latest/bin/apkanalyzer manifest application-id "$APK" 2>/dev/null ||
    echo org.qtproject.example)"
echo "launching $PKG"
"$ADB" shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
sleep 12   # let Qt load its plugins, build the UI and take the first poll tick

mkdir -p "$(dirname "$SHOT")"
"$ADB" exec-out screencap -p > "$SHOT"
echo "screenshot: ${SHOT#"$ROOT"/}  ($(du -h "$SHOT" | cut -f1))"

# The app's own log is the proof it got past plugin loading and into its own code.
echo "--- logcat (app only) ---"
"$ADB" logcat -d -s TradingApp:V QtCore:V Qt:V AndroidRuntime:E 2>/dev/null | tail -25 || true

if "$ADB" shell pidof "$PKG" >/dev/null 2>&1; then
    echo "RUNNING: $PKG is alive on the emulator"
else
    echo "WARNING: $PKG is not running — it started and exited; see the logcat above" >&2
    exit 1
fi
