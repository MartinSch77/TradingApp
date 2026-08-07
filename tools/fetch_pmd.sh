#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Fetch the pinned PMD release used for copy-paste detection (tools/cpd_scan.py).
# The dist is not committed (tools/third-party/ is git-ignored); see
# docs/tools.md for the tool inventory. Keep $VERSION in sync with
# tools/fetch_pmd.ps1.
set -euo pipefail
VERSION="7.19.0"
DEST_DIR="$(cd "$(dirname "$0")" && pwd)/third-party"
TARGET="$DEST_DIR/pmd-bin-$VERSION"
mkdir -p "$DEST_DIR"

if [ -x "$TARGET/bin/pmd" ]; then
    echo "pmd $VERSION already present"
else
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    curl -sL -o "$TMP/pmd.zip" \
        "https://github.com/pmd/pmd/releases/download/pmd_releases%2F${VERSION}/pmd-dist-${VERSION}-bin.zip"
    # Older dists unpack straight into pmd-bin-<version>/; unzip into the
    # third-party dir and keep only that one directory.
    unzip -q -o "$TMP/pmd.zip" -d "$DEST_DIR"
    # Drop any other PMD version so tools/cpd_scan.py cannot pick up a stale one.
    for old in "$DEST_DIR"/pmd-bin-*; do
        [ "$old" = "$TARGET" ] || rm -rf "$old"
    done
fi

if command -v java >/dev/null 2>&1; then
    "$TARGET/bin/pmd" --version
else
    echo "java not found — PMD was downloaded but cannot run" >&2
fi
