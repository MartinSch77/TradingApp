#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Express the quality GATES as JUnit test cases, for Test Center.

    python3 tools/gates_to_junit.py            # -> test-results/quality-gates.xml
    python3 tools/gates_to_junit.py --print    # summary to stdout, write nothing

WHY THIS EXISTS, AND WHAT IT DELIBERATELY DOES NOT DO.

Test Center stores test results; it has no static-analysis format. The temptation is
to turn every analyzer FINDING into a failing test case, which would be wrong twice
over: it conflates a finding with a test, and on a codebase carrying 143,813 Axivion
style violations it would render every batch as catastrophically failing while saying
nothing about whether the build was acceptable.

What IS a pass/fail fact is the GATE. `tools/static_analysis.sh` either finds zero
cppcheck findings or it does not, and the analysis stage passes or fails on exactly
that. Encoding one case per gate is therefore not an invention — it is the same
verdict the pipeline already computes, in a shape Test Center can chart across
batches. The findings themselves ride along as the failure message, and the full
reports go up as batch attachments; the place to ANALYSE findings remains the Axivion
dashboard.

STALE EVIDENCE IS AN ERROR, NOT A PASS. An artefact older than the newest tracked
source describes code that no longer exists, and a green case built from it is worse
than no case at all — it is a confident lie. Such a gate is reported as an `error`
with the age difference, which is the same rule tools/publish_release.sh applies
before it will publish anything.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
ANALYSIS = ROOT / "analysis-results"
DEFAULT_OUT = ROOT / "test-results" / "quality-gates.xml"

# The gated analyzers: artefact -> the case name Test Center will show. Each file holds
# one finding per line in the shared `file|line|severity|rule|message` format, so the
# count is the line count and the gate is "zero".
FILE_GATES = [
    ("cppcheck.txt", "cppcheck", "seven-analyzers"),
    ("clang-tidy.txt", "clang-tidy", "seven-analyzers"),
    ("clang-analyzer.txt", "clang-static-analyzer", "seven-analyzers"),
    ("gcc-analyzer.txt", "gcc-fanalyzer", "seven-analyzers"),
    ("clazy.txt", "clazy", "seven-analyzers"),
    ("pmd-cpd.txt", "pmd-cpd-clones", "clones"),
    ("qmllint.txt", "qmllint", "qml"),
    ("codespell.txt", "codespell", "spelling"),
    ("object-names.txt", "gui-object-names", "gui-testability"),
    ("sanitize-asan-ubsan.txt", "asan-ubsan", "sanitizers"),
    ("sanitize-tsan.txt", "tsan", "sanitizers"),
    ("sanitize-valgrind.txt", "valgrind-memcheck", "sanitizers"),
]

# Gates that are SECONDS to re-run, so they are re-run rather than trusted — the same
# reasoning tools/publish_release.sh uses for these two.
RERUN_GATES = [
    ("lizard-metrics-ratchet", [sys.executable, "tools/lizard_metrics.py", ".",
                                "analysis-results"], "metrics"),
    ("requirements-traceability", [sys.executable, "tools/trace_report.py"], "traceability"),
]

MAX_MESSAGE_FINDINGS = 25   # enough to act on; a 500-line message helps nobody


class Case:
    def __init__(self, name: str, classname: str) -> None:
        self.name = name
        self.classname = classname
        self.status = "passed"        # passed | failed | error | skipped
        self.message = ""
        self.detail = ""
        self.count = 0


def newest_tracked_source_mtime() -> float | None:
    """The mtime of the newest tracked SOURCE file, or None when git cannot answer.

    Sources only — an artefact does not go stale because a build tree changed.

    THE PATHSPEC IS THE WHOLE POINT, and omitting it was a real defect. A bare
    `git ls-files` lists every tracked file, including ones this pipeline GENERATES
    and tracks: docs/requirements.md is regenerated from the .sdoc by
    tools/make_test_report.sh's own traceability step, which runs AFTER the analyzers.
    Measured 2026-08-07: that regeneration stamped docs/requirements.md at 14:08:50 and
    all eight analyzer artefacts — written minutes earlier — were then reported as
    "stale", i.e. the report chain invalidated evidence it had just collected. A
    regenerated document cannot change what cppcheck found.

    The pathspec below is deliberately IDENTICAL to the one tools/publish_release.sh
    uses for the same judgement. Two tools that disagree about whether the same evidence
    is stale are worse than either rule alone: one would refuse to publish while the
    other reported green.
    """
    try:
        listing = subprocess.run(["git", "-C", str(ROOT), "ls-files",
                                  "src/*", "tests/*",
                                  "CMakeLists.txt", "tests/CMakeLists.txt"],
                                 capture_output=True, text=True, check=True,
                                 timeout=60).stdout.split("\n")
    except (subprocess.SubprocessError, OSError):
        return None
    newest = None
    for rel in listing:
        if not rel:
            continue
        path = ROOT / rel
        try:
            stamp = path.stat().st_mtime
        except OSError:
            continue          # deleted but still tracked
        if newest is None or stamp > newest:
            newest = stamp
    return newest


# Artefacts whose file is NOT findings-only, mapped to the marker that identifies a real
# finding. Every analyzer here writes findings and nothing else, so "non-empty line" is a
# sound rule for them — except one.
#
# tools/check_object_names.py prints a human-readable SUCCESS sentence ("every member
# widget in src/ui has a stable objectName") and static_analysis.sh redirects its stdout
# into the artefact. Counting lines therefore reported 1 finding for a CLEAN check, every
# time — a permanent false red on a gate that was passing. static_analysis.sh itself never
# had the bug: it judges by exit code and only counts "has no objectName" lines. This maps
# the same marker so the two tools agree. Measured 2026-08-07.
FINDING_MARKERS = {
    "object-names.txt": "has no objectName",
}


def findings_of(path: pathlib.Path) -> list[str]:
    """The finding lines of an analyzer artefact.

    Non-empty lines, except for artefacts listed in FINDING_MARKERS — those carry
    human-readable prose alongside their findings and are filtered by marker.
    """
    try:
        lines = [ln for ln in path.read_text(encoding="utf-8", errors="replace").splitlines()
                 if ln.strip()]
    except OSError:
        return []
    marker = FINDING_MARKERS.get(path.name)
    if marker is not None:
        return [ln for ln in lines if marker in ln]
    return lines


def file_gate(artefact: str, name: str, group: str, newest: float | None) -> Case:
    case = Case(name, group)
    path = ANALYSIS / artefact
    if not path.is_file():
        case.status = "error"
        case.message = f"no artefact: analysis-results/{artefact} was never produced"
        case.detail = ("The stage that writes this file did not run, so there is no "
                       "verdict to report. A missing measurement is not a pass.")
        return case
    if newest is not None and path.stat().st_mtime < newest:
        age = (newest - path.stat().st_mtime) / 60.0
        case.status = "error"
        case.message = (f"stale artefact: analysis-results/{artefact} is {age:.0f} min "
                        f"older than the newest tracked source")
        case.detail = ("This file describes code that has since changed. Re-run the "
                       "stage; a green gate from stale evidence is a confident lie.")
        return case
    lines = findings_of(path)
    case.count = len(lines)
    if lines:
        case.status = "failed"
        case.message = f"{len(lines)} finding(s)"
        shown = lines[:MAX_MESSAGE_FINDINGS]
        case.detail = "\n".join(shown)
        if len(lines) > len(shown):
            case.detail += f"\n… and {len(lines) - len(shown)} more in {artefact}"
    return case


def rerun_gate(name: str, command: list[str], group: str) -> Case:
    case = Case(name, group)
    try:
        done = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True,
                              timeout=600)
    except subprocess.TimeoutExpired:
        case.status = "error"
        case.message = "timed out"
        return case
    except (subprocess.SubprocessError, OSError) as exc:
        case.status = "error"
        case.message = f"could not run: {exc}"
        return case
    output = (done.stdout + done.stderr).strip()
    # Exit 3 is this repository's "skipped", not a failure.
    if done.returncode == 3:
        case.status = "skipped"
        case.message = "skipped (tool or licence unavailable)"
    elif done.returncode != 0:
        case.status = "failed"
        case.message = f"gate failed (exit {done.returncode})"
    case.detail = "\n".join(output.splitlines()[-MAX_MESSAGE_FINDINGS:])
    return case


def build_suite(cases: list[Case]) -> ET.Element:
    failures = sum(1 for c in cases if c.status == "failed")
    errors = sum(1 for c in cases if c.status == "error")
    skipped = sum(1 for c in cases if c.status == "skipped")
    suite = ET.Element("testsuite", {
        "name": "quality-gates",
        "tests": str(len(cases)),
        "failures": str(failures),
        "errors": str(errors),
        "skipped": str(skipped),
    })
    for case in cases:
        node = ET.SubElement(suite, "testcase",
                             {"name": case.name, "classname": case.classname})
        if case.status == "failed":
            ET.SubElement(node, "failure", {"message": case.message}).text = case.detail
        elif case.status == "error":
            ET.SubElement(node, "error", {"message": case.message}).text = case.detail
        elif case.status == "skipped":
            ET.SubElement(node, "skipped", {"message": case.message})
        elif case.detail:
            ET.SubElement(node, "system-out").text = case.detail
    return suite


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    parser.add_argument("--print", dest="show", action="store_true",
                        help="summary only; write nothing")
    args = parser.parse_args()

    if not ANALYSIS.is_dir():
        print(f"no {ANALYSIS.relative_to(ROOT)} — run tools/static_analysis.sh first",
              file=sys.stderr)
        return 1

    newest = newest_tracked_source_mtime()
    cases = [file_gate(a, n, g, newest) for a, n, g in FILE_GATES]
    cases += [rerun_gate(n, c, g) for n, c, g in RERUN_GATES]

    width = max(len(c.name) for c in cases)
    for case in cases:
        mark = {"passed": "PASS", "failed": "FAIL", "error": "ERROR",
                "skipped": "SKIP"}[case.status]
        extra = f"  {case.message}" if case.message else ""
        print(f"  {mark:<5} {case.name:<{width}}{extra}")

    failed = [c for c in cases if c.status in ("failed", "error")]
    print(f"\n{len(cases) - len(failed)} of {len(cases)} gates green")

    if not args.show:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        tree = ET.ElementTree(build_suite(cases))
        ET.indent(tree, space="  ")
        tree.write(args.out, encoding="utf-8", xml_declaration=True)
        print(f"wrote {args.out.relative_to(ROOT)}")

    # Zero even when gates fail: this tool REPORTS, and the pipeline stages are what
    # gate the build. Exiting non-zero here would fail a run twice for one cause and
    # would stop the upload that carries the evidence of the failure.
    return 0


if __name__ == "__main__":
    sys.exit(main())
