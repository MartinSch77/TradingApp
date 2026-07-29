#!/usr/bin/env python3
"""Copy-paste detection over the C++ sources with PMD CPD (provider `pmd-cpd`).

Shared by tools/static_analysis.sh (Linux) and tools/static_analysis.ps1
(Windows) so the two platforms cannot drift apart on the token threshold.

Why PMD CPD and not something already in the pipeline: this project's Axivion
configuration runs MISRA C++ 2023 only — clone detection (issue type CL) is not
part of it (see CLAUDE.md), so duplicated code had no gate at all. CPD is a
token-based detector with a real C++ tokenizer, so it finds copies through
renamed variables and reformatting, which grep cannot.

Threshold: 100 tokens. That is CPD's documented starting point for C-family
languages and, measured on this codebase, the point where every hit is a
genuine block of copied logic rather than a run of similar Qt setup calls
(60 tokens: 17 hits, mostly widget boilerplate; 100 tokens: 5 real ones).

Output: analysis-results/pmd-cpd.txt in the pipe format the dashboard import
reads — file|line|severity|id|message — one line per occurrence of a clone, so
both halves of a copy are reported where they are.

PMD is a Java tool: setup.sh / setup.ps1 install it under
tools/third-party/pmd-bin-<version>/ (like tools/third-party/plantuml.jar).
PMD_HOME overrides the location.

Usage: cpd_scan.py <project-root> <output-file> [--min-tokens N]
"""

import csv
import io
import os
import shutil
import subprocess
import sys
from pathlib import Path

MIN_TOKENS = 100
SCAN_DIRS = ("src", "tests")


def _pmd_executable(root: Path) -> Path | None:
    """The PMD launcher: $PMD_HOME, the bundled dist, or one on PATH."""
    name = "pmd.bat" if os.name == "nt" else "pmd"
    home = os.environ.get("PMD_HOME")
    if home:
        candidate = Path(home) / "bin" / name
        if candidate.is_file():
            return candidate
    for candidate in sorted((root / "tools" / "third-party").glob("pmd-bin-*/bin/" + name),
                            reverse=True):
        if candidate.is_file():
            return candidate
    found = shutil.which("pmd")
    return Path(found) if found else None


def _run_cpd(pmd: Path, root: Path, min_tokens: int) -> str:
    directories = []
    for name in SCAN_DIRS:
        if (root / name).is_dir():
            directories += ["--dir", str(root / name)]
    command = [str(pmd), "cpd", "--language", "cpp",
               "--minimum-tokens", str(min_tokens),
               "--format", "csv",
               # Report duplicates, do not fail the process on them: this script
               # owns the exit code, and CPD's own nonzero exit would be
               # indistinguishable from a crash.
               "--no-fail-on-violation",
               *directories]
    run = subprocess.run(command, capture_output=True, text=True, cwd=root)
    if run.returncode != 0:
        raise SystemExit(f"pmd cpd failed (rc={run.returncode}): "
                         f"{run.stderr.strip()[:500]}")
    return run.stdout


def _rows(csv_text: str, root: Path) -> tuple[list[str], int]:
    """CPD's CSV -> one pipe-format finding per occurrence, plus the clone count.

    CSV shape: lines,tokens,occurrences,then (line,file) repeated per occurrence.
    """
    findings, clones = [], 0
    reader = csv.reader(io.StringIO(csv_text))
    for record in reader:
        if len(record) < 5 or not record[0].isdigit():
            continue  # header or blank
        lines, tokens, count = int(record[0]), int(record[1]), int(record[2])
        places = []
        for index in range(count):
            line_field, file_field = record[3 + (index * 2)], record[4 + (index * 2)]
            try:
                relative = Path(file_field).resolve().relative_to(root.resolve()).as_posix()
            except ValueError:
                relative = Path(file_field).as_posix()
            places.append((relative, int(line_field)))
        clones += 1
        for position, (relative, line) in enumerate(places):
            others = ", ".join(f"{f}:{l}" for i, (f, l) in enumerate(places) if i != position)
            findings.append(f"{relative}|{line}|warning|pmd-cpd-duplicate|"
                            f"{lines} lines / {tokens} tokens duplicated with {others}")
    return findings, clones


def main() -> int:
    arguments = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(arguments) != 2:
        sys.exit(__doc__)
    min_tokens = MIN_TOKENS
    if "--min-tokens" in sys.argv:
        min_tokens = int(sys.argv[sys.argv.index("--min-tokens") + 1])
    root, out_path = Path(arguments[0]).resolve(), Path(arguments[1])

    pmd = _pmd_executable(root)
    if pmd is None:
        print("PMD not installed (./setup.sh installs it under "
              "tools/third-party/) — copy-paste detection skipped")
        out_path.write_text("", encoding="utf-8")
        return 3  # the pipeline's "stage skipped" code
    if shutil.which("java") is None:
        print("no Java runtime — PMD CPD skipped (apt install default-jre-headless)")
        out_path.write_text("", encoding="utf-8")
        return 3

    findings, clones = _rows(_run_cpd(pmd, root, min_tokens), root)
    out_path.write_text("\n".join(sorted(findings)) + ("\n" if findings else ""),
                        encoding="utf-8")
    print(f"pmd-cpd (>= {min_tokens} tokens): {len(findings)} findings "
          f"({clones} duplicated blocks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
