#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Reproducibility check for the Linux AppImage (tooling backlog item 5): builds
# TWO fully independent AppImages from the SAME source tree and toolchain — own
# build tree, own output directory each — and compares them, both as a whole
# file and content-by-content inside the SquashFS. This is the actual claim a
# supply-chain-conscious release wants evidence for: "given this source and this
# toolchain, the packaging pipeline produces the same bytes twice," not merely
# "the script ran without an error."
#
# SOURCE_DATE_EPOCH is pinned to HEAD's own commit time (the standard
# reproducible-builds.org convention) so that whichever tools in the chain
# respect it (mksquashfs inside linuxdeploy/appimagetool, some compiler/linker
# outputs) see the SAME clock on both runs instead of "now" twice.
#
# DELIBERATELY INFORMATIONAL, not a gate — same "measured, not yet enforced"
# stance as Mull/fuzzing: full bit-for-bit reproducibility of a real Qt/C++
# binary is a genuine, ongoing effort (ELF build-ids, embedded absolute paths,
# archive member ordering), and this script's job is to say HONESTLY how close
# the pipeline already is and name exactly what differs — not to pretend it is
# solved. Exits 3 ("skipped") when the AppImage packaging's own prerequisites
# (a Qt kit with Charts, linuxdeploy) are not available, same convention as
# tools/package_appimage.sh itself.
#
# Usage: tools/check_reproducibility.sh

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

EXIT_SKIPPED=3
WORK="$ROOT/build-repro"
OUT_DIR="${REPRO_OUT_DIR:-$ROOT/analysis-results}"
mkdir -p "$OUT_DIR"
REPORT="$OUT_DIR/reproducibility.txt"

SOURCE_DATE_EPOCH="$(git log -1 --format=%ct 2>/dev/null || date +%s)"
export SOURCE_DATE_EPOCH

echo "== reproducibility check: two independent AppImage builds (SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH) =="

rm -rf "$WORK"
mkdir -p "$WORK/a/downloads" "$WORK/b/downloads"

build_one() {
    local label="$1" build_dir="$2" out_dir="$3"
    echo "-- build $label: $build_dir -> $out_dir"
    APPIMAGE_BUILD_DIR="$build_dir" APPIMAGE_OUT_DIR="$out_dir" \
        "$ROOT/tools/package_appimage.sh"
}

build_one A "$WORK/a/build-appimage" "$WORK/a/downloads" >"$WORK/build-a.log" 2>&1
status_a=$?
if [ $status_a -ne 0 ]; then
    if grep -q "no Qt kit with the Charts module found" "$WORK/build-a.log"; then
        echo "check_reproducibility: no Qt kit with Charts — skipped" >&2
        tail -5 "$WORK/build-a.log" >&2
        exit $EXIT_SKIPPED
    fi
    echo "build A failed — see $WORK/build-a.log" >&2
    tail -30 "$WORK/build-a.log" >&2
    exit 1
fi

build_one B "$WORK/b/build-appimage" "$WORK/b/downloads" >"$WORK/build-b.log" 2>&1
status_b=$?
if [ $status_b -ne 0 ]; then
    echo "build B failed — see $WORK/build-b.log" >&2
    tail -30 "$WORK/build-b.log" >&2
    exit 1
fi

APPIMAGE_A="$(find "$WORK/a/downloads" -maxdepth 1 -name '*.AppImage' | head -1)"
APPIMAGE_B="$(find "$WORK/b/downloads" -maxdepth 1 -name '*.AppImage' | head -1)"
if [ -z "$APPIMAGE_A" ] || [ -z "$APPIMAGE_B" ]; then
    echo "one of the two builds produced no .AppImage — see $WORK/build-{a,b}.log" >&2
    exit 1
fi

{
    echo "Reproducibility check: $(date -u +%FT%TZ)"
    echo "SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH"
    echo "A: $APPIMAGE_A"
    echo "B: $APPIMAGE_B"
    echo
} | tee "$REPORT"

SHA_A="$(sha256sum "$APPIMAGE_A" | awk '{print $1}')"
SHA_B="$(sha256sum "$APPIMAGE_B" | awk '{print $1}')"
{
    echo "sha256 A: $SHA_A"
    echo "sha256 B: $SHA_B"
} | tee -a "$REPORT"

if [ "$SHA_A" = "$SHA_B" ]; then
    echo "REPRODUCIBLE: byte-identical AppImage on two independent builds." | tee -a "$REPORT"
    exit 0
fi

echo "NOT byte-identical — extracting both to compare contents" | tee -a "$REPORT"

extract_one() {
    local appimage="$1" dest="$2"
    mkdir -p "$dest"
    (cd "$dest" && APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" --appimage-extract >/dev/null 2>&1)
}
extract_one "$APPIMAGE_A" "$WORK/extract-a"
extract_one "$APPIMAGE_B" "$WORK/extract-b"

{
    echo
    echo "-- files differing inside the SquashFS (diff -rq) --"
} | tee -a "$REPORT"
diff -rq "$WORK/extract-a/squashfs-root" "$WORK/extract-b/squashfs-root" 2>&1 | tee -a "$REPORT"

echo "wrote $REPORT"
exit 0
