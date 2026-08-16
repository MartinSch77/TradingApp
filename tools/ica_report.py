#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Run ICA (IntelligentCodeAnalyzer, a clang-based static analyzer distributed
separately under ~/ica -- see ICA_DIR below) over TradingApp's own src/ tree,
via the SAME compile database `build_all.sh build` already produced, and
render a PDF + machine-readable evidence set.

This runs BESIDE Axivion, never in place of it: Axivion's MISRA C++ 2023 +
CERT/CWE + architecture analysis remains the project's gate
(axivion/start_analysis.sh). ICA is, by its own docs/ROADMAP.md, "an early
foundation, not a finished product" -- this report is informational evidence
of what a second, independent clang-based analyzer finds on the same code,
not a second gate. Nothing here fails a build; see main()'s exit codes.

ICA must already be present at ICA_DIR (default ~/ica) with a built
bin/ica-cc + bin/ica-cxx -- this script does NOT install or build ICA itself
(unlike the seven bundled analyzers, ICA is not one of setup.sh's fetched
tools). Checked first, honestly: if it's missing, this script says so and
exits 3 (this project's own "stage skipped, not failed" convention) rather
than silently doing nothing or fabricating a report.

Scope, stated rather than left to be discovered: src/ only (86 TUs measured
2026-08-16), not tests/ -- matching the "product code under scrutiny, tests
are verification artifacts" scope the MISRA-style analyzers already use
elsewhere in this project. Generated code (moc/ui) is skipped by ICA's own
default (ICA_ANALYZE_GENERATED unset).

Usage: tools/ica_report.py [--ica-dir DIR] [--build-dir DIR] [--out-dir DIR]
                           [--pdf PATH] [--jobs N]
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import shlex
import subprocess
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXIT_SKIPPED = 3

# Product code only -- see the module docstring's "Scope" paragraph.
SOURCE_DIRS = ("src",)

# ICA's own docs/ISO25010_MAPPING.md, category-level table, reproduced here
# (not parsed from that file: it is prose documentation, not a data file) so
# the PDF can frame every finding by the ISO/IEC 25010 characteristic it
# threatens, not just ICA's own internal Category label.
CATEGORY_ISO25010 = {
    "safety": "Reliability (+ Functional Suitability: Correctness)",
    "security": "Security (+ Reliability)",
    "maintainability": "Maintainability",
    "portability": "Portability (+ Maintainability)",
    "metric": "Maintainability (Analysability / Testability / Modularity)",
}


def find_ica(ica_dir: Path) -> tuple[Path, Path] | None:
    cc, cxx = ica_dir / "bin" / "ica-cc", ica_dir / "bin" / "ica-cxx"
    if cc.is_file() and cxx.is_file() and os.access(cxx, os.X_OK):
        return cc, cxx
    return None


def tu_entries(build_dir: Path, root: Path) -> list[dict]:
    db_path = build_dir / "compile_commands.json"
    if not db_path.is_file():
        return []
    with db_path.open(encoding="utf-8") as handle:
        database = json.load(handle)
    prefixes = tuple(str(root / d) + os.sep for d in SOURCE_DIRS)
    return [e for e in database if os.path.abspath(e["file"]).startswith(prefixes)]


def _retarget_output(args: list[str], scratch_dir: Path, file_path: str) -> list[str]:
    """Redirect -o to a SCRATCH object file, never the real build tree's own
    .o path: ICA performs a genuine compile as half of what it does, and this
    report must not race the release pipeline's own build/ artifacts (still
    read by later stages) or silently rewrite them mid-evidence-collection."""
    stem = hashlib.sha1(file_path.encode("utf-8")).hexdigest()[:16]
    scratch_obj = str(scratch_dir / f"{stem}.o")
    out, skip = [], False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg == "-o":
            out.append(arg)
            out.append(scratch_obj)
            skip = True
            continue
        out.append(arg)
    return out


def run_one(entry: dict, ica_cxx: Path, env: dict, scratch_dir: Path) -> str | None:
    command = entry.get("arguments") or shlex.split(entry["command"])
    real_compiler = command[0]
    args = _retarget_output([str(ica_cxx), *command[1:]], scratch_dir, entry["file"])
    tu_env = dict(env)
    tu_env["ICA_REAL_CXX"] = real_compiler
    try:
        run = subprocess.run(args, cwd=entry["directory"], env=tu_env,
                             capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return f'{entry["file"]}: ica timed out'
    if run.returncode != 0:
        tail = (run.stderr.strip() or run.stdout.strip()).splitlines()
        last = tail[-1] if tail else "no output"
        return f'{entry["file"]}: ica exited {run.returncode}: {last}'
    return None


def run_ica(entries: list[dict], ica_cxx: Path, out_dir: Path, jobs: int) -> list[str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    scratch_dir = out_dir / "objects"
    scratch_dir.mkdir(exist_ok=True)
    for stale in ("ica.json", "ica_metrics.json", "ica_odr.db", "ica_clones.db"):
        (out_dir / stale).unlink(missing_ok=True)

    env = dict(os.environ)
    env.update({
        "ICA_REPORT_JSON": str(out_dir / "ica.json"),
        "ICA_REPORT_METRICS_JSON": str(out_dir / "ica_metrics.json"),
        "ICA_ODR_DB": str(out_dir / "ica_odr.db"),
        "ICA_CLONE_DB": str(out_dir / "ica_clones.db"),
        # The dead-function whole-program report needs a separate tool
        # (tools/ica_dead_code_report.py) that this binary-only release
        # package does not ship -- collecting data nobody can read is
        # pointless disk I/O, so this whole-program check is off rather
        # than silently incomplete.
        "ICA_DEADCODE_DISABLED": "1",
    })

    errors: list[str] = []
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_one, e, ica_cxx, env, scratch_dir) for e in entries]
        for future in cf.as_completed(futures):
            result = future.result()
            if result:
                errors.append(result)
    return errors


def load_json_array(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def relativize(file_path: str) -> str:
    try:
        return str(Path(file_path).resolve().relative_to(ROOT))
    except ValueError:
        return file_path


def build_pdf(pdf_path: Path, findings: list[dict], metrics: list[dict],
              errors: list[str], entry_count: int, ica_version_note: str) -> None:
    from reportlab.lib import colors
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
    from reportlab.lib.units import mm
    from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer, Table,
                                    TableStyle, PageBreak)

    styles = getSampleStyleSheet()
    small = ParagraphStyle("small", parent=styles["BodyText"], fontSize=8, leading=10)
    doc = SimpleDocTemplate(str(pdf_path), pagesize=A4,
                            topMargin=18 * mm, bottomMargin=18 * mm,
                            leftMargin=16 * mm, rightMargin=16 * mm)
    story = []

    story.append(Paragraph("TradingApp -- ICA static-analysis report", styles["Title"]))
    story.append(Paragraph(
        f"Generated {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} "
        f"by tools/ica_report.py &middot; {ica_version_note}", styles["Normal"]))
    story.append(Spacer(1, 6 * mm))
    story.append(Paragraph(
        "<b>Scope and status.</b> ICA is a second, independent clang-based static "
        "analyzer, run BESIDE Axivion -- it does not replace Axivion's MISRA C++ 2023 "
        "+ CERT/CWE + architecture gate, and nothing in this report fails a build. "
        f"Analyzed: src/ only ({entry_count} translation units) -- tests/ and generated "
        "code (moc/ui) are out of scope for this run, the same convention this "
        "project's other MISRA-style analyzers use. ICA describes itself as \"an early "
        "foundation, not a finished product\" (its own docs/ROADMAP.md); read every "
        "number below with that in mind, not as a finished-product benchmark.",
        styles["BodyText"]))
    story.append(Spacer(1, 6 * mm))

    if errors:
        story.append(Paragraph(f"<b>{len(errors)} translation unit(s) ICA could not "
                                "analyze</b> (crash, timeout, or bad flags -- reported "
                                "rather than silently dropped):", styles["BodyText"]))
        for line in errors[:25]:
            story.append(Paragraph(relativize(line), small))
        if len(errors) > 25:
            story.append(Paragraph(f"... and {len(errors) - 25} more", small))
        story.append(Spacer(1, 6 * mm))

    real_findings = [f for f in findings if f.get("category") != "metric"]
    by_rule = Counter(f["ruleId"] for f in real_findings)
    by_category = Counter(f.get("category", "unknown") for f in real_findings)
    by_severity = Counter(f.get("severity", "unknown") for f in real_findings)
    by_file = Counter(relativize(f["file"]) for f in real_findings)

    story.append(Paragraph(f"<b>Total findings: {len(real_findings)}</b> "
                            f"over {entry_count} translation units.", styles["Heading2"]))

    def counter_table(title: str, counter: Counter, col_label: str, iso_map: bool = False) -> None:
        story.append(Paragraph(title, styles["Heading3"]))
        header = [col_label, "Count"] + (["Primary ISO/IEC 25010 characteristic"] if iso_map else [])
        rows = [header]
        for key, count in counter.most_common(30):
            row = [str(key), str(count)]
            if iso_map:
                row.append(CATEGORY_ISO25010.get(str(key).lower(), "(see docs/ISO25010_MAPPING.md)"))
            rows.append(row)
        table = Table(rows, repeatRows=1)
        table.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#2c3e50")),
            ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
            ("FONTSIZE", (0, 0), (-1, -1), 8),
            ("GRID", (0, 0), (-1, -1), 0.4, colors.grey),
            ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f2f2f2")]),
        ]))
        story.append(table)
        story.append(Spacer(1, 5 * mm))

    counter_table("By ISO/IEC 25010 category", by_category, "Category", iso_map=True)
    counter_table("By severity", by_severity, "Severity")
    counter_table("By rule (top 30)", by_rule, "Rule ID")
    counter_table("By file (top 30)", by_file, "File")

    story.append(PageBreak())
    story.append(Paragraph("Whole-program checks", styles["Heading2"]))
    odr = by_rule.get("MISRA-CPP2023-6.2.1", 0)
    clone = by_rule.get("ICA-CODE-CLONE", 0)
    story.append(Paragraph(
        f"One-Definition-Rule violations (MISRA-CPP2023-6.2.1): <b>{odr}</b>. "
        f"Cross-translation-unit clone detection (ICA-CODE-CLONE): <b>{clone}</b> "
        "-- both accumulate automatically across every analyzed TU above; "
        "already included in the tables above, not double-counted here.",
        styles["BodyText"]))
    story.append(Paragraph(
        "Dead-function (whole-program \"never called anywhere\") analysis was NOT run: "
        "it needs tools/ica_dead_code_report.py, which this binary-only ICA release "
        "package does not include. Stated rather than silently omitted.",
        styles["BodyText"]))
    story.append(Spacer(1, 6 * mm))

    story.append(Paragraph("Code metrics", styles["Heading2"]))
    loc_metrics = [m for m in metrics if m.get("ruleId") == "ICA-METRIC-LOC"]
    ccn_metrics = [m for m in metrics if m.get("ruleId") == "ICA-METRIC-COMPLEXITY-VALUE"]
    if ccn_metrics:
        def ccn_value(m: dict) -> int:
            # "function 'name' has cyclomatic complexity N"
            try:
                return int(m["message"].rsplit(" ", 1)[-1])
            except (ValueError, KeyError, IndexError):
                return 0
        values = [ccn_value(m) for m in ccn_metrics]
        over_10 = sum(1 for v in values if v > 10)
        story.append(Paragraph(
            f"Functions measured: <b>{len(ccn_metrics)}</b>. "
            f"Average cyclomatic complexity: <b>{(sum(values) / len(values)):.2f}</b>. "
            f"Max: <b>{max(values)}</b>. Over the default threshold (10): <b>{over_10}</b>.",
            styles["BodyText"]))
    if loc_metrics:
        story.append(Paragraph(f"Functions with an LOC measurement: {len(loc_metrics)}.",
                                styles["BodyText"]))
    story.append(Spacer(1, 4 * mm))
    story.append(Paragraph(
        "Metrics are measurements, never violations (ICA's own Category::Metric), "
        "reported here for context alongside the finding counts above -- not as a "
        "second ratchet gate; tools/lizard_metrics.py remains this project's own "
        "metrics ratchet.", small))

    doc.build(story)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ica-dir", default=os.environ.get("ICA_DIR", str(Path.home() / "ica")))
    parser.add_argument("--build-dir", default=str(ROOT / "build"))
    parser.add_argument("--out-dir", default=str(ROOT / "analysis-results" / "ica"))
    parser.add_argument("--pdf", default=str(ROOT / "downloads" / "TradingApp-ica-report.pdf"))
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 4))
    args = parser.parse_args()

    ica_dir = Path(args.ica_dir)
    located = find_ica(ica_dir)
    if not located:
        print(f"ica_report: no ICA install found at {ica_dir} "
              f"(expected bin/ica-cc + bin/ica-cxx) -- skipped, not run")
        return EXIT_SKIPPED
    _, ica_cxx = located

    build_dir = Path(args.build_dir)
    entries = tu_entries(build_dir, ROOT)
    if not entries:
        print(f"ica_report: no src/ translation units in "
              f"{build_dir / 'compile_commands.json'} -- build the project first")
        return EXIT_SKIPPED

    out_dir = Path(args.out_dir)
    print(f"ica_report: running ICA over {len(entries)} src/ translation units "
          f"({args.jobs} parallel jobs)...")
    errors = run_ica(entries, ica_cxx, out_dir, args.jobs)

    findings = load_json_array(out_dir / "ica.json")
    metrics = load_json_array(out_dir / "ica_metrics.json")
    real_count = sum(1 for f in findings if f.get("category") != "metric")
    print(f"ica_report: {real_count} findings, {len(metrics)} metric records, "
          f"{len(errors)} TU(s) failed to analyze")

    pdf_path = Path(args.pdf)
    pdf_path.parent.mkdir(parents=True, exist_ok=True)
    version_note = f"ICA at {ica_dir}"
    manifest = ica_dir / "MANIFEST.txt"
    if manifest.is_file():
        for line in manifest.read_text(encoding="utf-8").splitlines():
            if line.startswith("Version:") or line.startswith("Git commit:"):
                version_note += f" ({line.strip()})"
    build_pdf(pdf_path, findings, metrics, errors, len(entries), version_note)
    print(f"ica_report: wrote {pdf_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
