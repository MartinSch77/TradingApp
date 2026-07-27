#!/usr/bin/env python3
"""Regenerate docs/requirements.md from requirements/requirements.sdoc.

The .sdoc file (StrictDoc) is the single source of truth for requirements;
the markdown page is a GENERATED view kept for the Doxygen documentation and
as one input leg of tools/trace_report.py. Run via tools/make_requirements.sh.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDOC = ROOT / "requirements" / "requirements.sdoc"
OUT = ROOT / "docs" / "requirements.md"

text = SDOC.read_text(encoding="utf-8")

sections: list[tuple[str, list[dict]]] = []
current: list[dict] | None = None
req: dict | None = None
for line in text.splitlines():
    if line.startswith("[[SECTION]]"):
        current = []
        req = None
    elif m := re.match(r"^TITLE: (.+)$", line):
        if req is not None:
            req["title"] = m.group(1)
        elif current is not None and not current:
            sections.append((m.group(1), current))
    elif line.startswith("[REQUIREMENT]"):
        req = {}
        if current is not None:
            current.append(req)
    elif req is not None and (m := re.match(r"^(UID|VERIFICATION|STATEMENT): (.+)$", line)):
        value = m.group(2)
        # single-line sdoc strings may be quoted to protect ':' — unquote
        if value.startswith("'") and value.endswith("'"):
            value = value[1:-1]
        req[m.group(1).lower()] = value

if not sections or any(not reqs for _, reqs in sections):
    sys.exit(f"error: no sections/requirements parsed from {SDOC}")

lines = [
    "# TradingApp — Software Requirement Specification (SRS)",
    "",
    "@page requirements Software Requirements",
    "@tableofcontents",
    "",
    "<!-- GENERATED FILE — do not edit. Source of truth:",
    "     requirements/requirements.sdoc (StrictDoc). Regenerate with",
    "     tools/make_requirements.sh -->",
    "",
    "This page is **generated** from `requirements/requirements.sdoc`",
    "(requirements-as-code, StrictDoc) — edit there and run",
    "`tools/make_requirements.sh`. Requirement IDs are stable and referenced",
    "from the design (`docs/design.md`), the test specification",
    "(`docs/test_spec.md`), the test implementations (`tests/tst_*.cpp`, marker",
    "`@relation(REQ-…, scope=function)`) and the generated traceability matrix",
    "(`docs/traceability.html`). Format: `REQ-F-xxx` functional, `REQ-N-xxx`",
    "non-functional. Verification method: T = test, A = analysis,",
    "I = inspection.",
    "",
]
for title, reqs in sections:
    lines += [f"## {title}", "", "| ID | Requirement | Verify |", "|----|-------------|--------|"]
    for r in reqs:
        lines.append(f"| {r['uid']} | {r['statement']} | {r['verification']} |")
    lines.append("")

# newline="\n" keeps the generated page byte-identical between Linux and
# Windows; without it Python writes CRLF here and every regeneration on the
# other platform shows up as a whole-file diff.
OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
print(f"generated {OUT.relative_to(ROOT)} from {SDOC.relative_to(ROOT)} "
      f"({sum(len(r) for _, r in sections)} requirements)")
