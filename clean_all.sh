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
    build-release
    build_axivion
    build-android
    build-ios
    dist
    test-results
    coverage
    analysis-results
    docs/html
    docs/traceability.html
    docs/strictdoc
)
DEEP=(
    .axivion-cache
    .fslckout
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
