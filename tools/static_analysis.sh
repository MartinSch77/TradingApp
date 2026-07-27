#!/usr/bin/env bash
# Static analysis over the app sources: cppcheck + clang-tidy, plus clazy
# (Qt coding rules) when installed. Reports land in analysis-results/ as one
# plain-text log per tool — the next axivion_ci run imports those onto the
# Axivion dashboard (see axivion/external_import.py) — plus one merged CSV as
# a single-file overview. Exit code 1 when any tool reported findings.
#
# Usage: tools/static_analysis.sh [build-dir] [--fix]
#        (needs compile_commands.json; configure with
#        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
# --fix: first apply clang-tidy's automatic fixes (sequentially — the checks
#        edit shared headers), then run the normal analysis pass over the
#        fixed code. Rebuild and rerun the tests afterwards!
set -uo pipefail

FIX=0
ARGS=()
for a in "$@"; do
    if [ "$a" = "--fix" ]; then FIX=1; else ARGS+=("$a"); fi
done
BUILD_DIR="${ARGS[0]:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/analysis-results"
mkdir -p "$OUT"
SOURCES=("$ROOT"/src/domain/*.cpp "$ROOT"/src/services/*.cpp "$ROOT"/src/ui/*.cpp "$ROOT"/src/main.cpp)

if [ "$FIX" -eq 1 ]; then
    echo "== clang-tidy --fix (sequential: checks edit shared headers) =="
    for f in "${SOURCES[@]}"; do
        echo "fixing $(basename "$f")"
        clang-tidy -p "$ROOT/$BUILD_DIR" --fix "$f" >/dev/null 2>&1 || true
    done
    echo "auto-fixes applied — rebuild and rerun the tests"
fi

echo "== cppcheck ($(cppcheck --version)) =="
# Core flags: --project (compile database, so Qt include paths and defines
# match the real build), --enable=warning,performance,portability,
# --inconclusive, --error-exitcode=1 (any finding fails the run). On top:
# --library=qt (Qt function semantics), the autogen/tests suppressions (moc
# noise) and the pipe template that feeds the Axivion dashboard import.
cppcheck --project="$ROOT/$BUILD_DIR/compile_commands.json" \
    --enable=warning,performance,portability \
    --inconclusive \
    --error-exitcode=1 \
    --inline-suppr \
    --suppressions-list="$ROOT/tools/cppcheck-suppressions.txt" \
    --library=qt \
    -i "$ROOT/$BUILD_DIR" --suppress='*:*autogen*' --suppress='*:*/tests/*' \
    --template='{file}|{line}|{severity}|{id}|{message}' \
    --output-file="$OUT/cppcheck.txt" --quiet
CPPCHECK_RC=$?
# No output file = cppcheck never analyzed anything (e.g. the compile DB refers
# to missing moc autogen files). That must fail loudly — an empty-but-present
# file is the legitimate "ran and found nothing".
if [ ! -f "$OUT/cppcheck.txt" ]; then
    echo "cppcheck did not run: project load failed — build the compile-DB build dir first" >&2
    exit 1
fi
CPPCHECK_N=$(grep -c . "$OUT/cppcheck.txt" || true)
echo "cppcheck findings: $CPPCHECK_N (analysis-results/cppcheck.txt)"

echo "== clang-tidy ($(clang-tidy --version | head -1)) =="
# One process per source file, in parallel; per-file temp logs keep the
# concurrent output from interleaving mid-line.
TIDY_TMP="$(mktemp -d)"
printf '%s\n' "${SOURCES[@]}" | xargs -P "$(nproc)" -I{} sh -c '
    clang-tidy -p "$1" "$2" 2>/dev/null | grep -E "warning:|error:" \
        > "$0/$(basename "$2").log" || true' "$TIDY_TMP" "$ROOT/$BUILD_DIR" {}
cat "$TIDY_TMP"/*.log | sort -u > "$OUT/clang-tidy.txt"
rm -rf "$TIDY_TMP"
TIDY_N=$(grep -c . "$OUT/clang-tidy.txt" || true)
echo "clang-tidy findings: $TIDY_N (analysis-results/clang-tidy.txt)"

echo "== g++ -fanalyzer ($(g++ -dumpfullversion)) =="
# GCC's symbolic-execution analyzer over every project TU, flags taken from
# the compile database (objects to /dev/null, one process per core). Upstream
# marks C++ support experimental: diagnostics without a project file:line
# (cc1plus-attributed Qt-header noise) are dropped; the rest is GCC-style and
# feeds the dashboard import as provider "gcc-analyzer".
python3 - "$ROOT/$BUILD_DIR/compile_commands.json" "$ROOT" "$OUT/gcc-analyzer.txt" <<'EOF'
import concurrent.futures as cf
import json, os, re, shlex, subprocess, sys

db_path, root, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
entries = [e for e in json.load(open(db_path))
           if e["file"].startswith(os.path.join(root, "src") + os.sep)]
located = re.compile(r"^(/[^:]+):(\d+):(\d+): warning: .*\[-Wanalyzer-[^\]]+\]$")

def run(entry):
    args, skip = [], False
    for a in shlex.split(entry["command"]):
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        args.append(a)
    args += ["-fanalyzer", "-o", "/dev/null"]
    try:
        r = subprocess.run(args, cwd=entry["directory"], capture_output=True,
                           text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return [f'{entry["file"]}|1|error|gcc-analyzer-timeout|analyzer timed out']
    keep = []
    for line in r.stderr.splitlines():
        m = located.match(line)
        if not m or not m.group(1).startswith(root + os.sep):
            continue
        # The experimental C++ analyzer reports "uninitialized" placeholder
        # values ('<unknown>', '<unnamed>') for Qt/std internals it cannot
        # model — every audited instance was a false positive (e.g. members
        # WITH default initializers). Findings naming a concrete variable stay.
        if "‘<unknown>’" in line or "<unnamed>" in line:
            continue
        # Throwing `operator new` can never return null; the analyzer models
        # the nothrow variant and flags every `new Widget(...)` argument as
        # possibly-NULL — a documented false-positive class in C++ mode.
        if "possibly-NULL" in line and "operator new" in line:
            continue
        keep.append(line)
    return keep

lines = set()
with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
    for result in ex.map(run, entries):
        lines.update(result)
with open(out_path, "w") as f:
    f.write("\n".join(sorted(lines)) + ("\n" if lines else ""))
print(f"gcc-analyzer: {len(lines)} findings over {len(entries)} TUs")
EOF
GCCA_N=$(grep -c . "$OUT/gcc-analyzer.txt" || true)
echo "g++ -fanalyzer findings: $GCCA_N (analysis-results/gcc-analyzer.txt)"

CODESPELL_N=0
if command -v codespell >/dev/null 2>&1; then
    echo "== codespell ($(codespell --version 2>&1)) =="
    # Typos in comments, docs and scripts; config in .codespellrc. Output is
    # normalized to the pipe format so it lands on the Axivion dashboard.
    (cd "$ROOT" && codespell src tests docs/*.md tools *.md *.sh requirements .github .claude 2>/dev/null) \
        | sed -E 's#^([^:]+):([0-9]+): (.*)$#\1|\2|warning|codespell|\3#' \
        > "$OUT/codespell.txt" || true
    CODESPELL_N=$(grep -c . "$OUT/codespell.txt" || true)
    echo "codespell findings: $CODESPELL_N (analysis-results/codespell.txt)"
else
    echo "== codespell: not installed (pipx install codespell) — typo check skipped =="
    printf '' > "$OUT/codespell.txt"
fi

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
    # Not a coverage gap: Axivion's Qt-* ruleset (~180 rules incl. the clazy
    # checks, active in axivion/rule_config.json) already checks the Qt coding
    # rules on every axivion_ci run; clazy here would only add a second opinion.
    echo "== clazy: not installed — skipped (Qt rules covered by Axivion's Qt-* ruleset; for a second opinion: sudo apt install clazy) =="
    printf '' > "$OUT/clazy.txt"
fi

# Merged CSV for the dashboard import: tool;file;line;id;severity;message.
# Shared with the Windows script (tools/static_analysis.ps1) so the two cannot
# drift apart on the merge format.
python3 "$ROOT/tools/merge_findings.py" "$OUT"

TOTAL=$((CPPCHECK_N + TIDY_N + CLAZY_N + GCCA_N + CODESPELL_N))
echo "TOTAL findings: $TOTAL"
[ "$TOTAL" -eq 0 ] && [ "${CPPCHECK_RC:-0}" -eq 0 ]
