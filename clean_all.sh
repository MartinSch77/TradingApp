#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

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
    # build-cov and build-cov-coco-gui were both missing here. Measured 2026-08-07: a
    # build-cov tree from an earlier ad-hoc run survived a full clean_all (480 MB) and the
    # GUI-coverage tree survived too (518 MB). A "clean" that leaves a gigabyte of stale
    # build trees behind is not one, and a stale tree is exactly what makes a later
    # measurement describe code that no longer exists.
    build-cov
    build-cov-gcc
    build-cov-mcdc
    build-cov-coco
    build-cov-coco-gui
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
    downloads # packaged artifacts (AppImage/APK/zip/report) — every producer re-creates it
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
