#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Every widget a GUI test has to find must have a STABLE objectName (REQ-N-007).

Squish (and any other GUI driver) identifies widgets either by a fragile
positional/text description or by objectName. Text changes with wording and
translation; position changes with layout. An objectName is the one handle that a
refactor does not silently break — but only if new widgets keep getting one, which
is what this check enforces:

    tools/check_object_names.py            # every src/ui/*.cpp
    tools/check_object_names.py --fix-hint # …and print the line to add

A member widget assigned with `m_foo = new QSomething(...)` must be followed by
`m_foo->setObjectName(...)`. Non-member locals are ignored: a widget nobody keeps
a pointer to is one no test can meaningfully address either.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "src" / "ui"

# `m_foo = new QPushButton(...)` — the member widgets a test may need to address.
ASSIGN = re.compile(r"^(\s*)m_([A-Za-z]\w*)\s*=\s*new\s+(Q\w+)\s*\(")
# Widget-ish types only: a QTimer or a QNetworkAccessManager is not addressable.
NON_WIDGET = re.compile(
    r"^Q("
    r"Timer|NetworkAccessManager|Process|Settings|Thread|FutureWatcher|StandardItemModel|"
    r"SortFilterProxyModel|GraphicsScene|Shortcut|Action|ButtonGroup|ValueAxis|DateTimeAxis|"
    r"Chart|LineSeries|ScatterSeries|SplineSeries|BarSeries|BarSet|StringList|Json"
    r")"
)


def missing_in(path: Path) -> list[tuple[int, str, str]]:
    """(line number, member, type) for each member widget without an objectName."""
    lines = path.read_text(encoding="utf-8").split("\n")
    text = "\n".join(lines)
    gaps: list[tuple[int, str, str]] = []
    for number, line in enumerate(lines, start=1):
        match = ASSIGN.match(line)
        if not match:
            continue
        member, cls = match.group(2), match.group(3)
        if NON_WIDGET.match(cls):
            continue
        # The call may sit a few lines later (a multi-line constructor); searching the
        # whole file is enough — a name set anywhere is a name a test can use.
        if f'm_{member}->setObjectName(' not in text:
            gaps.append((number, member, cls))
    return gaps


def main() -> int:
    hint = "--fix-hint" in sys.argv
    total = 0
    for path in sorted(UI.glob("*.cpp")):
        gaps = missing_in(path)
        if not gaps:
            continue
        total += len(gaps)
        for number, member, cls in gaps:
            rel = path.relative_to(ROOT)
            print(f"{rel}:{number}: m_{member} ({cls}) has no objectName")
            if hint:
                print(f'    m_{member}->setObjectName(QStringLiteral("{member}"));')
    if total:
        print(f"\n{total} widget(s) without a stable objectName — a GUI test cannot address")
        print("them except by text or position, both of which a refactor breaks silently.")
        print("Run with --fix-hint to get the exact line to add.")
        return 1
    print("every member widget in src/ui has a stable objectName")
    return 0


if __name__ == "__main__":
    sys.exit(main())
