#!/usr/bin/env bash
# Static analysis over the app sources: cppcheck + clang-tidy, plus clazy
# (Qt coding rules) when installed. Reports land in analysis-results/ as
# plain-text logs and one merged CSV that axivion/import_external.py converts
# for the Axivion dashboard. Exit code 1 when any tool reported findings.
#
# Usage: tools/static_analysis.sh [build-dir]   (needs compile_commands.json;
#        configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
set -uo pipefail

BUILD_DIR="${1:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/analysis-results"
mkdir -p "$OUT"
SOURCES=("$ROOT"/src/domain/*.cpp "$ROOT"/src/services/*.cpp "$ROOT"/src/ui/*.cpp "$ROOT"/src/main.cpp)

echo "== cppcheck ($(cppcheck --version)) =="
# --project uses the compile database so Qt include paths and defines match
# the real build; the template makes the CSV merge below trivial.
cppcheck --project="$ROOT/$BUILD_DIR/compile_commands.json" \
    --enable=warning,performance,portability --inline-suppr \
    --suppressions-list="$ROOT/tools/cppcheck-suppressions.txt" \
    --library=qt --inconclusive \
    -i "$ROOT/$BUILD_DIR" --suppress='*:*autogen*' --suppress='*:*/tests/*' \
    --template='{file}|{line}|{severity}|{id}|{message}' \
    --output-file="$OUT/cppcheck.txt" --quiet
CPPCHECK_N=$(grep -c . "$OUT/cppcheck.txt" || true)
echo "cppcheck findings: $CPPCHECK_N (analysis-results/cppcheck.txt)"

echo "== clang-tidy ($(clang-tidy --version | head -1)) =="
: > "$OUT/clang-tidy.txt"
for f in "${SOURCES[@]}"; do
    clang-tidy -p "$ROOT/$BUILD_DIR" "$f" 2>/dev/null \
        | grep -E "warning:|error:" >> "$OUT/clang-tidy.txt" || true
done
sort -u "$OUT/clang-tidy.txt" -o "$OUT/clang-tidy.txt"
TIDY_N=$(grep -c . "$OUT/clang-tidy.txt" || true)
echo "clang-tidy findings: $TIDY_N (analysis-results/clang-tidy.txt)"

CLAZY_N=0
if command -v clazy-standalone >/dev/null 2>&1; then
    echo "== clazy ($(clazy-standalone --version 2>&1 | head -1)) =="
    : > "$OUT/clazy.txt"
    for f in "${SOURCES[@]}"; do
        clazy-standalone -p "$ROOT/$BUILD_DIR" \
            -checks=level0,level1 "$f" 2>&1 \
            | grep -E "warning:.*\[-Wclazy" >> "$OUT/clazy.txt" || true
    done
    sort -u "$OUT/clazy.txt" -o "$OUT/clazy.txt"
    CLAZY_N=$(grep -c . "$OUT/clazy.txt" || true)
    echo "clazy findings: $CLAZY_N (analysis-results/clazy.txt)"
else
    echo "== clazy: NOT INSTALLED (apt install clazy) — Qt coding rules skipped =="
    printf '' > "$OUT/clazy.txt"
fi

# Merged CSV for the dashboard import: tool;file;line;id;severity;message
python3 - "$OUT" <<'EOF'
import csv, re, sys
from pathlib import Path
out = Path(sys.argv[1])
rows = []
for line in (out / "cppcheck.txt").read_text().splitlines():
    parts = line.split("|", 4)
    if len(parts) == 5:
        rows.append(["cppcheck", parts[0], parts[1], parts[3], parts[2], parts[4]])
pat = re.compile(r"^(.*?):(\d+):\d+:\s+(warning|error):\s+(.*?)\s+\[(.*)\]$")
for name in ("clang-tidy", "clazy"):
    for line in (out / f"{name}.txt").read_text().splitlines():
        m = pat.match(line)
        if m:
            rows.append([name, m.group(1), m.group(2), m.group(5), m.group(3), m.group(4)])
with open(out / "external_findings.csv", "w", newline="") as f:
    w = csv.writer(f, delimiter=";")
    w.writerow(["tool", "file", "line", "rule", "severity", "message"])
    w.writerows(rows)
print(f"merged: {len(rows)} findings -> analysis-results/external_findings.csv")
EOF

TOTAL=$((CPPCHECK_N + TIDY_N + CLAZY_N))
echo "TOTAL findings: $TOTAL"
[ "$TOTAL" -eq 0 ]
