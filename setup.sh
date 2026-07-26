#!/usr/bin/env bash
# Provision a naked Debian/Ubuntu Linux with every tool this project needs,
# idempotently — existing tools are left alone — and keep them current:
#
#   ./setup.sh [install]   install everything that is missing
#   ./setup.sh update      update all managed tools to their latest versions
#   ./setup.sh status      report found/missing tools and versions (read-only)
#
# What it manages
#   apt    build-essential, ninja, git, curl, clang-18 + LLVM tools,
#          clang-tidy, cppcheck, clazy, valgrind, lcov, doxygen, Java (for
#          PlantUML), python3 + pipx, Qt xcb/OpenGL runtime libraries
#   pipx   cmake (>= 4.2 — distro cmake is usually too old), strictdoc,
#          doorstop, aqtinstall
#   aqt    Qt ${QT_VERSION} (gcc_64 + qtcharts) into ~/Qt — the layout the
#          build scripts expect (override with QT_PREFIX at build time)
#   curl   PlantUML jar (pinned in tools/fetch_plantuml.sh)
#
# NOT installable here (license-bound, detected + reported only):
#   Axivion Suite (~/bauhaus-suite + ~/AxivionDashboard), Squish Coco
#   (/opt/SquishCoco), and the eToro/Anthropic API keys (apiKeyEtoro.json —
#   copy apiKeyEtoro.example.json and fill in your keys).
#
# install/update need sudo for the apt part; everything else stays in $HOME.
set -uo pipefail

MODE="${1:-install}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
QT_VERSION="${QT_VERSION:-6.10.2}"
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
    libgl1-mesa-dev libglx-dev libopengl0 libegl1
    libxkbcommon0 libxkbcommon-x11-0 libfontconfig1 libfreetype6 libdbus-1-3
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0
    libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1
)
PIPX_PKGS=(cmake strictdoc doorstop aqtinstall codespell sphinx)

have() { command -v "$1" >/dev/null 2>&1; }

version_of() {
    case "$1" in
    cmake) cmake --version 2>/dev/null | head -1 | awk '{print $3}' ;;
    g++) g++ -dumpfullversion 2>/dev/null ;;
    clang-18) clang-18 --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 ;;
    qt) [ -d "$QT_DIR/$QT_VERSION/gcc_64" ] && echo "$QT_VERSION" ;;
    plantuml) [ -f "$ROOT/tools/third-party/plantuml.jar" ] && java -jar "$ROOT/tools/third-party/plantuml.jar" -version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9.]+' | head -1 ;;
    axivion) [ -x "$HOME/bauhaus-suite/bin/axivion_ci" ] && "$HOME/bauhaus-suite/bin/axivion_ci" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9.]+' | head -1 ;;
    coco) [ -x /opt/SquishCoco/bin/coveragescanner ] && /opt/SquishCoco/bin/coveragescanner --cs-version 2>/dev/null | head -1 ;;
    *) "$1" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9.]+' | head -1 ;;
    esac
}

report() { # name, present?, detail
    printf '  %-12s %-8s %s\n' "$1" "$2" "$3"
}

status() {
    echo "== toolchain status =="
    for t in g++ cmake ninja git clang-18 clang-tidy cppcheck clazy-standalone \
        valgrind lcov doxygen java python3 pipx strictdoc doorstop aqt; do
        if have "$t"; then
            report "$t" "ok" "$(version_of "$t")"
        else
            report "$t" "MISSING" ""
        fi
    done
    if [ -d "$QT_DIR/$QT_VERSION/gcc_64" ]; then
        report "Qt" "ok" "$QT_DIR/$QT_VERSION/gcc_64"
    else
        report "Qt" "MISSING" "expected $QT_DIR/$QT_VERSION/gcc_64"
    fi
    [ -f "$ROOT/tools/third-party/plantuml.jar" ] &&
        report "plantuml" "ok" "$(version_of plantuml)" ||
        report "plantuml" "missing" "fetched on demand by tools/make_docs.sh"
    echo "== license-bound (manual) =="
    [ -x "$HOME/bauhaus-suite/bin/axivion_ci" ] &&
        report "axivion" "ok" "$HOME/bauhaus-suite" ||
        report "axivion" "manual" "install Axivion Suite to ~/bauhaus-suite (license required)"
    [ -x /opt/SquishCoco/bin/coveragescanner ] &&
        report "coco" "ok" "/opt/SquishCoco ($(/opt/SquishCoco/bin/cocolic --check 2>&1 | head -1))" ||
        report "coco" "manual" "optional: Squish Coco to /opt/SquishCoco (license required)"
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

case "$MODE" in
install)
    apt_install
    pipx_install
    supply_chain_install
    qt_install
    plantuml_install
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
    echo
    status
    ;;
status)
    status
    ;;
*)
    echo "usage: $0 [install|update|status]" >&2
    exit 2
    ;;
esac
