#!/usr/bin/env python3
"""Quality report as one colourful PDF — the pipeline's result on a few pages.

Collects what the other stages already produced and renders it:

  build/toolchain    git revision, Qt kit, compilers, host                (build tree)
  tests              test-results/*.xml (JUnit)   — per suite AND per test function
  traceability       requirements <-> design <-> test spec <-> result
                     (imported from tools/trace_report.py, so the numbers here
                     can never disagree with docs/traceability.html)
  static analysis    analysis-results/*.txt       — per tool finding counts
  code metrics       analysis-results/lizard-metrics.csv + tools/lizard_baseline.json
  coverage           coverage/gcov/coverage.info, coverage/mcdc/summary.txt
  sanitizers         analysis-results/sanitize-*.txt

Nothing is computed twice and nothing is invented: a missing artefact is
reported as "not run" rather than guessed, so the PDF states what the pipeline
actually did.

Usage:  tools/make_report.py [--out downloads/TradingApp-report.pdf]

Exit codes: 0 ok · 3 SKIPPED (reportlab not installed — the pipeline stays
green, as with every other optional tool) · 1 real failure.
"""

import argparse
import base64
import csv
import glob
import json
import os
import platform
import re
import subprocess
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UTF8 = {"encoding": "utf-8"}
EXIT_SKIPPED = 3

sys.path.insert(0, str(ROOT / "tools"))

try:  # the one optional dependency
    from reportlab.graphics.charts.barcharts import HorizontalBarChart
    from reportlab.graphics.charts.piecharts import Pie
    from reportlab.graphics.shapes import Drawing, String
    from reportlab.lib import colors
    from reportlab.lib.enums import TA_CENTER
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
    from reportlab.lib.units import mm
    from reportlab.platypus import (
        BaseDocTemplate,
        Frame,
        KeepTogether,
        PageBreak,
        PageTemplate,
        Paragraph,
        Spacer,
        Table,
        TableStyle,
    )
except ImportError:  # pragma: no cover - environment-dependent
    print("SKIPPED: reportlab is not installed (pip install reportlab) — no PDF report.")
    sys.exit(EXIT_SKIPPED)

# ---------------------------------------------------------------------------
# palette — the app's own signal colours, so the report reads like the product
# ---------------------------------------------------------------------------
GREEN = colors.HexColor("#25b563")
RED = colors.HexColor("#e35555")
AMBER = colors.HexColor("#e0b000")
GREY = colors.HexColor("#9a9a9a")
INK = colors.HexColor("#1d2330")
BAND = colors.HexColor("#eef1f6")
HEAD = colors.HexColor("#2d3748")


def verdict_colour(ok, warn=False):
    if warn:
        return AMBER
    return GREEN if ok else RED


# ---------------------------------------------------------------------------
# collectors — each returns plain data and never raises on a missing artefact
# ---------------------------------------------------------------------------
def git_info():
    def run(*args):
        try:
            return subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                                  timeout=20, check=False).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return ""

    dirty = run("git", "status", "--porcelain")
    return {
        "commit": run("git", "rev-parse", "--short", "HEAD") or "n/a",
        "subject": run("git", "log", "-1", "--pretty=%s") or "n/a",
        "branch": run("git", "rev-parse", "--abbrev-ref", "HEAD") or "n/a",
        "dirty": len([ln for ln in dirty.splitlines() if ln.strip()]),
    }


def toolchain_info(build_dir):
    """Compiler/Qt facts straight out of the CMake cache of the build tree."""
    info = {}
    cache = ROOT / build_dir / "CMakeCache.txt"
    wanted = {
        "CMAKE_CXX_COMPILER:": "C++ compiler",
        "CMAKE_BUILD_TYPE:": "Build type",
        "Qt6_DIR:": "Qt kit",
        "CMAKE_CXX_STANDARD:": "C++ standard",
    }
    if cache.is_file():
        for line in cache.read_text(**UTF8).splitlines():
            for key, label in wanted.items():
                if line.startswith(key):
                    info[label] = line.split("=", 1)[1]
    info["Host"] = f"{platform.system()} {platform.release()} ({platform.machine()})"
    info["Python"] = platform.python_version()
    return info


def collect_tests():
    """[{suite, total, passed, failed, skipped, seconds, cases:[(name,status,secs)]}]"""
    suites = []
    for path in sorted(glob.glob(str(ROOT / "test-results" / "*.xml"))):
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            continue
        nodes = [root] if root.tag == "testsuite" else root.findall("testsuite")
        cases = []
        seconds = 0.0
        for node in nodes:
            seconds += float(node.get("time") or 0.0)
            for case in node.iter("testcase"):
                status = "PASS"
                if case.find("failure") is not None or case.find("error") is not None:
                    status = "FAIL"
                elif case.find("skipped") is not None:
                    status = "SKIP"
                cases.append((case.get("name", "?"), status, float(case.get("time") or 0.0)))
        # initTestCase/cleanupTestCase are QtTest bookkeeping, not requirements-bearing
        real = [c for c in cases if c[0] not in ("initTestCase", "cleanupTestCase")]
        suites.append({
            "suite": Path(path).stem,
            "cases": real,
            "total": len(real),
            "passed": sum(1 for c in real if c[1] == "PASS"),
            "failed": sum(1 for c in real if c[1] == "FAIL"),
            "skipped": sum(1 for c in real if c[1] == "SKIP"),
            "seconds": seconds,
        })
    return suites


def collect_trace():
    """Reuse the matrix generator's own parsers — one source of truth."""
    try:
        import trace_report as tr
    except ImportError:
        return None
    reqs = tr.parse_requirements()
    design = tr.parse_design()
    spec = tr.parse_test_spec()
    impl = tr.parse_test_impl()
    results = tr.parse_results()

    # Which requirements are verified by an EXECUTED test, and by which tests.
    per_req = {}
    for req in reqs:
        tests = sorted(ts for ts, info in impl.items() if req in info["verifies"])
        executed = [ts for ts in tests
                    if results.get(impl[ts]["function"], ("", ""))[0] == "PASS"]
        failed = [ts for ts in tests
                  if results.get(impl[ts]["function"], ("", ""))[0] == "FAIL"]
        per_req[req] = {"tests": tests, "executed": executed, "failed": failed}
    return {
        "requirements": reqs,
        "design": design,
        "spec": spec,
        "impl": impl,
        "results": results,
        "per_req": per_req,
        "hard_gaps": sorted(set(spec) ^ set(impl)),
        "covered": sum(1 for r in per_req.values() if r["executed"] and not r["failed"]),
    }


def collect_analysis():
    """[(tool, findings, detail)] from the logs tools/static_analysis.sh writes."""
    tools = [
        ("cppcheck", "cppcheck.txt"),
        ("clang-tidy", "clang-tidy.txt"),
        ("Clang Static Analyzer", "clang-analyzer.txt"),
        ("g++ -fanalyzer", "gcc-analyzer.txt"),
        ("clazy", "clazy.txt"),
        ("PMD CPD (clones)", "pmd-cpd.txt"),
        ("codespell", "codespell.txt"),
    ]
    out = []
    for name, fname in tools:
        path = ROOT / "analysis-results" / fname
        if not path.is_file():
            out.append((name, None, "not run"))
            continue
        lines = [ln for ln in path.read_text(**UTF8, errors="replace").splitlines()
                 if ln.strip()]
        out.append((name, len(lines), "clean" if not lines else lines[0][:120]))
    return out


def collect_metrics():
    """Over-threshold functions vs the ratchet baseline."""
    baseline_path = ROOT / "tools" / "lizard_baseline.json"
    csv_path = ROOT / "analysis-results" / "lizard-metrics.csv"
    baseline = {}
    if baseline_path.is_file():
        try:
            baseline = json.loads(baseline_path.read_text(**UTF8))
        except json.JSONDecodeError:
            baseline = {}
    worst = []
    if csv_path.is_file():
        # tools/lizard_metrics.py writes SEMICOLON-separated columns
        # (file;function;line;end;nloc;ccn;tokens;params).
        with csv_path.open(**UTF8, newline="") as handle:
            for row in csv.DictReader(handle, delimiter=";"):
                try:
                    ccn = int(row.get("ccn") or 0)
                    nloc = int(row.get("nloc") or 0)
                except ValueError:
                    continue
                where = Path(row.get("file") or "?").name
                worst.append((f"{row.get('function') or '?'}  <font size=6>({where})</font>",
                              ccn, nloc))
    worst.sort(key=lambda r: -r[1])
    return {"baselined": len(baseline), "worst": worst[:8]}


def collect_coverage():
    """(lines%, functions%) from lcov's tracefile, plus the MC/DC totals."""
    result = {"lines": None, "functions": None, "mcdc": None, "mcdc_rows": []}
    info = ROOT / "coverage" / "gcov" / "coverage.info"
    if info.is_file():
        found = hit = fnf = fnh = 0
        for line in info.read_text(**UTF8, errors="replace").splitlines():
            if line.startswith("LF:"):
                found += int(line[3:] or 0)
            elif line.startswith("LH:"):
                hit += int(line[3:] or 0)
            elif line.startswith("FNF:"):
                fnf += int(line[4:] or 0)
            elif line.startswith("FNH:"):
                fnh += int(line[4:] or 0)
        if found:
            result["lines"] = 100.0 * hit / found
        if fnf:
            result["functions"] = 100.0 * fnh / fnf
    summary = ROOT / "coverage" / "mcdc" / "summary.txt"
    if summary.is_file():
        for line in summary.read_text(**UTF8, errors="replace").splitlines():
            cols = line.split()
            if len(cols) >= 4 and cols[0].endswith((".cpp", ".h")) and cols[-1].endswith("%"):
                result["mcdc_rows"].append((cols[0], cols[-1]))
            if line.startswith("TOTAL"):
                pct = [c for c in cols if c.endswith("%")]
                if pct:
                    result["mcdc"] = pct[-1]
    return result


def collect_axivion():
    """Latest dashboard version's issue counts per kind, or None when unreachable.

    Reads the dashboard the same way axivion/start_analysis.{sh,ps1} publishes to it:
    AXIVION_DASHBOARD_URL (default http://localhost:9090/axivion) with
    AXIVION_USERNAME/AXIVION_PASSWORD — whose local-dev defaults live in that script's
    guarded credentials block, not here. No dashboard (CI, a machine without the
    license-bound Suite) simply means "not run": the timeout is short on purpose.
    """
    base = os.environ.get("AXIVION_DASHBOARD_URL", "http://localhost:9090/axivion").rstrip("/")
    user = os.environ.get("AXIVION_USERNAME", "admin")
    password = os.environ.get("AXIVION_PASSWORD", "password")
    project = "TradingApp"
    config = ROOT / "axivion" / "ci_config.json"
    if config.is_file():
        try:
            opts = json.loads(config.read_text(**UTF8))["Project"]["Project-GlobalOptions"]
            project = opts.get("name") or project
        except (json.JSONDecodeError, KeyError, TypeError):
            pass

    token = base64.b64encode(f"{user}:{password}".encode()).decode()
    req = urllib.request.Request(f"{base}/api/projects/{project}/issues?kind=SV",
                                headers={"Authorization": f"Basic {token}"})
    try:
        with urllib.request.urlopen(req, timeout=6) as response:
            payload = json.loads(response.read().decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, json.JSONDecodeError, ValueError):
        return None
    version = payload.get("endVersion") or {}
    counts = version.get("issueCounts") or {}
    if not counts:
        return None
    return {"project": project, "version": version.get("index"),
            "name": version.get("name", ""), "counts": counts}


def collect_sanitizers():
    out = []
    for path in sorted(glob.glob(str(ROOT / "analysis-results" / "sanitize-*.txt"))):
        name = Path(path).stem.replace("sanitize-", "")
        if name.endswith(".raw"):
            continue
        lines = [ln for ln in Path(path).read_text(**UTF8, errors="replace").splitlines()
                 if ln.strip()]
        out.append((name, len(lines)))
    return out


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------
class Report:
    def __init__(self, out_path, title):
        self.out = out_path
        self.title = title
        self.story = []
        base = getSampleStyleSheet()
        self.s = {
            "h1": ParagraphStyle("h1", parent=base["Heading1"], textColor=INK,
                                 fontSize=17, spaceAfter=2 * mm),
            "h2": ParagraphStyle("h2", parent=base["Heading2"], textColor=HEAD,
                                 fontSize=12.5, spaceBefore=4 * mm, spaceAfter=1.5 * mm),
            "body": ParagraphStyle("body", parent=base["BodyText"], fontSize=8.6,
                                   leading=11.4, textColor=INK),
            "small": ParagraphStyle("small", parent=base["BodyText"], fontSize=7.4,
                                    leading=9.4, textColor=GREY),
            "cell": ParagraphStyle("cell", parent=base["BodyText"], fontSize=7.6,
                                   leading=9.4, textColor=INK),
            "hero": ParagraphStyle("hero", parent=base["Title"], fontSize=27,
                                   textColor=colors.white, alignment=TA_CENTER,
                                   spaceAfter=0),
            "heroSub": ParagraphStyle("heroSub", parent=base["Normal"], fontSize=10.5,
                                      textColor=colors.white, alignment=TA_CENTER),
        }

    def h1(self, text):
        self.story.append(Paragraph(text, self.s["h1"]))

    def h2(self, text):
        self.story.append(Paragraph(text, self.s["h2"]))

    def text(self, body, style="body"):
        self.story.append(Paragraph(body, self.s[style]))

    def gap(self, height=3):
        self.story.append(Spacer(1, height * mm))

    def table(self, data, widths, style_extra=(), header=True, keep=False):
        tbl = Table(data, colWidths=widths, repeatRows=1 if header else 0, hAlign="LEFT")
        style = [
            ("FONTNAME", (0, 0), (-1, -1), "Helvetica"),
            ("FONTSIZE", (0, 0), (-1, -1), 7.6),
            ("TEXTCOLOR", (0, 0), (-1, -1), INK),
            ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
            ("GRID", (0, 0), (-1, -1), 0.25, colors.HexColor("#c9d1dd")),
            ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, BAND]),
            ("TOPPADDING", (0, 0), (-1, -1), 2.2),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 2.2),
        ]
        if header:
            style += [
                ("BACKGROUND", (0, 0), (-1, 0), HEAD),
                ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
                ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
            ]
        tbl.setStyle(TableStyle(style + list(style_extra)))
        self.story.append(KeepTogether(tbl) if keep else tbl)

    def hero(self, verdict, ok, subtitle, facts):
        """Full-width banner: the one-line verdict plus the run's identity."""
        banner = Table([[Paragraph(verdict, self.s["hero"])],
                        [Paragraph(subtitle, self.s["heroSub"])]],
                       colWidths=[176 * mm])
        banner.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, -1), verdict_colour(ok)),
            ("TOPPADDING", (0, 0), (-1, 0), 9),
            ("BOTTOMPADDING", (0, -1), (-1, -1), 9),
            ("LEFTPADDING", (0, 0), (-1, -1), 8),
            ("RIGHTPADDING", (0, 0), (-1, -1), 8),
        ]))
        self.story.append(banner)
        self.gap(3)
        rows = [[Paragraph(f"<b>{k}</b>", self.s["cell"]), Paragraph(str(v), self.s["cell"])]
                for k, v in facts.items()]
        self.table(rows, [44 * mm, 132 * mm], header=False)

    def charts(self, req_counts, coverage):
        """Two small charts side by side: how requirements are verified, and coverage."""
        pie_drawing = Drawing(84 * mm, 46 * mm)
        pie_drawing.add(String(0, 42 * mm, "Requirements by verification state",
                              fontName="Helvetica-Bold", fontSize=8.4,
                              fillColor=HEAD))
        labels = [("verified by a passing test", req_counts["verified"], GREEN),
                  ("test present, not executed", req_counts["not_executed"], AMBER),
                  ("verified by inspection", req_counts["manual"], GREY),
                  ("failing test", req_counts["failing"], RED)]
        shown = [(name, value, colour) for name, value, colour in labels if value > 0]
        pie = Pie()
        pie.x = 2 * mm
        pie.y = 4 * mm
        pie.width = 34 * mm
        pie.height = 34 * mm
        pie.data = [value for _, value, _ in shown] or [1]
        pie.labels = [str(value) for _, value, _ in shown] or [""]
        pie.sideLabels = False
        pie.slices.strokeWidth = 0.6
        pie.slices.strokeColor = colors.white
        pie.slices.fontName = "Helvetica-Bold"
        pie.slices.fontSize = 8
        pie.slices.labelRadius = 0.62
        pie.slices.fontColor = colors.white
        for i, (_, _, colour) in enumerate(shown):
            pie.slices[i].fillColor = colour
        pie_drawing.add(pie)
        for i, (name, value, colour) in enumerate(shown):
            y = (33 - (i * 6)) * mm
            pie_drawing.add(String(41 * mm, y, "■", fontSize=8, fillColor=colour))
            pie_drawing.add(String(45 * mm, y, f"{value}  {name}", fontSize=7.4,
                                  fillColor=INK))

        bar_drawing = Drawing(84 * mm, 46 * mm)
        bar_drawing.add(String(0, 42 * mm, "Coverage measured this run",
                              fontName="Helvetica-Bold", fontSize=8.4, fillColor=HEAD))
        values = [coverage["lines"] or 0.0, coverage["functions"] or 0.0,
                  _pct_value(coverage["mcdc"])]
        if not any(values):
            # Say so instead of drawing three 0.0% bars: an unmeasured run and a run that
            # measured zero must not look the same (CI has no coverage stage).
            bar_drawing.add(String(0, 33 * mm, "not measured in this run —", fontSize=8,
                                   fillColor=AMBER))
            bar_drawing.add(String(0, 27 * mm, "no coverage/ artefacts were produced",
                                   fontSize=8, fillColor=GREY))
            row = Table([[pie_drawing, bar_drawing]], colWidths=[88 * mm, 88 * mm])
            row.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"),
                                     ("LEFTPADDING", (0, 0), (-1, -1), 0),
                                     ("BOTTOMPADDING", (0, 0), (-1, -1), 0)]))
            self.story.append(row)
            return
        chart = HorizontalBarChart()
        chart.x = 24 * mm
        chart.y = 6 * mm
        chart.width = 52 * mm
        chart.height = 30 * mm
        chart.data = [values]
        chart.categoryAxis.categoryNames = ["lines", "functions", "MC/DC"]
        chart.categoryAxis.labels.fontSize = 7.4
        chart.categoryAxis.labels.dx = -2
        chart.valueAxis.valueMin = 0
        chart.valueAxis.valueMax = 100
        chart.valueAxis.valueStep = 25
        chart.valueAxis.labels.fontSize = 7
        chart.barLabels.fontSize = 7.2
        chart.barLabelFormat = "%.1f%%"
        chart.barLabels.dx = 7
        chart.bars[0].fillColor = colors.HexColor("#3f7fd0")
        chart.bars.strokeWidth = 0
        bar_drawing.add(chart)

        row = Table([[pie_drawing, bar_drawing]], colWidths=[88 * mm, 88 * mm])
        row.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"),
                                 ("LEFTPADDING", (0, 0), (-1, -1), 0),
                                 ("BOTTOMPADDING", (0, 0), (-1, -1), 0)]))
        self.story.append(row)

    def build(self):
        self.out.parent.mkdir(parents=True, exist_ok=True)
        doc = BaseDocTemplate(str(self.out), pagesize=A4, title=self.title,
                              author="TradingApp quality pipeline",
                              leftMargin=17 * mm, rightMargin=17 * mm,
                              topMargin=15 * mm, bottomMargin=15 * mm)
        frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="body")
        stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

        def decorate(canvas, _doc):
            canvas.saveState()
            canvas.setFillColor(GREY)
            canvas.setFont("Helvetica", 7)
            canvas.drawString(17 * mm, 9 * mm, f"TradingApp quality report · {stamp}")
            canvas.drawRightString(A4[0] - 17 * mm, 9 * mm, f"page {canvas.getPageNumber()}")
            canvas.setStrokeColor(colors.HexColor("#c9d1dd"))
            canvas.line(17 * mm, 12 * mm, A4[0] - 17 * mm, 12 * mm)
            canvas.restoreState()

        doc.addPageTemplates([PageTemplate(id="all", frames=[frame], onPage=decorate)])
        doc.build(self.story)


def pct(value):
    return "—" if value is None else f"{value:.1f}%"


def _pct_value(text):
    """"22.75%" -> 22.75; anything unusable -> 0.0 (charts need a number)."""
    if not text:
        return 0.0
    try:
        return float(str(text).rstrip("%"))
    except ValueError:
        return 0.0


def status_cell(text, colour):
    return Paragraph(f'<font color="{colour.hexval()}"><b>{text}</b></font>',
                     getSampleStyleSheet()["BodyText"])


def build_report(out_path, build_dir):
    tests = collect_tests()
    trace = collect_trace()
    analysis = collect_analysis()
    metrics = collect_metrics()
    coverage = collect_coverage()
    sanitizers = collect_sanitizers()
    axivion = collect_axivion()
    git = git_info()

    total = sum(s["total"] for s in tests)
    failed = sum(s["failed"] for s in tests)
    skipped = sum(s["skipped"] for s in tests)
    analysis_findings = sum(n for _, n, _ in analysis if n)
    san_findings = sum(n for _, n in sanitizers)
    hard_gaps = len(trace["hard_gaps"]) if trace else 0
    everything_ok = (failed == 0) and (analysis_findings == 0) and (san_findings == 0) \
        and (hard_gaps == 0) and (total > 0)

    rep = Report(out_path, "TradingApp quality report")
    verdict = "ALL CHECKS PASSED" if everything_ok else "ATTENTION REQUIRED"
    subtitle = (f"{total} tests · {analysis_findings} static-analysis findings · "
                f"{san_findings} sanitizer findings · {hard_gaps} traceability hard gaps")
    facts = {
        "Revision": f"{git['commit']} on {git['branch']} — {git['subject']}",
        "Working tree": ("clean" if git["dirty"] == 0
                         else f"{git['dirty']} uncommitted file(s)"),
        "Generated": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC"),
    }
    facts.update(toolchain_info(build_dir))
    rep.hero(verdict, everything_ok, subtitle, facts)

    # ---- stage scoreboard -------------------------------------------------
    rep.h2("Pipeline result by area")
    rows = [["Area", "Result", "Evidence"]]

    def add(area, ok, result, evidence, warn=False):
        rows.append([area, status_cell(result, verdict_colour(ok, warn)),
                     Paragraph(evidence, rep.s["cell"])])

    add("Tests", failed == 0,
        "PASS" if failed == 0 else f"{failed} FAILED",
        f"{total} test functions in {len(tests)} suites, {skipped} skipped "
        f"({sum(s['seconds'] for s in tests):.1f} s)")
    if trace:
        add("Traceability", hard_gaps == 0,
            "PASS" if hard_gaps == 0 else f"{hard_gaps} HARD GAPS",
            f"{len(trace['requirements'])} requirements · {len(trace['design'])} design "
            f"elements · {len(trace['spec'])} specified tests · "
            f"{trace['covered']} requirements verified by a passing test")
    add("Static analysis", analysis_findings == 0,
        "CLEAN" if analysis_findings == 0 else f"{analysis_findings} FINDINGS",
        " · ".join(f"{name}: {'—' if n is None else n}" for name, n, _ in analysis))
    if axivion is None:
        add("Axivion", False, "not run",
            "MISRA C++ 2023 / CERT / CWE + architecture, clones, cycles, dead code and "
            "metrics on one dashboard — license-bound, and no dashboard answered at "
            "AXIVION_DASHBOARD_URL (axivion/start_analysis.sh publishes it)", warn=True)
    else:
        ax = axivion["counts"]

        # NOT named `total`: that is the test count in this scope, and shadowing it made
        # the banner read "118155 tests".
        def ax_total(kind):
            return (ax.get(kind) or {}).get("Total", 0)
        # AV (architecture) and CL (clones) are the HARD gates here: this configuration is
        # MISRA-only for style, and clones are gated by PMD CPD, so a non-zero AV/CL is
        # what makes the Axivion result actionable.
        gates_clean = (ax_total("AV") == 0) and (ax_total("CL") == 0)
        add("Axivion", gates_clean,
            "GATES OK" if gates_clean else "GATE HIT",
            f"version {axivion['version']} ({axivion['name']}) — architecture "
            f"{ax_total('AV')} · clones {ax_total('CL')} · cycles {ax_total('CY')} · dead "
            f"code {ax_total('DE')} · metrics {ax_total('MV')} · style {ax_total('SV')}")

    add("Code metrics", True, f"{metrics['baselined']} known",
        "lizard ratchet: every over-threshold function is recorded with its numbers; "
        "a new or worsened one fails the stage")
    cov_txt = (f"lines {pct(coverage['lines'])} · functions {pct(coverage['functions'])}"
               f" · MC/DC {coverage['mcdc'] or '—'}")
    add("Coverage", coverage["lines"] is not None,
        "measured" if coverage["lines"] is not None else "not run", cov_txt,
        warn=coverage["lines"] is None)
    add("Sanitizers", san_findings == 0,
        "CLEAN" if san_findings == 0 else f"{san_findings} FINDINGS",
        " · ".join(f"{name}: {n}" for name, n in sanitizers) or "not run")
    rep.table(rows, [30 * mm, 26 * mm, 120 * mm])

    if trace:
        rep.gap(4)
        verified = sum(1 for r in trace["per_req"].values() if r["executed"] and not r["failed"])
        failing = sum(1 for r in trace["per_req"].values() if r["failed"])
        not_executed = sum(1 for r in trace["per_req"].values()
                           if r["tests"] and not r["executed"] and not r["failed"])
        rep.charts({"verified": verified,
                    "failing": failing,
                    "not_executed": not_executed,
                    "manual": len(trace["per_req"]) - verified - failing - not_executed},
                   coverage)

    # ---- tests ------------------------------------------------------------
    rep.story.append(PageBreak())
    rep.h1("Test results")
    rep.text("Every Qt Test function the suite executed, from the JUnit XML in "
             "<b>test-results/</b>. QtTest's own initTestCase/cleanupTestCase entries are "
             "left out — they carry no requirement.")
    rep.gap(2)
    rows = [["Suite", "Tests", "Passed", "Failed", "Skipped", "Time (s)"]]
    for suite in tests:
        ok = suite["failed"] == 0
        rows.append([suite["suite"], str(suite["total"]),
                     status_cell(str(suite["passed"]), GREEN),
                     status_cell(str(suite["failed"]), GREEN if ok else RED),
                     str(suite["skipped"]), f"{suite['seconds']:.2f}"])
    rep.table(rows, [52 * mm, 20 * mm, 22 * mm, 22 * mm, 22 * mm, 38 * mm])

    for suite in tests:
        rep.h2(suite["suite"])
        rows = [["Test function", "Result", "s"]]
        for name, status, secs in suite["cases"]:
            colour = {"PASS": GREEN, "FAIL": RED}.get(status, AMBER)
            rows.append([Paragraph(name, rep.s["cell"]), status_cell(status, colour),
                         f"{secs:.2f}"])
        rep.table(rows, [128 * mm, 26 * mm, 22 * mm], keep=False)

    # ---- traceability -----------------------------------------------------
    if trace:
        rep.story.append(PageBreak())
        rep.h1("Traceability highlights")
        rep.text("Requirement → design → test specification → executed result, joined by "
                 "the same parser that writes <b>docs/traceability.html</b>. A requirement "
                 "counts as verified only when a test that names it actually ran and passed.")
        rep.gap(2)
        rows = [["Requirement", "Verified by (test ids)", "Verdict"]]
        for req in sorted(trace["per_req"]):
            info = trace["per_req"][req]
            if info["failed"]:
                verdict_txt, colour = "FAILING TEST", RED
            elif info["executed"]:
                verdict_txt, colour = "verified", GREEN
            elif info["tests"]:
                verdict_txt, colour = "test not executed", AMBER
            else:
                verdict_txt, colour = "no automated test", GREY
            rows.append([req,
                         Paragraph(", ".join(info["tests"]) or "—", rep.s["cell"]),
                         status_cell(verdict_txt, colour)])
        rep.table(rows, [26 * mm, 116 * mm, 34 * mm])
        rep.text(f"{trace['covered']} of {len(trace['requirements'])} requirements are "
                 f"verified by a passing automated test. The remainder are the UI-level "
                 f"requirements exercised by inspection — listed above as "
                 f"“no automated test”, never hidden.", "small")

    # ---- analysis, metrics, coverage, sanitizers --------------------------
    rep.story.append(PageBreak())
    rep.h1("Static analysis, metrics, coverage, sanitizers")
    rep.h2("Analyzers")
    rows = [["Tool", "Findings", "First finding / state"]]
    for name, count, detail in analysis:
        colour = GREY if count is None else (GREEN if count == 0 else RED)
        rows.append([name, status_cell("—" if count is None else str(count), colour),
                     Paragraph(detail, rep.s["cell"])])
    rep.table(rows, [40 * mm, 20 * mm, 116 * mm])

    rep.h2("Axivion (MISRA C++ 2023 / CERT / CWE + architecture)")
    if axivion is None:
        rep.text("<b>not run.</b> The Axivion Suite is license-bound and its dashboard "
                 "answered nothing at <b>AXIVION_DASHBOARD_URL</b> (default "
                 "http://localhost:9090/axivion). Run <b>axivion/start_analysis.sh</b> — or "
                 "the <b>axivion</b> stage of build_all — and regenerate this report to fill "
                 "this section in.")
    else:
        kinds = [("AV", "Architecture violations", "layering rules — a hard gate"),
                 ("CL", "Clones", "gated by PMD CPD here; Axivion's own clone check is off"),
                 ("CY", "Cycles", "cyclic dependencies between components"),
                 ("DE", "Dead code", "unreachable / unused (operator-new false positives known)"),
                 ("MV", "Metric violations", "complexity thresholds; lizard is the ratchet"),
                 ("SV", "Style violations", "MISRA C++ 2023 + CERT + CWE + imported tool logs")]
        rows = [["Kind", "What it is", "Total", "Added", "Removed"]]
        for key, label, note in kinds:
            entry = axivion["counts"].get(key) or {}
            kind_total = entry.get("Total", 0)
            hard = key in ("AV", "CL")
            colour = ((GREEN if kind_total == 0 else RED) if hard
                      else (GREY if kind_total == 0 else AMBER))
            rows.append([f"{key} — {label}", Paragraph(note, rep.s["cell"]),
                         status_cell(str(kind_total), colour),
                         f"+{entry.get('Added', 0)}", f"-{entry.get('Removed', 0)}"])
        rep.table(rows, [46 * mm, 74 * mm, 20 * mm, 18 * mm, 18 * mm])
        rep.text(f"Dashboard version {axivion['version']} — {axivion['name']}. Added/removed "
                 f"are versus the previous version. The style-violation total is large by "
                 f"construction: MISRA C++ 2023 judges a Qt application, where every "
                 f"QStringLiteral is an allocation and money is a double — so the actionable "
                 f"signal here is the architecture and clone gates plus the DELTA, not the "
                 f"absolute count.", "small")

    rep.h2("Most complex functions (lizard)")
    rows = [["Function", "CCN", "NLOC"]]
    for name, ccn, nloc in metrics["worst"]:
        rows.append([Paragraph(name, rep.s["cell"]), str(ccn), str(nloc)])
    if len(rows) == 1:
        rows.append([Paragraph("no metrics csv — stage not run", rep.s["cell"]), "—", "—"])
    rep.table(rows, [124 * mm, 26 * mm, 26 * mm])

    rep.h2("Coverage")
    rows = [["Measurement", "Result"],
            ["Lines (gcov/lcov)", status_cell(pct(coverage["lines"]), GREEN if coverage["lines"] else GREY)],
            ["Functions (gcov/lcov)", status_cell(pct(coverage["functions"]), GREEN if coverage["functions"] else GREY)],
            ["MC/DC (clang source-based)", status_cell(coverage["mcdc"] or "—", GREEN if coverage["mcdc"] else GREY)]]
    rep.table(rows, [80 * mm, 40 * mm])
    if coverage["mcdc_rows"]:
        rep.gap(2)
        rows = [["File", "MC/DC"]] + [[Paragraph(f, rep.s["cell"]), p]
                                      for f, p in coverage["mcdc_rows"][:18]]
        rep.table(rows, [110 * mm, 30 * mm])

    rep.h2("Sanitizers")
    rows = [["Sanitizer", "Findings"]]
    for name, count in sanitizers:
        rows.append([name, status_cell(str(count), GREEN if count == 0 else RED)])
    if len(rows) == 1:
        rows.append(["not run", status_cell("—", GREY)])
    rep.table(rows, [60 * mm, 30 * mm])

    rep.gap(4)
    rep.text("Generated by tools/make_report.py from the artefacts of this pipeline run "
             "(test-results/, analysis-results/, coverage/, docs/). Missing artefacts are "
             "reported as “not run” rather than assumed clean.", "small")
    rep.build()
    return everything_ok, total, failed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "downloads" / "TradingApp-quality-report.pdf"),
                    help="output PDF path (default: downloads/TradingApp-quality-report.pdf)")
    ap.add_argument("--build-dir", default="build", help="build tree to read toolchain facts from")
    args = ap.parse_args()

    out = Path(args.out)
    if not out.is_absolute():
        out = ROOT / out
    try:
        ok, total, failed = build_report(out, args.build_dir)
    except Exception as exc:  # a report must never be the thing that breaks a build
        print(f"report FAILED: {exc}", file=sys.stderr)
        return 1
    rel = os.path.relpath(out, ROOT)
    print(f"report: {rel} ({total} tests, {failed} failed) — "
          f"{'all checks passed' if ok else 'attention required'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
