#!/usr/bin/env bash
# Static analysis over the app AND test sources: cppcheck + clang-tidy + the
# Clang Static Analyzer + g++ -fanalyzer + code metrics (lizard) + copy-paste
# detection (PMD CPD), plus clazy (Qt coding rules) and codespell when
# installed. Reports land in analysis-results/ as one plain-text log per tool —
# the next axivion_ci run imports those onto the Axivion dashboard (see
# axivion/external_import.py) — plus one merged CSV as a single-file overview.
# Exit code 1 when any tool reported findings.
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
# The test sources are analysed like production code (tests/.clang-tidy exempts
# only what Qt Test's moc-driven layout forces).
SOURCES=("$ROOT"/src/domain/*.cpp "$ROOT"/src/services/*.cpp "$ROOT"/src/ui/*.cpp
    "$ROOT"/src/main.cpp "$ROOT"/tests/*.cpp)

if [ "$FIX" -eq 1 ]; then
    echo "== clang-tidy --fix (sequential: checks edit shared headers) =="
    for f in "${SOURCES[@]}"; do
        echo "fixing $(basename "$f")"
        clang-tidy -p "$ROOT/$BUILD_DIR" --fix "$f" >/dev/null 2>&1 || true
    done
    echo "auto-fixes applied — rebuild and rerun the tests"
fi

echo "== cppcheck ($(cppcheck --version)) =="
# Core flags: --project (compile database, so Qt include paths and defines match
# the real build), --enable=all (every check class, i.e. style and information
# on top of warning/performance/portability), --check-level=exhaustive (the
# deeper value-flow search; the default "normal" bails out early on big
# functions), --inconclusive, --error-exitcode=1 (any finding fails the run).
# On top: --library=qt (Qt function semantics), the id-scoped suppressions with
# their written rationale, and the pipe template that feeds the Axivion
# dashboard import. --checkers-report records which of cppcheck's ~590 checkers
# were actually active — evidence that the run was as strict as claimed.
# cppcheck's free ADDONS are deliberately not part of this gate. Measured over the
# real compile database (not a bare file list, which makes them bail out early and
# look clean):
#   misc-implicitlyVirtual      16  wants `virtual` repeated on an override that
#                                   already says `override` — the opposite of what
#                                   modern C++ and this codebase do
#   threadsafety-unsafe-call    19  getenv via QProcessEnvironment in Config::load,
#                                   which runs once at start-up before any thread
#                                   exists — a real property in the wrong context
#   findcasts-cast               7  an INVENTORY of casts at "information" severity;
#                                   not a defect list by design
# The MISRA addon is a category of its own — see tools/misra_cppcheck.sh, which runs
# all of these on demand with the numbers next to them.
# The test sources are analysed too; only the build tree is excluded (-i), which
# is where the moc/autogen output lives.
cppcheck --project="$ROOT/$BUILD_DIR/compile_commands.json" \
    --enable=all \
    --check-level=exhaustive \
    --inconclusive \
    --error-exitcode=1 \
    --inline-suppr \
    --suppressions-list="$ROOT/tools/cppcheck-suppressions.txt" \
    --library=qt \
    -i "$ROOT/$BUILD_DIR" \
    --checkers-report="$OUT/cppcheck-checkers.txt" \
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
# --extra-arg: the compile database is GCC's, so it carries GCC-only warning
# spellings (-Wduplicated-cond, -Wlogical-op …). clang reports each as an unknown
# warning option, which says nothing about the code.
TIDY_TMP="$(mktemp -d)"
printf '%s\n' "${SOURCES[@]}" | xargs -P "$(nproc)" -I{} sh -c '
    clang-tidy -p "$1" --extra-arg=-Wno-unknown-warning-option "$2" 2>/dev/null \
        | grep -E "warning:|error:" > "$0/$(basename "$2").log" || true' \
    "$TIDY_TMP" "$ROOT/$BUILD_DIR" {}
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

def run(entry, extra=()):
    # -Werror is dropped: it is a build policy (TRADINGAPP_WARNINGS_AS_ERRORS),
    # and with it every analyzer warning turns the exit code nonzero, which this
    # stage would then report as "the TU was never fully analyzed".
    args, skip = [], False
    for a in shlex.split(entry["command"]):
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-Werror" or a.startswith("-Werror="):
            continue
        args.append(a)
    args += ["-fanalyzer", *extra, "-o", "/dev/null"]
    try:
        r = subprocess.run(args, cwd=entry["directory"], capture_output=True,
                           text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return [f'{entry["file"]}|1|error|gcc-analyzer-timeout|analyzer timed out']
    if r.returncode != 0:
        # g++ exits 0 even with analyzer warnings, so nonzero means the TU was
        # never fully analyzed (ICE, or the kernel OOM-killed cc1plus). Surface
        # that as a finding — silently returning nothing would green-wash it.
        return [f'{entry["file"]}|1|error|gcc-analyzer-failed|'
                f'g++ -fanalyzer exited with {r.returncode} (OOM-killed?)']
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
        # GCC 13's analyzer cannot model std::optional's contained-value
        # lifetime: a lambda in a constexpr function-pointer table returning
        # std::optional<qint32> trips -Wanalyzer-null-argument on the
        # optional's internal copy ("use of NULL where non-null expected").
        # Audited 2026-08-01: 10/10 hits were the SignalEnsemble voter table,
        # which contains no pointer at all.
        if "-Wanalyzer-null-argument" in line and "SignalEnsemble.cpp" in line:
            continue
        keep.append(line)
    return keep

# The analyzer's memory scales hard with TU size: unbounded, the two big TUs
# peak at ~13 GB (src/ui/MainWindow.cpp) and ~8 GB (src/services/EtoroClient.cpp)
# on GCC 13 — one process per core OOMs a 16 GB CI runner, whose supervisor
# then SIGTERMs the whole step (exit 143, ~100 s in). So: small TUs (peaks in
# the hundreds of MB) run fully parallel at full precision; TUs over the size
# threshold run one at a time with the exploded-graph growth bounded, which
# brings MainWindow.cpp down to ~4.4 GB / 35 s with an identical finding set
# (measured 2026-07: 0 findings both ways). Full precision is simply not
# buyable for these TUs on any reasonable machine.
HEAVY_BYTES = 50_000
HEAVY_FLAGS = ("-fanalyzer-call-summaries",
               "--param", "analyzer-max-svalue-depth=6",
               "--param", "analyzer-max-enodes-per-program-point=4",
               "--param", "analyzer-bb-explosion-factor=2")
light = [e for e in entries if os.path.getsize(e["file"]) <= HEAVY_BYTES]
heavy = [e for e in entries if os.path.getsize(e["file"]) > HEAVY_BYTES]
lines = set()
with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
    for result in ex.map(run, light):
        lines.update(result)
for entry in heavy:
    lines.update(run(entry, HEAVY_FLAGS))
with open(out_path, "w") as f:
    f.write("\n".join(sorted(lines)) + ("\n" if lines else ""))
print(f"gcc-analyzer: {len(lines)} findings over {len(entries)} TUs")
EOF
GCCA_N=$(grep -c . "$OUT/gcc-analyzer.txt" || true)
echo "g++ -fanalyzer findings: $GCCA_N (analysis-results/gcc-analyzer.txt)"

echo "== Clang Static Analyzer =="
# The analyzer standalone, with the off-by-default checkers and a deeper search
# than clang-tidy's inline clang-analyzer-* checks can be given (clang-tidy has
# no way to pass -analyzer-config). Driver, checker list and the rationale for
# what is NOT enabled: tools/clang_analyzer.py. Shared with the Windows script.
python3 "$ROOT/tools/clang_analyzer.py" \
    "$ROOT/$BUILD_DIR/compile_commands.json" "$ROOT" "$OUT/clang-analyzer.txt"
CSA_RC=$?
if [ "$CSA_RC" -ne 0 ] && [ "$CSA_RC" -ne 3 ]; then
    echo "ERROR: clang_analyzer.py failed (rc=$CSA_RC)" >&2
    exit 1
fi
CSA_N=$(grep -c . "$OUT/clang-analyzer.txt" || true)
echo "clang-analyzer findings: $CSA_N (analysis-results/clang-analyzer.txt)"

echo "== lizard (code metrics) =="
# Cyclomatic complexity / function length / parameter count over src and tests.
# Gate is a ratchet against tools/lizard_baseline.json, so the finding count is
# deliberately NOT summed into TOTAL — the exit code is what decides (0 ok,
# 1 new or regressed debt, 3 lizard not installed). See tools/lizard_metrics.py.
python3 "$ROOT/tools/lizard_metrics.py" "$ROOT" "$OUT"
LIZARD_RC=$?
LIZARD_N=$(grep -c . "$OUT/lizard.txt" 2>/dev/null || true)

echo "== PMD CPD (copy-paste detection) =="
# Token-based clone detection; Axivion's configuration here is MISRA-only, so
# this is the project's only clone gate. See tools/cpd_scan.py.
python3 "$ROOT/tools/cpd_scan.py" "$ROOT" "$OUT/pmd-cpd.txt"
CPD_RC=$?
if [ "$CPD_RC" -ne 0 ] && [ "$CPD_RC" -ne 3 ]; then
    echo "ERROR: cpd_scan.py failed (rc=$CPD_RC)" >&2
    exit 1
fi
CPD_N=$(grep -c . "$OUT/pmd-cpd.txt" || true)
echo "pmd-cpd findings: $CPD_N (analysis-results/pmd-cpd.txt)"

echo "== objectName check (GUI-test addressability) =="
# A widget without a stable objectName can only be found by text or position, both
# of which a refactor breaks silently — and the Squish object map is built from these
# names (REQ-N-007).
OBJ_N=0
if python3 "$ROOT/tools/check_object_names.py" > "$OUT/object-names.txt" 2>&1; then
    echo "objectName findings: 0"
else
    OBJ_N=$(grep -c "has no objectName" "$OUT/object-names.txt" || true)
    echo "objectName findings: $OBJ_N (analysis-results/object-names.txt)"
fi

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

# Every stage above tolerates its own tool failing (|| true) — so before
# summing, prove the run actually produced all eight logs. A vanished
# analysis-results/ (e.g. a concurrent clean) once yielded "TOTAL: 0"/exit 0
# with half the logs missing: a green-washed non-run.
for f in cppcheck.txt clang-tidy.txt gcc-analyzer.txt clang-analyzer.txt \
    lizard.txt pmd-cpd.txt codespell.txt clazy.txt; do
    if [ ! -f "$OUT/$f" ]; then
        echo "ERROR: $OUT/$f missing — the analysis did not complete (concurrent clean?)" >&2
        exit 1
    fi
done

# Merged CSV for the dashboard import: tool;file;line;rule;severity;message.
# Shared with the Windows script (tools/static_analysis.ps1) so the two cannot
# drift apart on the merge format. Its failure must fail the run.
if ! python3 "$ROOT/tools/merge_findings.py" "$OUT"; then
    echo "ERROR: merge_findings.py failed — no external_findings.csv" >&2
    exit 1
fi

TOTAL=$((CPPCHECK_N + TIDY_N + CLAZY_N + GCCA_N + CSA_N + CPD_N + CODESPELL_N + OBJ_N))
echo "TOTAL findings: $TOTAL"
# The lizard metrics are reported separately: their violations are ratcheted
# against a recorded baseline, so a nonzero count is expected — the gate is the
# script's exit code, not the count.
echo "code metrics: $LIZARD_N over-threshold findings, ratchet $(
    case "$LIZARD_RC" in
    0) echo "clean" ;;
    3) echo "SKIPPED (lizard not installed)" ;;
    *) echo "FAILED — see the lizard GATE lines above" ;;
    esac)"
[ "$TOTAL" -eq 0 ] && [ "${CPPCHECK_RC:-0}" -eq 0 ] &&
    { [ "${LIZARD_RC:-0}" -eq 0 ] || [ "${LIZARD_RC:-0}" -eq 3 ]; }
