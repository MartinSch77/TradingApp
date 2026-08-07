#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Record WHICH tool produced each analyzer verdict, as analysis-results/tool-versions.json.

    python3 tools/record_tool_versions.py [analysis-results]

WHY THIS EXISTS. The analysis stage prints its tool versions to the console — "cppcheck
(Cppcheck 2.13.0)", "clang-tidy (Ubuntu LLVM version 18.1.3)" — and nowhere else. The
console is transient, so the qualification bundle could show "cppcheck: 0 findings" while
being unable to answer WHICH cppcheck produced that zero, with what flags, into which
file. For evidence meant to outlive the run that is backwards: a verdict without tool
identity is not traceable.

This writes one JSON record per tool — name, version, invocation, output artefact — beside
the findings themselves, so tools/make_report.py can render a tool -> version -> invocation
-> artefact -> result table from artefacts alone.

WHY PYTHON RATHER THAN THE SHELL. The first attempt built this JSON with printf in
static_analysis.sh and produced invalid JSON: `printf '%s' ',\\n'` writes a literal
backslash-n, not a newline, so the separator corrupted the document. Quoting tool version
strings safely in shell is fiddly for no benefit. As a shared Python tool this is also
called unchanged by static_analysis.ps1, so the two platforms cannot drift.

A tool that is not installed is recorded as "not installed" rather than omitted: the
report must be able to say that a check did not run, which is different from saying it
found nothing.
"""

from __future__ import annotations

import json
import pathlib
import shutil
import subprocess
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent.parent

# tool label, version command, invocation summary, artefact (repo-relative)
TOOLS = [
    ("cppcheck", ["cppcheck", "--version"],
     "--project=compile_commands.json --enable=all --check-level=exhaustive "
     "--inconclusive --library=qt",
     "analysis-results/cppcheck.txt"),
    ("clang-tidy", ["clang-tidy", "--version"],
     "over the compile database, config in .clang-tidy, warnings are errors",
     "analysis-results/clang-tidy.txt"),
    ("g++ -fanalyzer", ["g++", "-dumpfullversion"],
     "-fanalyzer over every TU; experimental-C++ <unknown> reports filtered",
     "analysis-results/gcc-analyzer.txt"),
    ("Clang Static Analyzer", ["clang", "--version"],
     "tools/clang_analyzer.py, 11 extra checkers beyond the defaults",
     "analysis-results/clang-analyzer.txt"),
    ("clazy", ["clazy-standalone", "--version"],
     "Qt-specific checks over the compile database",
     "analysis-results/clazy.txt"),
    ("lizard", ["lizard", "--version"],
     "tools/lizard_metrics.py against tools/lizard_baseline.json (a ratchet, not a "
     "threshold)",
     "analysis-results/lizard.txt"),
    ("codespell", ["codespell", "--version"],
     "over tracked sources and documentation",
     "analysis-results/codespell.txt"),
    ("qmllint", ["qmllint", "--version"],
     "-i <generated qmldir> over src/quick/qml — Qt's own QML static analysis",
     "analysis-results/qmllint.txt"),
    ("valgrind", ["valgrind", "--version"],
     "memcheck over every test binary, tools/valgrind.supp",
     "analysis-results/sanitize-valgrind.txt"),
]


def first_line(cmd: list[str]) -> str:
    """The tool's version line, or "not installed" when it is absent.

    Never raises: a missing tool is a fact to record, not a failure of this script.
    """
    if shutil.which(cmd[0]) is None:
        return "not installed"
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.SubprocessError):
        return "not installed"
    text = (done.stdout or done.stderr or "").strip().splitlines()
    return " ".join(text[0].split()) if text else "unknown"


def pmd_version() -> str:
    """PMD ships as an unpacked archive, so its version is the directory name."""
    found = sorted((ROOT / "tools" / "third-party").glob("pmd-bin-*"))
    return found[-1].name.replace("pmd-bin-", "") if found else "not installed"


def main() -> int:
    out_dir = ROOT / (sys.argv[1] if len(sys.argv) > 1 else "analysis-results")
    out_dir.mkdir(parents=True, exist_ok=True)

    tools = [{"tool": label, "version": first_line(cmd), "invocation": how,
              "artefact": artefact}
             for label, cmd, how, artefact in TOOLS]
    tools.append({"tool": "PMD CPD", "version": pmd_version(),
                  "invocation": "tools/cpd_scan.py, minimum 100 tokens — the clone GATE",
                  "artefact": "analysis-results/pmd-cpd.txt"})
    tools.append({"tool": "objectName check", "version": "tools/check_object_names.py",
                  "invocation": "every member widget in src/ui needs a stable objectName "
                                "(REQ-N-007)",
                  "artefact": "analysis-results/object-names.txt"})

    payload = {"generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
               "tools": tools}
    target = out_dir / "tool-versions.json"
    target.write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")
    installed = sum(1 for t in tools if t["version"] != "not installed")
    print(f"tool identity: {installed} of {len(tools)} tools recorded -> "
          f"{target.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
