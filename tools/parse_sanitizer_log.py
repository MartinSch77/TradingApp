#!/usr/bin/env python3
"""Normalize sanitizer / valgrind raw logs into the pipe format the Axivion
external import understands (file|line|severity|id|message — the same shape
as the cppcheck log), so runtime findings appear on the dashboard next to the
static ones (see axivion/external_import.py).

Usage: parse_sanitizer_log.py {asan-ubsan|tsan|valgrind} <raw-log> <out> <project-root>

Only findings that can be pinned to a project source line are emitted:
 * UBSan "file:line: runtime error: …" lines
 * ASan/TSan "SUMMARY: …Sanitizer: <kind> file:line …" lines
 * LSan leak blocks — located at their first stack frame inside the project
 * valgrind error/leak blocks — likewise via the first project frame;
   "still reachable" blocks are skipped (Qt/OpenSSL runtime allocations, kept
   in the raw log for inspection but not actionable findings)
"""

import re
import sys
from pathlib import Path

KIND, RAW, OUT, ROOT = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])

# valgrind prints frame locations as basenames ("Config.cpp:47"); map every
# project source basename to its repo-relative path once.
BASENAMES: dict[str, str] = {}
for sub in ("src", "tests"):
    for p in (ROOT / sub).rglob("*"):
        if p.suffix in (".cpp", ".h"):
            BASENAMES.setdefault(p.name, str(p.relative_to(ROOT)))

UBSAN = re.compile(r"^(?P<file>/\S+?):(?P<line>\d+)(?::\d+)?: runtime error: (?P<msg>.*)$")
SUMMARY = re.compile(
    r"SUMMARY: (?P<san>\w+)Sanitizer: (?P<kind>.+?) (?P<file>/\S+?):(?P<line>\d+)"
)
LEAK_HEAD = re.compile(r"^(?P<kind>Direct|Indirect) leak of (?P<bytes>\d+) byte")
FRAME = re.compile(r"#\d+ 0x\w+ in \S+ (?P<file>/\S+?):(?P<line>\d+)")
VG_PREFIX = re.compile(r"^==\d+==\s?")
VG_HEADS = [
    ("error", "invalid-read", re.compile(r"^Invalid read of size")),
    ("error", "invalid-write", re.compile(r"^Invalid write of size")),
    ("error", "uninitialised", re.compile(r"^(Conditional jump or move|Use of uninitialised value)")),
    ("error", "invalid-free", re.compile(r"^(Invalid free|Mismatched free)")),
    ("warning", "definitely-lost", re.compile(r"are definitely lost")),
    ("warning", "indirectly-lost", re.compile(r"are indirectly lost")),
    ("warning", "possibly-lost", re.compile(r"are possibly lost")),
]
VG_FRAME = re.compile(r"^\s*(?:at|by) 0x\w+: .* \((?P<file>[^():]+):(?P<line>\d+)\)")


def project_path(filename: str) -> str | None:
    p = Path(filename)
    if p.is_absolute():
        try:
            rel = p.resolve().relative_to(ROOT)
        except ValueError:
            return None
        return str(rel)
    return BASENAMES.get(p.name)


def parse_sanitizer(lines):
    """asan-ubsan and tsan share the UBSan/SUMMARY/leak-block grammar."""
    pending = None  # open leak block looking for its first project frame
    for line in lines:
        m = UBSAN.search(line)
        if m and (f := project_path(m["file"])):
            yield f, m["line"], "error", "ubsan", m["msg"].strip()
            continue
        m = SUMMARY.search(line)
        if m and (f := project_path(m["file"])):
            ident = f"{m['san'].lower()}san-{m['kind'].strip().replace(' ', '-')}"
            yield f, m["line"], "error", ident, f"{m['san']}Sanitizer: {m['kind'].strip()}"
            continue
        m = LEAK_HEAD.search(line)
        if m:
            pending = (m["kind"].lower(), m["bytes"])
            continue
        if pending:
            m = FRAME.search(line)
            if m and (f := project_path(m["file"])):
                kind, nbytes = pending
                pending = None
                yield f, m["line"], "warning", f"lsan-{kind}-leak", f"{kind} leak of {nbytes} bytes"


def parse_valgrind(lines):
    pending = None  # (severity, id, message) awaiting a project frame
    for raw in lines:
        line = VG_PREFIX.sub("", raw.rstrip())
        if not line.strip():
            pending = None
            continue
        for severity, ident, rx in VG_HEADS:
            if rx.search(line):
                pending = (severity, ident, line.strip())
                break
        else:
            if pending:
                m = VG_FRAME.match(line)
                if m and (f := project_path(m["file"])):
                    severity, ident, msg = pending
                    pending = None
                    yield f, m["line"], severity, f"valgrind-{ident}", msg


lines = RAW.read_text(errors="replace").splitlines() if RAW.exists() else []
parser = parse_valgrind if KIND == "valgrind" else parse_sanitizer
rows = sorted(set(parser(lines)))
OUT.write_text("".join(f"{f}|{ln}|{sev}|{ident}|{msg}\n" for f, ln, sev, ident, msg in rows))
print(f"{KIND}: {len(rows)} findings -> {OUT}")
