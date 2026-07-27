#!/usr/bin/env python3
"""MSVC /analyze over every project TU — the Windows counterpart of the
`g++ -fanalyzer` stage in tools/static_analysis.sh.

Visual C++ ships its own symbolic-execution analyzer (the C6xxx code-analysis
warnings). Flags are taken from the compile database, so include paths and
defines match the real build; /analyze:only skips code generation, and one
process per core keeps the wall time close to a normal build.

Output is normalized to the GCC style the Axivion import already understands
(file:line:col: warning: message [C6011]) and written to
analysis-results/msvc-analyze.txt as provider "msvc-analyze".

Only diagnostics located inside <root>/src survive: /analyze walks into the Qt
and CRT headers and reports there in volume, which is neither actionable nor
ours — the same project-scope filter the gcc-analyzer stage applies.

Usage: msvc_analyze.py <compile_commands.json> <project-root> <out.txt>
"""

import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys

# cl.exe diagnostics: "C:\path\file.cpp(120,9): warning C6011: message"
DIAG = re.compile(
    r"^(?P<file>[A-Za-z]:\\[^(]+)\((?P<line>\d+)(?:,(?P<col>\d+))?\)\s*:\s*"
    r"(?P<severity>warning|error)\s+(?P<code>C\d+)\s*:\s*(?P<msg>.*)$"
)

# Flags that make no sense (or actively break) an /analyze-only pass.
DROP_EXACT = {"/c", "-c", "/showIncludes", "-showIncludes"}
DROP_PREFIX = ("/Fo", "-Fo", "/Fd", "-Fd", "/FS", "-FS", "/MP", "-MP")


def split_command(entry):
    """compile_commands entries carry either 'arguments' or a 'command' string."""
    if "arguments" in entry:
        return list(entry["arguments"])
    # cl.exe command lines quote paths containing spaces; a naive split breaks
    # on "C:\Program Files\...". shlex with posix=False keeps the quotes, which
    # subprocess would then pass through literally — strip them explicitly.
    import shlex

    lex = shlex.shlex(entry["command"], posix=False)
    lex.whitespace_split = True
    return [a.strip('"') for a in lex]


def run(entry, root):
    args = [a for a in split_command(entry)
            if a not in DROP_EXACT and not a.startswith(DROP_PREFIX)]
    args += [
        "/analyze",
        "/analyze:only",     # no code generation, diagnostics only
        "/analyze:quiet",    # no per-file summary banner
        "/analyze:WX-",      # never promote analysis warnings to errors
        "/nologo",
        "/c",
    ]
    # /analyze insists on an object path even in :only mode on some toolsets.
    args += ["/Fo" + os.devnull]
    try:
        r = subprocess.run(args, cwd=entry["directory"], capture_output=True,
                           text=True, errors="replace", timeout=900)
    except subprocess.TimeoutExpired:
        return [f'{entry["file"]}:1:1: warning: analyzer timed out [msvc-analyze-timeout]']
    except OSError as exc:
        return [f'{entry["file"]}:1:1: warning: analyzer failed to start: {exc} [msvc-analyze-error]']

    src_dir = os.path.join(root, "src") + os.sep
    keep = []
    for line in (r.stdout + r.stderr).splitlines():
        m = DIAG.match(line.strip())
        if not m:
            continue
        path = os.path.normpath(m["file"])
        if not path.lower().startswith(src_dir.lower()):
            continue
        col = m["col"] or "1"
        keep.append(f'{path}:{m["line"]}:{col}: {m["severity"]}: {m["msg"].strip()} [{m["code"]}]')
    return keep


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    db_path, root, out_path = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]

    with open(db_path, encoding="utf-8") as f:
        db = json.load(f)
    src_dir = os.path.join(root, "src") + os.sep
    entries = [e for e in db if os.path.normpath(e["file"]).lower().startswith(src_dir.lower())]

    lines = set()
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for result in ex.map(lambda e: run(e, root), entries):
            lines.update(result)

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(lines)) + ("\n" if lines else ""))
    print(f"msvc-analyze: {len(lines)} findings over {len(entries)} TUs")


if __name__ == "__main__":
    main()
