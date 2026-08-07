#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Clang Static Analyzer over every project TU (provider `clang-analyzer`).

Shared by tools/static_analysis.sh (Linux) and tools/static_analysis.ps1
(Windows) so the two platforms cannot drift apart on the checker set.

Why a stage of its own, next to clang-tidy's clang-analyzer-* checks: clang-tidy
runs the analyzer with its DEFAULT checker set and DEFAULT exploration budget
and offers no way to pass -analyzer-config. This stage is the strict run —

  * the off-by-default checkers that apply to a Qt/C++ codebase
    (optin.cplusplus.*, security.*, nullability.Nullable*, valist.*), and
  * a deeper search: 400k exploration nodes instead of 225k, loop widening and
    unrolling, and the aggressive-binary-operation-simplification model.

Overlapping findings therefore arrive from two providers (clang-tidy and
clang-analyzer). That is deliberate; this pass is the authoritative one.

NOT enabled, and why (measured 2026-07-29 over all 23 src TUs):
  alpha.*                     112 findings, false-positive dominated: 99 are
                              alpha.cplusplus.IteratorRange on plain range-for
                              loops over QList (Qt's iterators are not modelled),
                              12 alpha.deadcode.UnreachableCode on the nested
                              conditional-operator idiom, 1
                              alpha.cplusplus.MismatchedIterator on two
                              constBegin()/constEnd() calls through the same
                              pointer. Upstream marks them experimental for
                              exactly this reason.
  crosscheck-with-z3=true     would refute infeasible paths (fewer false
                              positives), but Ubuntu's LLVM 18 is built without
                              Z3 (`LLVM was not compiled with Z3 support`), and
                              enabling it is a hard backend error rather than a
                              warning — so it is probed, not assumed.

Output: analysis-results/clang-analyzer.txt in the GCC-style line format the
dashboard import reads:  file:line:col: warning: message [checker.name]
A TU the analyzer could not finish (crash, OOM, timeout) is reported as a
finding of its own instead of silently contributing nothing.

Usage: clang_analyzer.py <compile_commands.json> <project-root> <output-file>
"""

import concurrent.futures as cf
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

# Off-by-default checkers worth having. (The default set — core.*, cplusplus.*,
# deadcode.*, unix.*, nullability.NullPassedToNonnull … — is always active.)
EXTRA_CHECKERS = (
    "optin.cplusplus.UninitializedObject",   # member left uninitialized by a ctor
    "optin.cplusplus.VirtualCall",           # virtual call during construction
    "optin.core.EnumCastOutOfRange",         # cast of an out-of-range value to an enum
    "optin.portability.UnixAPI",             # non-portable API assumptions
    "security.FloatLoopCounter",             # float as a loop counter
    "security.cert.env.InvalidPtr",          # getenv/setenv pointer invalidation
    "nullability.NullableDereferenced",      # deref of a _Nullable pointer
    "nullability.NullablePassedToNonnull",
    "nullability.NullableReturnedFromNonnull",
    "valist.Uninitialized",
    "valist.CopyToSelf",
)

# Deeper exploration than the analyzer's defaults.
ANALYZER_CONFIG = (
    "max-nodes=400000",                                # default 225000
    "widen-loops=true",                                # keep analysing past loops
    "unroll-loops=true",
    "aggressive-binary-operation-simplification=true",
    "ipa=dynamic-bifurcate",
)

# Every project TU is analysed, sources and tests alike.
SOURCE_DIRS = ("src", "tests")

LOCATED = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): "
                     r"warning: (?P<msg>.*) \[(?P<checker>[A-Za-z0-9_.]+)\]$")


def _find_compiler(db_compiler: str) -> str | None:
    """A clang driver that understands the compile database's flag dialect.

    An MSVC-style database (cl.exe) needs clang-cl, everything else clang++.
    CLANG_ANALYZER_CXX overrides the choice.
    """
    override = os.environ.get("CLANG_ANALYZER_CXX")
    if override:
        return shutil.which(override) or override
    msvc_style = os.path.basename(db_compiler).lower().startswith(("cl.", "cl-", "cl_")) \
        or os.path.basename(db_compiler).lower() == "cl"
    candidates = ("clang-cl",) if msvc_style else ("clang++-18", "clang++", "clang-cl")
    for candidate in candidates:
        found = shutil.which(candidate)
        if found:
            return found
    return None


def _supports_z3(compiler: str) -> bool:
    """Probe whether this clang was built with the Z3 constraint solver.

    The probe program has to actually PRODUCE a report: Z3 is only called to
    refute a candidate bug path, so a clean file compiles happily even on a
    build without Z3 and would answer "supported". A clang without Z3 aborts
    with `LLVM was not compiled with Z3 support` — a fatal backend error, not a
    warning, which is why this is probed instead of assumed.
    """
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.cpp")
        with open(probe, "w", encoding="utf-8") as handle:
            handle.write("int probe(int *p) { if (p == nullptr) { return *p; } return 0; }\n")
        run = subprocess.run([compiler, "--analyze",
                              "-Xclang", "-analyzer-config", "-Xclang", "crosscheck-with-z3=true",
                              "-o", os.devnull, probe],
                             capture_output=True, text=True)
        return run.returncode == 0 and "Z3" not in run.stderr


def _analyzer_flags(with_z3: bool) -> list[str]:
    flags = ["--analyze", "-Xclang", "-analyzer-output=text"]
    for checker in EXTRA_CHECKERS:
        flags += ["-Xclang", f"-analyzer-checker={checker}"]
    config = list(ANALYZER_CONFIG) + (["crosscheck-with-z3=true"] if with_z3 else [])
    for option in config:
        flags += ["-Xclang", "-analyzer-config", "-Xclang", option]
    return flags


def _tu_arguments(entry: dict, compiler: str, flags: list[str]) -> list[str]:
    """The database's own command, retargeted at the analyzer.

    Same flags as the real build (so Qt include paths and defines match), with
    the compiler swapped for clang, -o/-c dropped and the analyzer flags added.

    Warnings-as-errors is dropped: it is a build policy
    (TRADINGAPP_WARNINGS_AS_ERRORS), and leaving it in makes the analyzer exit
    nonzero on any warning, which this stage would then have to report as "the
    TU could not be analysed".
    """
    command = entry.get("arguments") or shlex.split(entry["command"])
    args, skip = [], False
    for arg in command[1:]:
        if skip:
            skip = False
            continue
        if arg in ("-o", "/Fo"):
            skip = True
            continue
        if arg in ("-c", "/c"):
            continue
        if arg == "-Werror" or arg.startswith("-Werror=") or arg == "/WX":
            continue
        args.append(arg)
    return [compiler, *flags, *args, "-o", os.devnull]


def _run(entry: dict, compiler: str, flags: list[str], root: str) -> list[str]:
    args = _tu_arguments(entry, compiler, flags)
    try:
        run = subprocess.run(args, cwd=entry["directory"], capture_output=True,
                             text=True, timeout=1800)
    except subprocess.TimeoutExpired:
        return [f'{entry["file"]}:1:1: error: clang --analyze timed out '
                f'[clang-analyzer-timeout]']
    if run.returncode != 0:
        # A nonzero exit means the TU was never fully analysed (crash, OOM,
        # bad flags). Reporting nothing here would green-wash the run.
        last = run.stderr.strip().splitlines()[-1] if run.stderr.strip() else "no diagnostics"
        return [f'{entry["file"]}:1:1: error: clang --analyze exited with '
                f'{run.returncode}: {last} [clang-analyzer-failed]']
    keep = []
    for line in run.stderr.splitlines():
        match = LOCATED.match(line)
        if match and os.path.abspath(match.group("file")).startswith(root + os.sep):
            keep.append(line)
    return keep


def main() -> int:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    db_path, root, out_path = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]

    with open(db_path, encoding="utf-8") as handle:
        database = json.load(handle)
    prefixes = tuple(os.path.join(root, d) + os.sep for d in SOURCE_DIRS)
    entries = [e for e in database if os.path.abspath(e["file"]).startswith(prefixes)]
    if not entries:
        print("clang-analyzer: no project TUs in the compile database — "
              "build the compile-DB build dir first", file=sys.stderr)
        return 1

    first = entries[0].get("arguments") or shlex.split(entries[0]["command"])
    compiler = _find_compiler(first[0])
    if not compiler:
        print("clang-analyzer: no clang++/clang-cl found — stage skipped")
        with open(out_path, "w", encoding="utf-8") as handle:
            handle.write("")
        return 3  # the pipeline's "stage skipped" code

    with_z3 = _supports_z3(compiler)
    flags = _analyzer_flags(with_z3)

    lines: set[str] = set()
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        futures = [pool.submit(_run, e, compiler, flags, root) for e in entries]
        for future in cf.as_completed(futures):
            lines.update(future.result())

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(sorted(lines)) + ("\n" if lines else ""))
    print(f"clang-analyzer ({os.path.basename(compiler)}, "
          f"{len(EXTRA_CHECKERS)} extra checkers, "
          f"z3 {'on' if with_z3 else 'unavailable'}): "
          f"{len(lines)} findings over {len(entries)} TUs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
