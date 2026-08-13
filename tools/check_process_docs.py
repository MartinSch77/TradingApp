#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Process-doc traceability check (process/process-model.md §5, "testable").

Mirrors tools/trace_report.py's discipline but one level up: instead of
REQ<->DES<->TS, this checks process/processes/*.md <-> process/work-products/*.md
<-> process/strategies/*.md <-> process/templates/*.md all resolve to real
files, that every process file has the minimum required sections, and that
every work product is referenced by at least one process. A broken
cross-reference here is exactly as real a defect as an orphaned REQ id —
the process framework is a work product too (process-model.md §5).

Exit 0 = no findings, prints a summary. Exit 1 = findings printed, one per
line, nothing hidden.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROCESS_DIR = ROOT / "process"
PROCESSES_DIR = PROCESS_DIR / "processes"
WORK_PRODUCTS_DIR = PROCESS_DIR / "work-products"
STRATEGIES_DIR = PROCESS_DIR / "strategies"
TEMPLATES_DIR = PROCESS_DIR / "templates"

# The minimum sections every process file must carry, regardless of whether
# it maps to a numbered ASPICE base process (DEVOPS-principles.md and
# MLE.1-4-machine-learning-engineering.md are deliberately structured
# differently — process-model.md documents why — so only the universally
# meaningful three are required here, not the full seven-section template).
REQUIRED_PROCESS_SECTIONS = ["## Purpose", "## Roles", "## Verification / QA Hooks"]

# A reference to another process-framework file, written as a backtick'd
# relative-ish path: `work-products/foo.md`, `strategies/bar.md`,
# `templates/baz.md`, `processes/SWE.1-....md`.
REF_RE = re.compile(
    r"`(work-products|strategies|templates|processes)/([A-Za-z0-9_.\-]+\.md)`"
)


def find_refs(text: str) -> set[tuple[str, str]]:
    return set(REF_RE.findall(text))


def check_sections(path: Path, findings: list[str]) -> None:
    text = path.read_text(encoding="utf-8")
    for section in REQUIRED_PROCESS_SECTIONS:
        if section not in text:
            findings.append(f"{path.relative_to(ROOT)}: missing required section {section!r}")


def check_refs_resolve(path: Path, findings: list[str]) -> set[tuple[str, str]]:
    text = path.read_text(encoding="utf-8")
    refs = find_refs(text)
    for kind, name in refs:
        target = PROCESS_DIR / kind / name
        if not target.is_file():
            findings.append(f"{path.relative_to(ROOT)}: references {kind}/{name}, which does not exist")
    return refs


def main() -> int:
    if not PROCESSES_DIR.is_dir():
        print("process/processes/ not found — nothing to check", file=sys.stderr)
        return 1

    findings: list[str] = []
    all_refs: set[tuple[str, str]] = set()

    process_files = sorted(PROCESSES_DIR.glob("*.md"))
    for path in process_files:
        check_sections(path, findings)
        all_refs |= check_refs_resolve(path, findings)

    # Strategies/templates can reference work products/each other too; check
    # their own references resolve, and fold them into the reference set so
    # a work product cited only from a strategy still counts as "used."
    for extra_dir in (STRATEGIES_DIR, TEMPLATES_DIR, WORK_PRODUCTS_DIR):
        for path in sorted(extra_dir.glob("*.md")):
            all_refs |= check_refs_resolve(path, findings)

    referenced_work_products = {name for kind, name in all_refs if kind == "work-products"}
    for path in sorted(WORK_PRODUCTS_DIR.glob("*.md")):
        if path.name not in referenced_work_products:
            findings.append(
                f"{path.relative_to(ROOT)}: not referenced from any processes/strategies/templates file"
            )

    referenced_strategies = {name for kind, name in all_refs if kind == "strategies"}
    for path in sorted(STRATEGIES_DIR.glob("*.md")):
        if path.name not in referenced_strategies:
            findings.append(f"{path.relative_to(ROOT)}: not referenced from any process file")

    referenced_templates = {name for kind, name in all_refs if kind == "templates"}
    for path in sorted(TEMPLATES_DIR.glob("*.md")):
        if path.name == "ai-reviewer-instructions.md":
            continue  # referenced by convention (every checklist says "read this first"), not by backtick path
        if path.name not in referenced_templates:
            findings.append(f"{path.relative_to(ROOT)}: not referenced from any process file")

    if findings:
        print(f"check_process_docs: {len(findings)} finding(s)", file=sys.stderr)
        for f in findings:
            print(f"  {f}", file=sys.stderr)
        return 1

    print(
        f"check_process_docs: OK — {len(process_files)} processes, "
        f"{len(list(WORK_PRODUCTS_DIR.glob('*.md')))} work products, "
        f"{len(list(STRATEGIES_DIR.glob('*.md')))} strategies, "
        f"{len(list(TEMPLATES_DIR.glob('*.md')))} templates, all cross-referenced"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
