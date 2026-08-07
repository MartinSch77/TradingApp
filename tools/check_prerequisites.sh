#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# What this machine can and cannot do — every external program the pipeline uses,
# what it is FOR, and how to get it.
#
#   tools/check_prerequisites.sh              # everything, grouped
#   tools/check_prerequisites.sh --release    # only what a RELEASE needs
#   tools/check_prerequisites.sh --quiet      # one line per missing item
#
# Why this exists rather than a page in the README: a list of prerequisites in prose
# goes stale silently, while a check that runs says what is true today. Every entry
# below names the stage it belongs to, so a missing tool translates directly into
# "this stage will report skipped" rather than into a surprise halfway through a
# release.
#
# THE RULE THIS PROJECT FOLLOWS: everything OPEN SOURCE that the pipeline needs is
# installable by ./setup.sh. The licence-bound tools (Squish, Squish Coco, Axivion,
# Qt Test Center) cannot be — they need a licence and a manual download — so they
# report SKIPPED and are listed as MISSING LICENCES in the quality PDF. A missing
# licence is never a build failure here.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
[ -f "$ROOT/tools/common.sh" ] && source "$ROOT/tools/common.sh"

RELEASE_ONLY=0
QUIET=0
for arg in "$@"; do
    case "$arg" in
    --release) RELEASE_ONLY=1 ;;
    --quiet) QUIET=1 ;;
    -h | --help)
        sed -n '2,18p' "$0"
        exit 0
        ;;
    esac
done

GREEN=$'\033[32m'
YELLOW=$'\033[33m'
RED=$'\033[31m'
DIM=$'\033[2m'
OFF=$'\033[0m'
[ -t 1 ] || { GREEN=""; YELLOW=""; RED=""; DIM=""; OFF=""; }

MISSING_REQUIRED=0
MISSING_OPTIONAL=0

# report <state> <name> <what it is for> <how to get it>
#   state: ok | missing-required | missing-optional | missing-licence
report() {
    local state="$1" name="$2" purpose="$3" howto="${4:-}"
    case "$state" in
    ok)
        [ "$QUIET" -eq 1 ] && return 0
        printf '  %s✔%s %-22s %s\n' "$GREEN" "$OFF" "$name" "$purpose"
        ;;
    missing-required)
        MISSING_REQUIRED=$((MISSING_REQUIRED + 1))
        printf '  %s✘%s %-22s %s\n' "$RED" "$OFF" "$name" "$purpose"
        printf '      %sinstall: %s%s\n' "$DIM" "$howto" "$OFF"
        ;;
    missing-optional)
        MISSING_OPTIONAL=$((MISSING_OPTIONAL + 1))
        printf '  %s○%s %-22s %s\n' "$YELLOW" "$OFF" "$name" "$purpose"
        printf '      %sinstall: %s%s\n' "$DIM" "$howto" "$OFF"
        ;;
    missing-licence)
        MISSING_OPTIONAL=$((MISSING_OPTIONAL + 1))
        printf '  %s○%s %-22s %s\n' "$YELLOW" "$OFF" "$name" "$purpose"
        printf '      %slicence-bound — %s%s\n' "$DIM" "$howto" "$OFF"
        ;;
    esac
}

have() { command -v "$1" >/dev/null 2>&1; }

check() { # check <command> <required|optional> <name> <purpose> <howto>
    if have "$1"; then
        report ok "$3" "$4"
    else
        report "missing-$2" "$3" "$4" "$5"
    fi
}

section() {
    [ "$QUIET" -eq 1 ] && return 0
    printf '\n%s== %s ==%s\n' "$OFF" "$1" "$OFF"
}

# ---------------------------------------------------------------------------
# Building and testing
# ---------------------------------------------------------------------------
if [ "$RELEASE_ONLY" -eq 0 ]; then
    section "build + test (./build_all.sh build test)"
    check cmake required "CMake" "configures and builds everything" \
        "./setup.sh — or apt-get install cmake"
    check g++ required "g++" "the compiler the gate treats as an analyzer too" \
        "apt-get install build-essential"
    check ninja optional "Ninja" "faster builds; CMake falls back to make" \
        "apt-get install ninja-build"
    check python3 required "Python 3" "traceability, metrics, the report, the trainer" \
        "apt-get install python3"

    # qt_kit_dir names the KIT ("gcc_64" / "gcc_arm64"), not a path — the kits
    # themselves live under ~/Qt/<version>/<kit>, newest first.
    qt_kit=""
    if declare -f qt_kit_dir >/dev/null 2>&1; then
        qt_kit="$(qt_kit_dir 2>/dev/null || true)"
    fi
    qt_dir=""
    if [ -n "$qt_kit" ]; then
        qt_dir="$(ls -d "$HOME"/Qt/*/"$qt_kit" 2>/dev/null | sort -Vr | head -1)"
    fi
    if [ -n "$qt_dir" ] && [ -d "$qt_dir" ]; then
        report ok "Qt 6" "$qt_dir"
    elif have qmake6 || [ -d /usr/include/x86_64-linux-gnu/qt6 ]; then
        report ok "Qt 6" "the distribution's Qt 6 (no ~/Qt kit for this architecture)"
    else
        report missing-required "Qt 6" "the toolkit the app is written in" \
            "./setup.sh — installs a Qt kit with Charts under ~/Qt"
    fi

    section "static analysis (./build_all.sh analysis)"
    check cppcheck required "cppcheck" "one of the seven gated analyzers" "./setup.sh"
    check clang-tidy required "clang-tidy" "gated analyzer + the .clang-tidy rules" "./setup.sh"
    check clazy optional "clazy" "Qt-specific analyzer (gated when present)" \
        "./setup.sh — needs sudo"
    check codespell optional "codespell" "spelling in code and docs (gated)" \
        "python3 -m pip install codespell"
    check java optional "Java" "runs PMD CPD, the clone gate" "apt-get install default-jre"
    check lizard optional "lizard" "the code-metrics ratchet" "python3 -m pip install lizard"
    check clang-format optional "clang-format" "the CI formatting check" "./setup.sh"

    section "coverage (./build_all.sh coverage)"
    check gcov optional "gcov" "line/branch coverage — the badge is built from it" \
        "comes with g++"
    check lcov optional "lcov/genhtml" "turns gcov data into coverage.info + HTML" \
        "apt-get install lcov"
    check gcovr optional "gcovr" "the coverage percentage CI publishes" \
        "python3 -m pip install gcovr"
    llvm_ver=""
    if declare -f llvm_suffix >/dev/null 2>&1; then
        llvm_ver="$(llvm_suffix 2>/dev/null || true)"
    fi
    if [ -n "$llvm_ver" ] && have "llvm-cov$llvm_ver"; then
        report ok "llvm-cov" "clang MC/DC (needs clang >= 18)"
    else
        report missing-optional "llvm-cov" "clang MC/DC (needs clang >= 18)" \
            "apt-get install clang-18 llvm-18"
    fi
    if [ -x "${COCO_DIR:-/opt/SquishCoco}/bin/csg++" ]; then
        if "${COCO_DIR:-/opt/SquishCoco}/bin/cocolic" --check >/dev/null 2>&1; then
            report ok "Squish Coco" "statement/decision/condition + MC/DC, licensed"
        else
            report missing-licence "Squish Coco" "statement/decision/condition + MC/DC" \
                "installed at ${COCO_DIR:-/opt/SquishCoco} but the licence check fails"
        fi
    else
        report missing-licence "Squish Coco" "statement/decision/condition + MC/DC" \
            "qt.io download + licence; the stage reports skipped without it"
    fi

    section "dynamic analysis (./build_all.sh sanitize)"
    check valgrind optional "valgrind" "memcheck over the plain test binaries" \
        "apt-get install valgrind"
    if [ -n "$llvm_ver" ] && have "clang++$llvm_ver"; then
        report ok "clang++" "ASan/UBSan and TSan builds"
    else
        report missing-optional "clang++" "ASan/UBSan and TSan builds" \
            "apt-get install clang-18"
    fi

    section "GUI testing (licence-bound, never a gate)"
    squish_dir=""
    for candidate in ${SQUISH_DIR:-} ${SQUISH_PREFIX:-} "$HOME"/squish-for-qt-* /opt/squish*; do
        [ -x "$candidate/bin/squishrunner" ] && squish_dir="$candidate" && break
    done
    if [ -n "$squish_dir" ]; then
        report ok "Squish" "the GUI suite ($squish_dir), forced into simulation"
    else
        report missing-licence "Squish" "the GUI suite (tools/squish_run.sh)" \
            "qt.io download + licence; installs to ~/squish-for-qt-<version>"
    fi
    if [ -n "${TESTCENTER_URL:-}" ] && [ -n "${TESTCENTER_TOKEN:-}" ]; then
        report ok "Qt Test Center" "results upload configured ($TESTCENTER_URL)"
    else
        report missing-licence "Qt Test Center" "collects every JUnit XML in one place" \
            "set TESTCENTER_URL and TESTCENTER_TOKEN (tools/testcenter_upload.sh)"
    fi
    if [ -x "${AXIVION_HOME:-$HOME/bauhaus-suite}/bin/axivion_ci" ] ||
        have axivion_ci; then
        report ok "Axivion Suite" "MISRA C++ 2023 (x86-64 only)"
    else
        report missing-licence "Axivion Suite" "MISRA C++ 2023 (x86-64 only)" \
            "licence + install; the stage reports skipped without it"
    fi

    section "optional runtime features"
    if [ -x "$HOME/.local/ollama/bin/ollama" ] || have ollama; then
        report ok "Ollama" "the LOCAL model the bot can take its picks from"
    else
        report missing-optional "Ollama" "the LOCAL model the bot can take its picks from" \
            "./setup.sh ollama — installs runtime + model under ~/.local/ollama"
    fi
fi

# ---------------------------------------------------------------------------
# Packaging and publishing — what a RELEASE actually needs
# ---------------------------------------------------------------------------
section "packaging a release (tools/package_*.sh, tools/publish_release.sh)"
check git required "git" "the version, the tag and the clean-tree check" "apt-get install git"
check gh required "GitHub CLI" "creates the release and uploads the assets" \
    "https://cli.github.com — then: gh auth login"
check zip optional "zip" "the docs and qualification bundles" "apt-get install zip"
check curl optional "curl" "downloads linuxdeploy, PMD, Test Center uploads" \
    "apt-get install curl"
# The Coverity build log is fetched with gh from the weekly cloud run; the check above
# already covers gh, so this only says where the evidence ends up.
if [ -f "$ROOT/analysis-results/coverity-build-log.txt" ]; then
    report ok "Coverity build log" "fetched into analysis-results/ (tools/fetch_coverity_log.sh)"
else
    report missing-optional "Coverity build log" "what cov-build captured on the cloud run" \
        "tools/fetch_coverity_log.sh — needs gh and a run within the 90-day retention"
fi

if have linuxdeploy || [ -x "$ROOT/.cache/linuxdeploy-x86_64.AppImage" ] ||
    [ -x "$ROOT/tools/.cache/linuxdeploy" ]; then
    report ok "linuxdeploy" "builds the Linux AppImage"
else
    report missing-optional "linuxdeploy" "builds the Linux AppImage" \
        "tools/package_appimage.sh downloads it on first use"
fi

if [ -n "${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}" ] || [ -d "$HOME/Android/Sdk" ]; then
    report ok "Android SDK/NDK" "builds the APK (tools/build_android.sh)"
else
    report missing-optional "Android SDK/NDK" "builds the APK" \
        "./setup.sh android — SDK+NDK+system image+Qt kits, ~6 GB"
fi

if python3 -c "import reportlab" >/dev/null 2>&1; then
    report ok "reportlab" "renders the quality PDF (tools/make_report.py)"
else
    report missing-required "reportlab" "renders the quality PDF — a release needs it" \
        "python3 -m pip install reportlab"
fi
# StrictDoc is usually installed as a COMMAND (pipx / --user), which is not the same
# as being importable by this interpreter — checking only the import reported it
# missing on a machine where tools/make_requirements.sh works fine.
if have strictdoc || python3 -c "import strictdoc" >/dev/null 2>&1; then
    report ok "StrictDoc" "regenerates docs/requirements.md from the .sdoc source"
else
    report missing-optional "StrictDoc" "regenerates docs/requirements.md" \
        "python3 -m pip install strictdoc"
fi

# Windows and macOS are built by CI on a tag; say so rather than let a reader wonder
# why their Linux box cannot produce four platforms.
if [ "$QUIET" -eq 0 ]; then
    cat <<'EOF'

== the other platforms ==
  Windows (portable ZIP), Linux ARM64 (Raspberry Pi) and the signed Android APK are
  built by .github/workflows/release.yml when a v* tag is pushed — one runner per
  platform. Nothing on this machine can produce them all, and the release script does
  not pretend otherwise: it attaches what exists and names what is missing.

  On Windows, every script here has a PowerShell counterpart (setup.ps1,
  build_all.ps1, tools\*.ps1) and the substitutions are listed in docs/windows.md:
  OpenCppCoverage instead of gcov, no clazy, no TSan, no valgrind.
EOF
fi

printf '\n'
if [ "$MISSING_REQUIRED" -gt 0 ]; then
    printf '%s%d required tool(s) missing%s — %d optional/licence-bound\n' \
        "$RED" "$MISSING_REQUIRED" "$OFF" "$MISSING_OPTIONAL"
    exit 1
fi
if [ "$MISSING_OPTIONAL" -gt 0 ]; then
    printf '%severything required is present%s — %d optional/licence-bound tool(s) absent, ' \
        "$GREEN" "$OFF" "$MISSING_OPTIONAL"
    printf 'their stages will report SKIPPED\n'
    exit 0
fi
printf '%severy tool this pipeline can use is present%s\n' "$GREEN" "$OFF"
