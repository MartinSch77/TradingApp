#!/usr/bin/env bash
# Remove everything build_all.sh (and the tools/ scripts) generate: all build
# trees, test results, coverage and static-analysis reports, and the generated
# documentation. Everything here is reproducible with ./build_all.sh.
#
# Kept by default (pass --deep to remove them too):
#   .axivion-cache/ + .fslckout   Axivion incremental-analysis state — wiping it
#                                 forces the next Axivion run to re-analyze from
#                                 scratch and loses the local finding history
#   tools/third-party/            pinned plantuml.jar (tools/fetch_plantuml.sh
#                                 re-downloads it when the docs are built)
#
# Usage: ./clean_all.sh [--deep]
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

GENERATED=(
    build
    build-cov-gcc
    build-cov-mcdc
    build-cov-coco
    build-san
    build-san-tsan
    build-san-ubsan
    build-cov-msvc
    build-release
    build-vs
    build_axivion
    build-android
    build-appimage
    build-portable
    build-ios
    dist
    test-results
    coverage
    analysis-results
    docs/html
    docs/traceability.html
    docs/strictdoc
    docs/sphinx-html
)
DEEP=(
    .axivion-cache
    .fslckout
    _FOSSIL_ # the Windows name of the Axivion Shadow checkout database
    tools/third-party
)

TARGETS=("${GENERATED[@]}")
case "${1:-}" in
"") ;;
--deep) TARGETS+=("${DEEP[@]}") ;;
*)
    echo "usage: $0 [--deep]" >&2
    exit 2
    ;;
esac

# Pattern-matched leftovers (nullglob: nothing there = nothing added):
#   build-android-<abi>   the per-ABI Android build trees tools/build_android.sh
#                         creates (the fixed "build-android" entry above never
#                         matched them)
#   *.log                 stray tool logs at the repo root — aqt writes
#                         aqtinstall.log into the CWD on every Qt-kit install
#                         (setup, CI, release), and nothing else puts a .log here
shopt -s nullglob
for p in "$ROOT"/build-android-* "$ROOT"/*.log; do
    TARGETS+=("${p#"$ROOT"/}")
done
shopt -u nullglob

EXISTING=()
for p in "${TARGETS[@]}"; do
    [ -e "$ROOT/$p" ] && EXISTING+=("$p")
done

if [ ${#EXISTING[@]} -eq 0 ]; then
    echo "already clean — nothing to remove"
    exit 0
fi

(cd "$ROOT" && du -shc "${EXISTING[@]}" 2>/dev/null)
for p in "${EXISTING[@]}"; do
    rm -rf "${ROOT:?}/$p"
    echo "removed $p"
done
