#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Code metrics over the C++ sources with lizard (provider `lizard`).

Shared by tools/static_analysis.sh (Linux) and tools/static_analysis.ps1
(Windows) so the two platforms cannot drift apart on thresholds or on the
ratchet baseline.

What it produces in analysis-results/
  lizard-metrics.csv  every function, every metric — the raw measurement
                      (file;function;line;end;nloc;ccn;tokens;params)
  lizard.txt          the threshold violations in the pipe format the Axivion
                      dashboard import reads (file|line|severity|id|message)

Thresholds (industry-standard McCabe/size limits, see docs/verification.md):
  CCN     > 15   cyclomatic complexity per function
  NLOC    > 100  non-comment lines per function
  PARAMS  > 5    parameter count

The gate is a RATCHET, not a hard threshold: the functions that exceed a
threshold today are recorded with their current numbers in
tools/lizard_baseline.json (regenerate with --update-baseline). The run fails
when

  * a function not in the baseline exceeds a threshold        (new debt)
  * a baselined function gets worse than its recorded number  (regression)
  * a baselined function no longer exceeds any threshold      (stale entry —
    delete the line, that is how the ratchet tightens)
  * a baselined function has disappeared                      (stale entry)

Every violation is reported to the dashboard either way, baselined or not, so
the debt stays visible instead of being silently accepted.

Usage: lizard_metrics.py <project-root> <analysis-results-dir> [--update-baseline]
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

CCN_LIMIT = 15
NLOC_LIMIT = 100
PARAM_LIMIT = 5

# lizard --csv columns, in order (lizard 1.17+):
#   nloc, ccn, token_count, param_count, length, location, file, name,
#   long_name, start_line, end_line
_CSV_NLOC, _CSV_CCN, _CSV_TOKENS, _CSV_PARAMS = 0, 1, 2, 3
_CSV_FILE, _CSV_NAME = 6, 7
_CSV_START, _CSV_END = 9, 10

SCAN_DIRS = ("src", "tests")
BASELINE = Path("tools") / "lizard_baseline.json"


def _lizard_command() -> list[str]:
    """lizard as an executable, or through the interpreter that has the module.

    pipx puts `lizard` on PATH (Linux/Windows alike); a plain `pip install
    --user lizard` may only leave the module importable, which the Windows
    Store Python does.
    """
    exe = shutil.which("lizard")
    if exe:
        return [exe]
    probe = subprocess.run([sys.executable, "-m", "lizard", "--version"],
                           capture_output=True, text=True)
    if probe.returncode == 0:
        return [sys.executable, "-m", "lizard"]
    return []


def _measure(root: Path, command: list[str]) -> list[list[str]]:
    """One lizard run over src/ and tests/, parsed from its CSV output."""
    targets = [str(root / d) for d in SCAN_DIRS if (root / d).is_dir()]
    run = subprocess.run(command + ["--csv", "--languages", "cpp", *targets],
                         capture_output=True, text=True, cwd=root)
    # lizard exits nonzero when its own (default) thresholds are exceeded — the
    # CSV on stdout is still complete, so only an empty stdout is a real error.
    if not run.stdout.strip():
        raise SystemExit(f"lizard produced no output (rc={run.returncode}): "
                         f"{run.stderr.strip()[:400]}")
    rows = []
    for line in run.stdout.splitlines():
        fields = next(__import__("csv").reader([line]))
        if len(fields) > _CSV_END and fields[_CSV_CCN].isdigit():
            rows.append(fields)
    if not rows:
        raise SystemExit("lizard parsed no functions — CSV format changed?")
    return rows


def _key(root: Path, row: list[str]) -> str:
    """Baseline key: repo-relative file plus function name.

    Deliberately without the line number, so moving a function inside its file
    does not invalidate its baseline entry.
    """
    path = Path(row[_CSV_FILE])
    try:
        rel = path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        rel = path.as_posix()
    return f"{rel}::{row[_CSV_NAME]}"


def _violations(row: list[str]) -> list[tuple[str, int, int]]:
    """(metric, measured, limit) for every threshold this function exceeds."""
    found = []
    for metric, index, limit in (("ccn", _CSV_CCN, CCN_LIMIT),
                                 ("nloc", _CSV_NLOC, NLOC_LIMIT),
                                 ("params", _CSV_PARAMS, PARAM_LIMIT)):
        value = int(row[index])
        if value > limit:
            found.append((metric, value, limit))
    return found


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    update = "--update-baseline" in sys.argv[1:]
    if len(args) != 2:
        sys.exit(__doc__)
    root, out = Path(args[0]).resolve(), Path(args[1])
    out.mkdir(parents=True, exist_ok=True)

    command = _lizard_command()
    if not command:
        print("lizard not installed (pipx install lizard) — metrics skipped")
        (out / "lizard.txt").write_text("", encoding="utf-8")
        return 3  # the pipeline's "stage skipped" code

    rows = _measure(root, command)

    # --- the raw measurement, every function ---------------------------------
    csv_lines = ["file;function;line;end;nloc;ccn;tokens;params"]
    for row in sorted(rows, key=lambda r: (-int(r[_CSV_CCN]), r[_CSV_FILE])):
        rel = _key(root, row).split("::", 1)[0]
        csv_lines.append(";".join((rel, row[_CSV_NAME], row[_CSV_START],
                                   row[_CSV_END], row[_CSV_NLOC], row[_CSV_CCN],
                                   row[_CSV_TOKENS], row[_CSV_PARAMS])))
    (out / "lizard-metrics.csv").write_text("\n".join(csv_lines) + "\n",
                                            encoding="utf-8")

    # --- threshold violations, dashboard format ------------------------------
    measured: dict[str, dict[str, int]] = {}
    findings, seen = [], set()
    for row in rows:
        hits = _violations(row)
        if not hits:
            continue
        key = _key(root, row)
        measured[key] = {metric: value for metric, value, _ in hits}
        for metric, value, limit in hits:
            findings.append(
                f"{key.split('::', 1)[0]}|{row[_CSV_START]}|warning|"
                f"lizard-{metric}|{row[_CSV_NAME]} has {metric.upper()} {value} "
                f"(limit {limit}): nloc={row[_CSV_NLOC]} ccn={row[_CSV_CCN]} "
                f"params={row[_CSV_PARAMS]}")
        seen.add(key)
    (out / "lizard.txt").write_text("\n".join(sorted(findings)) +
                                    ("\n" if findings else ""), encoding="utf-8")

    baseline_path = root / BASELINE
    if update:
        baseline_path.write_text(json.dumps(measured, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
        print(f"lizard: baseline updated — {len(measured)} functions over threshold")
        return 0

    baseline = json.loads(baseline_path.read_text(encoding="utf-8")) if baseline_path.is_file() else {}
    live = {_key(root, row) for row in rows}

    problems = []
    for key, values in sorted(measured.items()):
        allowed = baseline.get(key)
        if allowed is None:
            problems.append(f"NEW over-threshold function: {key} "
                            f"({', '.join(f'{m}={v}' for m, v in values.items())})")
            continue
        for metric, value in values.items():
            budget = allowed.get(metric)
            if budget is None:
                problems.append(f"NEW {metric.upper()} violation in {key}: {value}")
            elif value > budget:
                problems.append(f"{metric.upper()} regressed in {key}: "
                                f"{value} > baseline {budget}")
    for key in sorted(baseline):
        if key not in live:
            problems.append(f"stale baseline entry (function gone): {key} "
                            f"— remove it from {BASELINE.as_posix()}")
        elif key not in seen:
            problems.append(f"stale baseline entry (now under every threshold): {key} "
                            f"— remove it from {BASELINE.as_posix()}, that is the ratchet")

    total_nloc = sum(int(r[_CSV_NLOC]) for r in rows)
    worst = max(rows, key=lambda r: int(r[_CSV_CCN]))
    print(f"lizard: {len(rows)} functions, {total_nloc} nloc, "
          f"max CCN {worst[_CSV_CCN]} ({worst[_CSV_NAME]}), "
          f"{len(seen)} over threshold ({len(baseline)} baselined)")
    print(f"lizard findings: {len(findings)} (analysis-results/lizard.txt, "
          f"metrics in analysis-results/lizard-metrics.csv)")
    for problem in problems:
        print(f"lizard GATE: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
