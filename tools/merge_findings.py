#!/usr/bin/env python3
"""Merge the per-tool analyzer logs into one CSV overview.

Shared by tools/static_analysis.sh (Linux) and tools/static_analysis.ps1
(Windows) so the two platforms cannot drift apart on the merge format.

Input  analysis-results/<tool>.txt, in the two shapes the tools produce:
         pipe       file|line|severity|id|message        (cppcheck, codespell,
                                                          lizard, pmd-cpd)
         gcc-style  file:line:col: warning: msg [id]      (clang-tidy, clazy,
                                                           gcc-analyzer,
                                                           clang-analyzer,
                                                           msvc-analyze)
Output analysis-results/external_findings.csv
         tool;file;line;rule;severity;message

Logs that do not exist are skipped: gcc-analyzer only runs on Linux,
msvc-analyze only on Windows, clazy only where it is installed, lizard and
pmd-cpd only where setup.sh/setup.ps1 provisioned them.

Usage: merge_findings.py <analysis-results-dir>
"""

import csv
import re
import sys
from pathlib import Path

PIPE_TOOLS = ("cppcheck", "codespell", "lizard", "pmd-cpd")
GCC_STYLE_TOOLS = ("clang-tidy", "clazy", "gcc-analyzer", "clang-analyzer", "msvc-analyze")

GCC_RE = re.compile(r"^(.*?):(\d+):\d+:\s+(warning|error):\s+(.*?)\s+\[(.*)\]$")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    rows = []

    for tool in PIPE_TOOLS:
        path = out / f"{tool}.txt"
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split("|", 4)
            if len(parts) == 5:
                # file|line|severity|id|message -> tool;file;line;rule;severity;message
                rows.append([tool, parts[0], parts[1], parts[3], parts[2], parts[4]])

    for tool in GCC_STYLE_TOOLS:
        path = out / f"{tool}.txt"
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = GCC_RE.match(line)
            if m:
                rows.append([tool, m.group(1), m.group(2), m.group(5), m.group(3), m.group(4)])

    with open(out / "external_findings.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, delimiter=";")
        w.writerow(["tool", "file", "line", "rule", "severity", "message"])
        w.writerows(rows)
    print(f"merged: {len(rows)} findings -> analysis-results/external_findings.csv")


if __name__ == "__main__":
    main()
