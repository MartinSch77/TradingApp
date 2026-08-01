#!/usr/bin/env bash
# Provision a naked Debian/Ubuntu Linux with every tool this project needs,
# idempotently — existing tools are left alone — and keep them current:
#
#   ./setup.sh [install]   install everything that is missing
#   ./setup.sh update      update all managed tools to their latest versions
#   ./setup.sh status      report found/missing tools and versions (read-only)
#
# What it manages
#   apt    build-essential, ninja, git, gh, curl, clang-18 + LLVM tools,
#          clang-tidy, cppcheck, clazy, valgrind, lcov, doxygen, Java (for
#          PlantUML), python3 + pipx, Qt xcb/OpenGL runtime libraries
#   pipx   cmake (>= 4.2 — distro cmake is usually too old), strictdoc,
#          doorstop, aqtinstall, codespell, sphinx (+ myst-parser), gcovr
#   aqt    Qt ${QT_VERSION} (gcc_64 + qtcharts) into ~/Qt — the layout the
#          build scripts expect (override with QT_PREFIX at build time)
#   curl   PlantUML jar (pinned in tools/fetch_plantuml.sh); supply-chain
#          tools syft / grype / trivy into ~/.local/bin
#
# NOT installable here — LICENSE-BOUND, so they are detected and reported, and
# the stages that need them SKIP with a message instead of failing (exit 3 =
# "stage skipped", which build_all.sh reports as `skipped`):
#   Axivion Suite (~/bauhaus-suite + ~/AxivionDashboard)
#                 -> axivion/start_analysis.sh reports `skipped`
#   Squish Coco   (/opt/SquishCoco)
#                 -> tools/coverage.sh coco reports `skipped`; auto mode just
#                    uses gcov + clang MC/DC instead
# Also manual: the eToro/Anthropic API keys (copy apiKeyEtoro.example.json to
# apiKeyEtoro.json — the app runs in SIMULATION mode without them).
#
# install/update need sudo for the apt part; everything else stays in $HOME.
set -uo pipefail

MODE="${1:-install}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
QT_VERSION="${QT_VERSION:-6.11.1}"
QT_DIR="$HOME/Qt"
export PATH="$HOME/.local/bin:$PATH"

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

# Everything apt-managed. Qt runtime libs cover running the GUI/tests on a
# naked system (xcb platform plugin, OpenGL, fontconfig).
APT_PKGS=(
    build-essential ninja-build git gh curl ca-certificates
    clang-18 llvm-18 clang-tidy clang-tools-18
    cppcheck clazy valgrind lcov
    doxygen graphviz default-jre-headless
    python3 python3-venv python3-pip pipx
    python3-reportlab  # PDF quality report (tools/make_report.py); apt, not pipx:
                       # the report is IMPORTED by the system python3, and a pipx
                       # venv is not on that interpreter's path
    libgl1-mesa-dev libglx-dev libopengl0 libegl1
    libxkbcommon0 libxkbcommon-x11-0 libfontconfig1 libfreetype6 libdbus-1-3
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0
    libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1
)
# Keep in sync with $PipPkgs in setup.ps1 so both platforms carry the same
# python-based tooling (gcovr is the CI-friendly gcov reporter named in
# docs/tools.md).
PIPX_PKGS=(cmake strictdoc doorstop aqtinstall codespell sphinx gcovr lizard)

have() { command -v "$1" >/dev/null 2>&1; }

version_of() {
    case "$1" in
    cmake) cmake --version 2>/dev/null | head -1 | awk '{print $3}' ;;
    g++) g++ -dumpfullversion 2>/dev/null ;;
    # dot answers -V (not --version) and writes to stderr.
    dot) dot -V 2>&1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1 ;;
    # aqt has no --version; the subcommand is spelled "version".
    aqt) aqt version 2>&1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1 ;;
    clang-18) clang-18 --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 ;;
    qt) [ -d "$QT_DIR/$QT_VERSION/gcc_64" ] && echo "$QT_VERSION" ;;
    # PMD prints a banner before the version line.
    pmd) local pmd_bin; pmd_bin="$(ls -d "$ROOT"/tools/third-party/pmd-bin-*/bin/pmd 2>/dev/null | tail -1)"
        [ -x "$pmd_bin" ] && "$pmd_bin" --version 2>/dev/null | grep -oE 'PMD [0-9.]+' | head -1 | awk '{print $2}' ;;
    plantuml) [ -f "$ROOT/tools/third-party/plantuml.jar" ] && java -jar "$ROOT/tools/third-party/plantuml.jar" -version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9.]+' | head -1 ;;
    axivion) [ -x "$HOME/bauhaus-suite/bin/axivion_ci" ] && "$HOME/bauhaus-suite/bin/axivion_ci" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9.]+' | head -1 ;;
    coco) [ -x /opt/SquishCoco/bin/coveragescanner ] && /opt/SquishCoco/bin/coveragescanner --cs-version 2>/dev/null | head -1 ;;
    # Stop the version at the last digit: [0-9.]+ would keep a trailing dot
    # ("ninja 1.13.0." from "1.13.0.git.kitware...").
    *) "$1" --version 2>/dev/null | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1 ;;
    esac
}

report() { # name, present?, detail
    printf '  %-12s %-8s %s\n' "$1" "$2" "$3"
}

# ---------------------------------------------------------------------------
# Android (separate mode: ~6 GB of SDK/NDK/system image + a Qt kit per ABI, which
# nobody building the desktop app should have to download)
# ---------------------------------------------------------------------------
# Everything lands under $HOME — no sudo, no apt. The ONE thing this cannot do for
# you is /dev/kvm access for the emulator: the device is root:kvm 0660, so it needs
# `sudo usermod -aG kvm $USER` and a new login shell. It is reported, not silently
# worked around, because without it the emulator "boots" for tens of minutes.
ANDROID_SDK_DIR="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
ANDROID_CMDLINE_ZIP="https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip"
ANDROID_PLATFORM="android-35"          # matches QT_ANDROID_TARGET_SDK_VERSION in CMakeLists
ANDROID_COMPILE_PLATFORM="android-36"  # matches QT_ANDROID_COMPILE_SDK_VERSION: Qt 6.11's
                                       # androidx.core dependency refuses anything older
ANDROID_BUILD_TOOLS="35.0.1"
ANDROID_SYSTEM_IMAGE="system-images;$ANDROID_PLATFORM;google_apis;x86_64"

android_install() {
    echo "== Android SDK ($ANDROID_SDK_DIR) =="
    local mgr="$ANDROID_SDK_DIR/cmdline-tools/latest/bin/sdkmanager"
    if [ ! -x "$mgr" ]; then
        echo "-- command-line tools"
        mkdir -p "$ANDROID_SDK_DIR/cmdline-tools"
        local zip="$ANDROID_SDK_DIR/cmdline-tools/cmdline-tools.zip"
        curl -sSL -o "$zip" "$ANDROID_CMDLINE_ZIP"
        unzip -q -o "$zip" -d "$ANDROID_SDK_DIR/cmdline-tools"
        rm -f "$zip"
        # The zip unpacks as cmdline-tools/; sdkmanager insists on being under
        # cmdline-tools/latest/ or it cannot find the SDK root.
        [ -d "$ANDROID_SDK_DIR/cmdline-tools/latest" ] ||
            mv "$ANDROID_SDK_DIR/cmdline-tools/cmdline-tools" "$ANDROID_SDK_DIR/cmdline-tools/latest"
    fi
    yes | "$mgr" --licenses >/dev/null 2>&1 || true

    # The NDK version is NOT a free choice: Qt is built against one, and a mismatch
    # is a link-time or run-time surprise. Read it out of the installed Qt kit; fall
    # back to a known-good one only when no kit is there yet.
    local ndk
    ndk="$(grep -rhoE '2[0-9]\.[0-9]\.[0-9]{8}' "$HOME"/Qt/*/android_*/lib/cmake 2>/dev/null |
        sort | uniq -c | sort -rn | head -1 | awk '{print $2}')"
    ndk="${ndk:-27.2.12479018}"
    echo "-- platform-tools, $ANDROID_PLATFORM, build-tools $ANDROID_BUILD_TOOLS, NDK $ndk, emulator, system image"
    "$mgr" --install "platform-tools" "platforms;$ANDROID_PLATFORM" "platforms;$ANDROID_COMPILE_PLATFORM" \
        "build-tools;$ANDROID_BUILD_TOOLS" "ndk;$ndk" "emulator" "$ANDROID_SYSTEM_IMAGE" ||
        echo "sdkmanager reported an error — rerun; partial downloads resume" >&2

    echo "== Qt for Android =="
    # aqt serves Android from the `all_os` host for Qt >= 6.8 (NOT `linux`), and one
    # kit per ABI. x86_64 is what an emulator on an x86_64 host runs natively;
    # arm64_v8a is what a phone needs. Charts is required by this app.
    local want="$QT_VERSION" abi
    for abi in android_x86_64 android_arm64_v8a; do
        if [ -d "$HOME/Qt/$want/$abi" ]; then
            echo "$abi already installed ($want)"
            continue
        fi
        have aqt || { echo "aqt missing — run ./setup.sh install first" >&2; return 1; }
        aqt install-qt all_os android "$want" "$abi" -m qtcharts -O "$HOME/Qt" ||
            echo "aqt could not install $abi for $want" >&2
    done

    echo
    echo "== emulator prerequisite: /dev/kvm =="
    if [ ! -e /dev/kvm ]; then
        echo "  /dev/kvm MISSING — on WSL2 enable nested virtualisation in %USERPROFILE%\\.wslconfig:"
        echo "      [wsl2]"
        echo "      nestedVirtualization=true"
        echo "  then: wsl --shutdown"
    elif [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        echo "  ok — hardware acceleration available"
    else
        echo "  present but NOT accessible to $(id -un). Run once:"
        echo "      sudo usermod -aG kvm $(id -un)"
        echo "  then start a NEW login shell (group membership is per-session)."
    fi
    echo
    echo "Build:  tools/build_android.sh --abi android_x86_64        (APK in downloads/)"
    echo "Run:    tools/build_android.sh --abi android_x86_64 --run  (emulator + screenshot)"
}

status() {
    echo "== toolchain status =="
    # Keep this list in step with Show-Status in setup.ps1 so both platforms
    # report the same set of tools.
    for t in g++ cmake ninja git gh clang-18 clang-tidy cppcheck clazy-standalone \
        valgrind lcov gcovr doxygen dot java python3 pipx \
        strictdoc doorstop codespell sphinx-build aqt lizard syft grype trivy; do
        if have "$t"; then
            report "$t" "ok" "$(version_of "$t")"
        else
            report "$t" "MISSING" ""
        fi
    done
    # Android target (REQ-N-001): a separate ~6 GB toolchain, so it is reported here
    # but installed only by `./setup.sh android`.
    local asdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
    if [ -x "$asdk/platform-tools/adb" ]; then
        report "android-sdk" "ok" "$asdk"
    else
        report "android-sdk" "missing" "./setup.sh android (Android target only)"
    fi
    local andk
    andk="$(ls -d "$asdk"/ndk/* 2>/dev/null | sort -V | tail -1)"
    [ -n "$andk" ] && report "android-ndk" "ok" "$(basename "$andk")" ||
        report "android-ndk" "missing" "./setup.sh android"
    local aqtkits
    aqtkits="$(ls -d "$HOME"/Qt/*/android_* 2>/dev/null | sed 's#.*/##' | tr '\n' ' ')"
    [ -n "$aqtkits" ] && report "qt-android" "ok" "$aqtkits" ||
        report "qt-android" "missing" "./setup.sh android"
    if [ ! -e /dev/kvm ]; then
        report "kvm" "missing" "emulator needs WSL2 nestedVirtualization=true"
    elif [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        report "kvm" "ok" "emulator can use hardware acceleration"
    else
        report "kvm" "MISSING" "present but no access: sudo usermod -aG kvm $(id -un)"
    fi

    # reportlab is a LIBRARY (no console script), so probe the import the PDF
    # report actually performs rather than looking for a binary on PATH.
    if python3 -c "import reportlab" >/dev/null 2>&1; then
        report "reportlab" "ok" "$(python3 -c 'import reportlab; print(reportlab.Version)' 2>/dev/null)"
    else
        report "reportlab" "MISSING" "PDF report stage skips (apt python3-reportlab)"
    fi
    # Any gcc_64 kit will do; the build scripts take the newest.
    local qt
    qt="$(ls -d "$QT_DIR"/*/gcc_64 2>/dev/null | sort -V | tail -1)"
    if [ -n "$qt" ]; then
        report "Qt" "ok" "$qt"
    else
        report "Qt" "MISSING" "expected $QT_DIR/$QT_VERSION/gcc_64"
    fi
    [ -f "$ROOT/tools/third-party/plantuml.jar" ] &&
        report "plantuml" "ok" "$(version_of plantuml)" ||
        report "plantuml" "missing" "fetched on demand by tools/make_docs.sh"
    [ -x "$(ls -d "$ROOT"/tools/third-party/pmd-bin-*/bin/pmd 2>/dev/null | tail -1)" ] &&
        report "pmd" "ok" "$(version_of pmd)" ||
        report "pmd" "missing" "copy-paste detection skips; ./setup.sh install fetches it"
    [ -x "$ROOT/tools/third-party/linuxdeploy-x86_64.AppImage" ] &&
        report "linuxdeploy" "ok" "tools/third-party" ||
        report "linuxdeploy" "missing" "AppImage packaging; ./setup.sh install fetches it"
    echo "== no Linux counterpart =="
    report "OpenCppCov" "n/a" "Windows-only; gcov+lcov is the Linux line/branch tool"
    echo "== license-bound (manual) — the stages that need these report 'skipped' =="
    if [ -x "$HOME/bauhaus-suite/bin/axivion_ci" ]; then
        report "axivion" "ok" "$HOME/bauhaus-suite"
    elif command -v axivion_ci >/dev/null 2>&1; then
        report "axivion" "ok" "$(command -v axivion_ci)"
    else
        report "axivion" "manual" "license required; 'axivion' stage reports skipped"
    fi
    # The Axivion MCP servers in .mcp.json take their paths from the environment
    # (tools/mcp_env.sh resolves them; exit 3 = no Suite installed).
    # Checks the durable store (~/.profile), not this process: a shell started
    # before --persist ran would otherwise report the setup as incomplete.
    if "$ROOT/tools/mcp_env.sh" >/dev/null 2>&1; then
        if grep -qF 'TradingApp Axivion MCP env' "$HOME/.profile" 2>/dev/null; then
            report "ax MCP" "ok" "servers configured; see tools/mcp_env.sh"
        else
            report "ax MCP" "manual" "run tools/mcp_env.sh --persist, then a new login shell"
        fi
    else
        report "ax MCP" "n/a" "no Axivion Suite — the .mcp.json servers stay unavailable"
    fi
    [ -x /opt/SquishCoco/bin/coveragescanner ] &&
        report "coco" "ok" "/opt/SquishCoco ($(/opt/SquishCoco/bin/cocolic --check 2>&1 | head -1))" ||
        report "coco" "manual" "license required; 'coverage.sh coco' reports skipped"
    [ -f "$ROOT/apiKeyEtoro.json" ] &&
        report "api keys" "ok" "apiKeyEtoro.json present" ||
        report "api keys" "manual" "cp apiKeyEtoro.example.json apiKeyEtoro.json + fill in keys (app runs in SIMULATION without)"
}

apt_install() {
    echo "== apt packages =="
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${APT_PKGS[@]}"
}

pipx_install() {
    echo "== pipx tools =="
    have pipx || { echo "pipx missing — apt step must run first" >&2; return 1; }
    pipx ensurepath >/dev/null 2>&1 || true
    local p
    for p in "${PIPX_PKGS[@]}"; do
        # skip cmake from pipx when a system cmake >= 4.2 already exists
        if [ "$p" = "cmake" ] && have cmake; then
            if printf '%s\n4.2\n' "$(version_of cmake)" | sort -V | head -1 | grep -qx "4.2"; then
                echo "cmake $(version_of cmake) already >= 4.2 — pipx cmake skipped"
                continue
            fi
        fi
        pipx list 2>/dev/null | grep -q "package $p " || pipx install "$p"
    done
}

supply_chain_install() {
    echo "== supply-chain tools (syft / grype / trivy) =="
    local dst="$HOME/.local/bin"
    mkdir -p "$dst"
    have syft || curl -sSfL https://raw.githubusercontent.com/anchore/syft/main/install.sh | sh -s -- -b "$dst"
    have grype || curl -sSfL https://raw.githubusercontent.com/anchore/grype/main/install.sh | sh -s -- -b "$dst"
    have trivy || curl -sSfL https://raw.githubusercontent.com/aquasecurity/trivy/main/contrib/install.sh | sh -s -- -b "$dst"
    # sphinx needs the MyST markdown parser inside its pipx venv
    pipx runpip sphinx show myst-parser >/dev/null 2>&1 || pipx inject sphinx myst-parser || true
}

qt_install() {
    echo "== Qt $QT_VERSION =="
    if [ -d "$QT_DIR/$QT_VERSION/gcc_64" ]; then
        echo "already at $QT_DIR/$QT_VERSION/gcc_64"
        return 0
    fi
    have aqt || { echo "aqt missing — pipx step must run first" >&2; return 1; }
    # qtbase (Widgets/Network/Test) + the Charts add-on the app links against.
    aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 -m qtcharts -O "$QT_DIR"
}

plantuml_install() {
    echo "== PlantUML =="
    [ -f "$ROOT/tools/third-party/plantuml.jar" ] && { echo "already present"; return 0; }
    "$ROOT/tools/fetch_plantuml.sh"
}

# PMD CPD: the copy-paste detector tools/cpd_scan.py drives (Java tool, runs on
# the default-jre-headless installed above).
pmd_install() {
    echo "== PMD (copy-paste detection) =="
    "$ROOT/tools/fetch_pmd.sh"
}

# linuxdeploy + its Qt plugin: what tools/package_appimage.sh bundles the
# AppImage with. Windows needs no counterpart — windeployqt ships with Qt.
linuxdeploy_install() {
    echo "== linuxdeploy (AppImage packaging) =="
    "$ROOT/tools/fetch_linuxdeploy.sh"
}

# The Axivion MCP servers configured in .mcp.json resolve their paths from the
# environment, because the JSON must stay free of machine-specific paths and
# Claude Code's ${VAR} interpolation cannot branch on the platform. Exit 3 =
# no (license-bound) Suite installed, which is not an error here.
mcp_env_install() {
    echo "== Axivion MCP environment (.mcp.json) =="
    "$ROOT/tools/mcp_env.sh" --persist || true
}

case "$MODE" in
install)
    apt_install
    pipx_install
    supply_chain_install
    qt_install
    plantuml_install
    pmd_install
    linuxdeploy_install
    mcp_env_install
    echo
    status
    echo
    echo "Done. Build everything with: ./build_all.sh"
    ;;
update)
    echo "== apt update =="
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --only-upgrade "${APT_PKGS[@]}"
    echo "== pipx update =="
    pipx upgrade-all || true
    echo "== Qt =="
    if have aqt; then
        LATEST_QT="$(aqt list-qt linux desktop 2>/dev/null | tr ' ' '\n' | grep -E '^6\.' | sort -V | tail -1)"
        if [ -n "${LATEST_QT:-}" ] && [ "$LATEST_QT" != "$QT_VERSION" ] && [ ! -d "$QT_DIR/$LATEST_QT/gcc_64" ]; then
            echo "newer Qt available: $LATEST_QT (installed: $QT_VERSION)."
            echo "install with:  QT_VERSION=$LATEST_QT ./setup.sh install"
            echo "then build with:  QT_PREFIX=$QT_DIR/$LATEST_QT/gcc_64 ./build_all.sh"
        else
            echo "Qt $QT_VERSION is current (or the newer version is already installed)"
        fi
    fi
    echo "== PlantUML =="
    echo "pinned to the version in tools/fetch_plantuml.sh — bump VERSION there,"
    echo "delete tools/third-party/plantuml.jar and rerun ./setup.sh install."
    echo "== PMD =="
    echo "pinned to the version in tools/fetch_pmd.sh — bump VERSION there and"
    echo "rerun ./setup.sh install (the fetch script drops the old dist)."
    echo "== linuxdeploy =="
    echo "pinned (tag + sha256) in tools/fetch_linuxdeploy.sh — bump both there,"
    echo "delete tools/third-party/linuxdeploy*.AppImage and rerun ./setup.sh install."
    echo "== Android =="
    echo "NOT part of this install: the SDK + NDK + system image + a Qt kit per ABI"
    echo "are ~6 GB and only the Android target needs them. Run './setup.sh android'"
    echo "when you want it; 'status' below reports what is present."
    echo
    status
    ;;
status)
    status
    ;;
android)
    android_install
    ;;
*)
    echo "usage: $0 [install|update|status|android]" >&2
    exit 2
    ;;
esac
