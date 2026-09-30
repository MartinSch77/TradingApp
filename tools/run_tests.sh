#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Run the whole test suite and record per-test-function results as JUnit XML
# in test-results/ — the result leg of the traceability chain
# (tools/trace_report.py joins them with the specs). ctest alone only records
# pass/fail per executable, so each Qt Test binary is run with its own
# junitxml writer.
#
# Usage: tools/run_tests.sh [build-dir]     (default: build)
set -euo pipefail

BUILD_DIR="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/test-results"
mkdir -p "$OUT"
rm -f "$OUT"/*.xml

FAIL=0
for exe in "$ROOT/$BUILD_DIR"/tests/tst_*; do
    [ -f "$exe" ] && [ -x "$exe" ] || continue # skip the *_autogen directories
    name="$(basename "$exe")"
    # A Linux test binary has no filename extension. Everything else matching
    # tst_* is a build by-product — and the -x test above does not filter them
    # out on a DrvFs/9p mount (/mnt/c under WSL), where every file appears
    # executable. Without this, a Windows build left in the same tree makes the
    # loop "execute" tst_x.exe.manifest as a shell script and no results at all
    # get recorded.
    case "$name" in *.*) continue ;; esac
    echo "=== $name ==="
    if ! "$exe" -o "$OUT/$name.xml,junitxml" -o -,txt; then
        FAIL=1
        # Name the failing functions from the JUnit file too — the same summary the
        # Windows runner prints, where Qt Test's own text report never reaches the
        # CI console; here it is a second, grep-able line per failure.
        python3 - "$OUT/$name.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
try:
    root = ET.parse(sys.argv[1]).getroot()
except Exception as exc:  # noqa: BLE001 - a report that cannot be read is named, not hidden
    print(f"  (unreadable JUnit file: {exc})"); sys.exit(0)
for case in root.iter("testcase"):
    node = case.find("failure") if case.find("failure") is not None else case.find("error")
    if node is not None:
        msg = " ".join((node.get("message") or node.text or "").split())
        print(f"  FAIL {case.get('name')}: {msg}")
PY
    fi
done

echo
echo "JUnit results in $OUT/"
exit $FAIL
