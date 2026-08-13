#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Quality Assurance report (SUP.1, process/processes/SUP.1-quality-assurance.md).

Answers "was the process followed" — NEVER "is the product correct" (that
question belongs to tools/trace_report.py, tools/static_analysis.sh, and the
rest of the engineering verification toolchain; see process/process-model.md
Section 2 for why the two are kept deliberately separate).

Every verdict below cites the exact file/command it checked — never a bare
CONFIRMED/NOT FOUND with no evidence, per process/work-products/qa-report.md's
own content rule. Informational: exits 0 in every case except the ONE stated
exception process/process-model.md carries — missing/stale test-report
evidence — which exits 1, because a release with no proof anything was
tested is not a case QA may stay silent about.

Usage: python3 tools/qa_report.py [--out PATH]
"""
from __future__ import annotations

import argparse
import glob
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


@dataclass
class Verdict:
    process: str
    status: str  # CONFIRMED | NOT FOUND | PARTIAL
    evidence: list[str] = field(default_factory=list)


def run(cmd: list[str]) -> tuple[int, str]:
    try:
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=120)
        return proc.returncode, (proc.stdout + proc.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 1, f"could not run {' '.join(cmd)}: {exc}"


def check_sup1() -> Verdict:
    rc, out = run([sys.executable, str(ROOT / "tools" / "check_process_docs.py")])
    if rc == 0:
        return Verdict("SUP.1 Quality Assurance", "CONFIRMED", [out])
    return Verdict("SUP.1 Quality Assurance", "PARTIAL", [out])


def check_swe1() -> Verdict:
    sdoc = ROOT / "requirements" / "requirements.sdoc"
    if not sdoc.is_file():
        return Verdict("SWE.1 Requirements Elicitation", "NOT FOUND", ["requirements/requirements.sdoc absent"])
    text = sdoc.read_text(encoding="utf-8")
    # Split into per-requirement blocks so a global substring count (which the
    # document's own [TEXT] preamble, mentioning "VERIFICATION:" in prose,
    # would silently corrupt) cannot produce a wrong total.
    blocks = text.split("[REQUIREMENT]")[1:]
    total = len(blocks)
    missing_verification = sum(1 for b in blocks if "\nVERIFICATION:" not in b)
    evidence = [f"requirements/requirements.sdoc: {total} requirements, "
                f"{missing_verification} missing a VERIFICATION field"]
    status = "CONFIRMED" if missing_verification == 0 else "PARTIAL"
    return Verdict("SWE.1 Requirements Elicitation", status, evidence)


def check_swe2_swe3() -> Verdict:
    arch = ROOT / "docs" / "architecture.md"
    design = ROOT / "docs" / "design.md"
    evidence = []
    ok = True
    for name, path in (("architecture.md", arch), ("design.md", design)):
        if path.is_file():
            evidence.append(f"docs/{name}: present, {len(path.read_text(encoding='utf-8').splitlines())} lines")
        else:
            evidence.append(f"docs/{name}: NOT FOUND")
            ok = False
    return Verdict("SWE.2/SWE.3 Architecture & Detailed Design", "CONFIRMED" if ok else "NOT FOUND", evidence)


def check_swe4_5_6() -> Verdict:
    """Also the ONE hard-fail case: missing/stale test evidence."""
    results = sorted(glob.glob(str(ROOT / "test-results" / "*.xml")))
    if not results:
        return Verdict("SWE.4/5/6 Verification & Qualification", "NOT FOUND",
                        ["test-results/*.xml: no files — run tools/run_tests.sh"])
    total = failures = 0
    newest_result = 0.0
    for r in results:
        try:
            root = ET.parse(r).getroot()
            total += int(root.get("tests", 0))
            failures += int(root.get("failures", 0)) + int(root.get("errors", 0))
        except ET.ParseError:
            failures += 1
        newest_result = max(newest_result, Path(r).stat().st_mtime)

    rc, sources_out = run(["git", "ls-files", "src/*", "tests/*", "CMakeLists.txt", "tests/CMakeLists.txt"])
    stale = False
    if rc == 0:
        for rel in sources_out.splitlines():
            p = ROOT / rel
            if p.is_file() and p.stat().st_mtime > newest_result:
                stale = True
                break
    evidence = [f"{len(results)} suite result files, {total} tests, {failures} failures/errors",
                f"stale (a tracked source newer than every test result): {stale}"]
    if failures > 0 or stale:
        return Verdict("SWE.4/5/6 Verification & Qualification", "NOT FOUND", evidence)
    return Verdict("SWE.4/5/6 Verification & Qualification", "CONFIRMED", evidence)


def check_sup8() -> Verdict:
    rc, tags = run(["git", "tag", "--list"])
    n = len([t for t in tags.splitlines() if t.strip()]) if rc == 0 else 0
    evidence = [f"git tags: {n} found" if rc == 0 else "git tag --list failed"]
    return Verdict("SUP.8 Configuration Management", "CONFIRMED" if n > 0 else "NOT FOUND", evidence)


def check_sup9_10() -> Verdict:
    # No live GitHub API call here (this tool must work offline/in CI without a
    # token); it reports what it can check locally and names what it cannot.
    templates_dir = ROOT / ".github" / "ISSUE_TEMPLATE"
    required = {"problem_report.md", "change_request.md", "project_risk.md",
                "qa_nonconformance.md", "process_improvement.md"}
    present = {p.name for p in templates_dir.glob("*.md")} if templates_dir.is_dir() else set()
    missing = required - present
    evidence = [f".github/ISSUE_TEMPLATE/: {sorted(present & required)}",
                f"missing: {sorted(missing) if missing else 'none'}",
                "live issue/PR instance counts are NOT checked here — no GitHub API "
                "call is made offline; see the repository's Issues for the live record"]
    return Verdict("SUP.9/SUP.10 Problem & Change Management", "CONFIRMED" if not missing else "PARTIAL", evidence)


def check_man5() -> Verdict:
    register = ROOT / "process" / "risk-register.md"
    if not register.is_file():
        return Verdict("MAN.5 Risk Management", "NOT FOUND", ["process/risk-register.md absent"])
    rows = [line for line in register.read_text(encoding="utf-8").splitlines() if line.startswith("| RISK-")]
    return Verdict("MAN.5 Risk Management", "CONFIRMED" if rows else "NOT FOUND",
                   [f"process/risk-register.md: {len(rows)} risk row(s)"])


def check_process_framework_self() -> Verdict:
    version = ROOT / "process" / "VERSION"
    changelog = ROOT / "process" / "CHANGELOG.md"
    evidence = []
    ok = True
    for name, path in (("VERSION", version), ("CHANGELOG.md", changelog)):
        if path.is_file():
            evidence.append(f"process/{name}: present")
        else:
            evidence.append(f"process/{name}: NOT FOUND")
            ok = False
    return Verdict("Process Framework (self, SUP.8-style)", "CONFIRMED" if ok else "NOT FOUND", evidence)


def check_devops() -> Verdict:
    workflows = list((ROOT / ".github" / "workflows").glob("*.yml")) if (ROOT / ".github" / "workflows").is_dir() else []
    setup_sh = (ROOT / "setup.sh").is_file()
    setup_ps1 = (ROOT / "setup.ps1").is_file()
    evidence = [f"{len(workflows)} CI workflow file(s)", f"setup.sh present: {setup_sh}",
                f"setup.ps1 present: {setup_ps1}"]
    ok = bool(workflows) and setup_sh and setup_ps1
    return Verdict("DevOps Principles", "CONFIRMED" if ok else "PARTIAL", evidence)


def check_application_changelog() -> Verdict:
    changelog = ROOT / "CHANGELOG.md"
    if changelog.is_file():
        return Verdict("MAN.3 Project Management (planning artefact)", "CONFIRMED", ["CHANGELOG.md present"])
    return Verdict("MAN.3 Project Management (planning artefact)", "NOT FOUND",
                   ["CHANGELOG.md absent at repository root — docs/roadmap.md + git tags are the "
                    "actual planning record per strategies/project-management-strategy.md; this "
                    "finding is filed as RISK-003 in process/risk-register.md"])


def render(verdicts: list[Verdict]) -> str:
    lines = ["# TradingApp — Quality Assurance Report", "",
             "Generated by `tools/qa_report.py`. Answers PROCESS conformance "
             "(\"was the defined process followed\"), never product correctness "
             "— see `process/process-model.md` Section 2. Every verdict below "
             "cites the exact evidence checked.", ""]
    hard_fail = False
    for v in verdicts:
        lines.append(f"## {v.process}: **{v.status}**")
        for e in v.evidence:
            lines.append(f"- {e}")
        lines.append("")
        if v.process.startswith("SWE.4/5/6") and v.status == "NOT FOUND":
            hard_fail = True
    lines.append("## Release-gate exception")
    lines.append(
        "Per `process/process-model.md`, every finding above is informational "
        "EXCEPT missing/stale test-report evidence (SWE.4/5/6), which is a "
        "hard release-blocking finding. "
        + ("**HARD FAIL: test evidence is missing or stale.**" if hard_fail
           else "No hard fail: test evidence is present and current.")
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(ROOT / "downloads" / "TradingApp-qa-report.md"))
    args = parser.parse_args()

    verdicts = [
        check_sup1(),
        check_swe1(),
        check_swe2_swe3(),
        check_swe4_5_6(),
        check_sup8(),
        check_sup9_10(),
        check_man5(),
        check_process_framework_self(),
        check_devops(),
        check_application_changelog(),
    ]

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    report = render(verdicts)
    out_path.write_text(report, encoding="utf-8")
    print(f"wrote {out_path}")
    for v in verdicts:
        print(f"  {v.status:10s} {v.process}")

    hard_fail = any(v.process.startswith("SWE.4/5/6") and v.status == "NOT FOUND" for v in verdicts)
    if hard_fail:
        print("QA HARD FAIL: missing/stale test-report evidence (the one blocking exception)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
