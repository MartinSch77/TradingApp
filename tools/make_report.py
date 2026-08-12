#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

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
import configparser
import csv
import glob
import json
import os
import shutil
import platform
import re
import subprocess
import sys
import urllib.error
import urllib.parse
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
        ("qmllint", "qmllint.txt"),
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
    """(lines%, functions%) from lcov's tracefile, the MC/DC totals, and Coco's four.

    THREE back ends, deliberately: gcov gives the line/branch figure the badge is built
    from, clang instruments the IR for an independent MC/DC number, and Squish Coco
    instruments the SOURCE for statement/decision/condition/MC/DC from a qualified
    tool. A report that showed only whichever one ran last would hide the cross-check.
    """
    # coco = the unit/integration suites; coco_gui = the Squish GUI suite. Two separate
    # figures on purpose (tools/coverage.sh coco vs coco-gui): one blended percentage would
    # answer neither "are the domain decisions exercised" nor "does a user driving the real
    # window reach this code", and blending lets a well-covered domain hide an untouched UI.
    result = {"lines": None, "functions": None, "mcdc": None, "mcdc_rows": [],
              "coco": {}, "coco_unit": {}, "coco_integration": {}, "coco_gui": {}}
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
    # Coco writes its four levels as JSON precisely so this report can quote them
    # (tools/coverage.sh coco_stat_levels). Absent = Coco did not run here, which is a
    # licence question and is reported as one further down.
    for key, folder in (("coco", "coco"), ("coco_unit", "coco-unit"),
                        ("coco_integration", "coco-integration"), ("coco_gui", "coco-gui")):
        summary = ROOT / "coverage" / folder / "summary.json"
        if not summary.is_file():
            continue
        try:
            result[key] = json.loads(summary.read_text(**UTF8, errors="replace"))
        except (ValueError, OSError):
            result[key] = {}
    return result


class _Redirected(Exception):
    """A redirect, carried out of the opener so the CALLER can read the destination.

    Test Center answers 302 -> /activation/index until it has been activated, and that is
    the one piece of information worth reporting: following the redirect would land on a
    perfectly healthy login page and hide the fact that nothing can be uploaded yet.
    """

    def __init__(self, location):
        super().__init__(location)
        self.location = location or ""


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise _Redirected(newurl)


def _testcentercmd_has_token(url):
    """True when testcentercmd's OWN credential store holds a token for this server.

    The recommended way to authenticate is `testcentercmd config token <value>`, which
    keeps the secret out of the environment entirely. Judging credentials by environment
    variables alone therefore reports "no credentials were given" on a machine where
    uploads demonstrably work — so the store is consulted too, or this report drifts away
    from the practice its own documentation recommends.

    Only the value's PRESENCE is read, never the value. The Linux path is measured; the
    Windows candidates are included because the same client runs there and a false
    negative here only costs a slightly pessimistic line in the report.
    """
    host = urllib.parse.urlsplit(url).netloc or "localhost:8800"
    candidates = [Path.home() / ".squish" / "ver1" / "testcentercmd.ini"]
    if appdata := os.environ.get("APPDATA"):
        candidates.append(Path(appdata) / "froglogic" / "Squish" / "ver1"
                          / "testcentercmd.ini")
    for path in candidates:
        try:
            parser = configparser.ConfigParser()
            parser.read(path, encoding="utf-8")
        except (configparser.Error, OSError, UnicodeDecodeError):
            continue
        # Sections are named `host:port`; an empty section is what `config remove` leaves
        # behind, so a section existing is NOT the same as a token existing.
        if parser.has_option(host, "token") and parser.get(host, "token").strip(' "'):
            return True
    return False


def _runs_ok(argv):
    """True when the command exists and exits 0 — a licence check, not a version print."""
    try:
        return subprocess.run(argv, capture_output=True, timeout=20).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def licensed_tools():
    """(name, available, detail) for every tool this project can use but not ship.

    None of them gates a build: each stage exits 3 ("skipped") when its licence is
    absent, and this table is how the final report says so — the alternative is a PDF
    that looks complete while three measurements silently did not happen.
    """
    out = []

    # The SAME two-part check tools/coverage.sh makes: the wrapper has to exist AND
    # the licence has to be valid. Reporting "licensed" because a binary is on disk is
    # how a PDF ends up claiming a measurement that never ran.
    coco_dir = Path(os.environ.get("COCO_DIR", "/opt/SquishCoco"))
    coco_installed = (coco_dir / "bin" / "csg++").exists()
    coco = coco_installed and _runs_ok([str(coco_dir / "bin" / "cocolic"), "--check"])
    if coco:
        coco_detail = (f"licensed at {coco_dir} — tools/coverage.sh coco measures MC/DC, "
                       "and coco-components the per-test call coverage of the integrated "
                       "components")
    elif coco_installed:
        coco_detail = (f"installed at {coco_dir} but the licence check fails "
                       f"({coco_dir}/bin/cocolic --check) — only the licence is missing; "
                       "gcov and clang MC/DC measured the coverage above")
    else:
        coco_detail = ("not installed here — gcov and clang MC/DC measured the coverage "
                       "above (tools/coverage.sh coco is the licensed path)")
    out.append(("Squish Coco", coco, coco_detail))

    # The SAME search order tools/squish_run.sh uses, and for a measured reason: the
    # official installer puts Squish in ~/squish-for-qt-<version> and does NOT put it on
    # PATH, so a probe limited to PATH and /opt/squish reports "not installed" on a machine
    # that is fully licensed — and the PDF then lists a missing licence that is not missing.
    squish_candidates = []
    configured = os.environ.get("SQUISH_DIR") or os.environ.get("SQUISH_PREFIX")
    if configured:
        squish_candidates.append(Path(configured))
    squish_candidates.append(Path("/opt/squish"))
    squish_candidates.extend(sorted(Path.home().glob("squish-for-qt-*"), reverse=True))
    squish_candidates.extend(sorted(Path("/opt").glob("squish*"), reverse=True))
    squish_dir = next((d for d in squish_candidates if (d / "bin" / "squishrunner").exists()),
                      Path(configured) if configured else Path("/opt/squish"))
    squish = bool(shutil.which("squishrunner")) or (squish_dir / "bin" / "squishrunner").exists()
    out.append(("Squish (GUI tests)", squish,
                "the GUI suite in squish/suite_gui runs the app FORCED into simulation "
                "(TRADINGAPP_FORCE_SIMULATION) so it can never reach a real account. Not "
                "installed here — tools/squish_run.sh skipped"
                if not squish else
                f"GUI suite available at {squish_dir}; every run is forced into simulation "
                "and can never reach a real account"))

    # Test Center is reported by its ACTUAL state, not merely by whether a variable is
    # set: "no token", "server down" and "server up but never activated" are three
    # different situations, and a reader who cannot tell them apart cannot act on any of
    # them. Credentials may be a token OR an email/password pair — the product accepts both.
    tc_url = os.environ.get("TESTCENTER_URL", "http://localhost:8800")
    have_creds = bool(os.environ.get("TESTCENTER_TOKEN")) or bool(
        os.environ.get("TESTCENTER_USER") and os.environ.get("TESTCENTER_PASSWORD")) or (
        _testcentercmd_has_token(tc_url))
    state = "unreachable"
    try:
        request = urllib.request.Request(tc_url, method="GET")
        opener = urllib.request.build_opener(_NoRedirect())
        with opener.open(request, timeout=4) as response:
            state = "activated" if response.status < 300 else "reachable"
    except _Redirected as redirect:
        state = "not activated" if "/activation" in redirect.location else "activated"
    except (urllib.error.URLError, OSError, ValueError):
        state = "unreachable"

    tc = have_creds and (state == "activated")
    if tc:
        detail = f"uploading to {tc_url}"
    elif state == "not activated":
        detail = (f"server is running at {tc_url} but has NEVER BEEN ACTIVATED (it "
                  "redirects to /activation) — the licence and the first user are entered "
                  "in a browser, which cannot be scripted. Nothing was uploaded; the "
                  "JUnit XML is complete in test-results/ and can be uploaded later")
    elif state == "unreachable":
        detail = (f"no server answering at {tc_url} — start it with "
                  "<b>bin/testcenter start</b>. tools/testcenter_upload.sh skipped; the "
                  "results stay in test-results/")
    else:
        detail = ("server is up but no credential was found — store one once with "
                  "<b>testcentercmd config token &lt;value&gt;</b> (Admin -> User Management "
                  "-> upload token), or set TESTCENTER_TOKEN / TESTCENTER_USER + "
                  "TESTCENTER_PASSWORD. tools/testcenter_upload.sh skipped")
    out.append(("Qt Test Center", tc, detail))
    return out


def collect_external_import():
    """What the other analyzers contributed TO the Axivion dashboard, per provider.

    analysis-results/external_findings.csv is the merged log (tools/merge_findings.py) that
    axivion/external_import.py re-emits as Axivion style violations, one provider per tool.
    Without this the report shows "eight analyzers: 0" and "Axivion: 154,758" as two
    unrelated facts, when in truth the first feed the second — so a reader cannot tell
    whether an analyzer's output reached the dashboard at all.

    Returns (rows, providers_wired) where rows is [(provider, count)] for what actually
    arrived. A provider that is wired but contributed nothing is NOT an error: with every
    gated analyzer at zero there is correctly nothing to import.
    """
    path = ROOT / "analysis-results" / "external_findings.csv"
    counts = {}
    if path.is_file():
        try:
            with path.open(newline="", **UTF8) as handle:
                for row in csv.reader(handle, delimiter=";"):
                    if not row or row[0] == "tool":
                        continue
                    counts[row[0]] = counts.get(row[0], 0) + 1
        except (OSError, csv.Error):
            return [], []
    wired = []
    importer = ROOT / "axivion" / "external_import.py"
    if importer.is_file():
        try:
            wired = sorted(set(re.findall(r"^\s*'([a-z0-9-]+)':\s*\('",
                                          importer.read_text(**UTF8), re.M)))
        except OSError:
            wired = []
    return sorted(counts.items(), key=lambda kv: -kv[1]), wired


def collect_tool_versions():
    """analysis-results/tool-versions.json, or [] when the analysis stage never wrote it.

    Written by tools/record_tool_versions.py. Absent on a tree where only some stages ran,
    which is why the report treats it as optional rather than required.
    """
    path = ROOT / "analysis-results" / "tool-versions.json"
    if not path.is_file():
        return []
    try:
        return json.loads(path.read_text(**UTF8)).get("tools") or []
    except (json.JSONDecodeError, OSError, AttributeError):
        return []


def _artefact_verdict(artefact: str, absent: bool):
    """(text, colour) for a tool's output file — its finding count, or why there is none.

    Three states, deliberately distinct: the tool did not run, the artefact is missing, or
    the artefact exists and has a count. Collapsing the first two into "0" is the failure
    this whole report is built to avoid.
    """
    if absent:
        return "not run", GREY
    if not artefact:
        return "—", GREY
    path = ROOT / artefact
    if not path.is_file():
        return "no artefact", GREY
    try:
        findings = len([ln for ln in path.read_text(**UTF8, errors="replace").splitlines()
                        if ln.strip()])
    except OSError:
        return "unreadable", GREY
    # object-names.txt carries a human-readable success sentence rather than findings, the
    # same special case tools/gates_to_junit.py handles — counting its lines would report a
    # clean check as one finding.
    if path.name == "object-names.txt":
        findings = len([ln for ln in path.read_text(**UTF8, errors="replace").splitlines()
                        if "has no objectName" in ln])
    return (f"{findings} findings" if findings else "0 — pass"), (RED if findings else GREEN)


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
            "name": version.get("name", ""), "counts": counts,
            "metrics": _axivion_system_metrics(base, token, project)}


# The ABSOLUTE metric values, not just the violation counts. Axivion measures lines of
# code, McCabe complexity, statement counts and comment lines and stores them against the
# "System Entity" — the one entity per project that subsumes all others — but none of that
# reached this report before, which showed verdicts without the sizes they were measured
# over. A reader cannot judge "154,758 style violations" without knowing it is 21,292 lines.
#
# The three-step API is the documented one (Metrics API): getSystemEntity to find the
# entity, getMetrics to enumerate what is stored against it, then queryMetricValueRange per
# metric. Note the Suite only stores metrics for entities it kept, so an empty list here
# means "not configured", never "zero" — and every failure path returns {} so a dashboard
# that answers nothing degrades to "not run" rather than to a page of zeros.
def _axivion_system_metrics(base: str, token: str, project: str) -> dict:
    def get(path: str):
        req = urllib.request.Request(f"{base}{path}",
                                     headers={"Authorization": f"Basic {token}"})
        with urllib.request.urlopen(req, timeout=8) as response:
            return json.loads(response.read().decode("utf-8", "replace"))

    try:
        entities = get(f"/api/projects/{project}/getSystemEntity").get("entities") or []
        if not entities:
            return {}
        entity = urllib.parse.quote(str(entities[0]["id"]))
        listed = get(f"/api/projects/{project}/getMetrics?entity={entity}").get("metrics") or []
    except (urllib.error.URLError, OSError, json.JSONDecodeError, ValueError, KeyError):
        return {}

    out = {}
    for metric in listed:
        name = metric.get("name")
        if not name:
            continue
        try:
            payload = get(f"/api/projects/{project}/queryMetricValueRange"
                          f"?start=latest&end=latest&entity={entity}"
                          f"&metric={urllib.parse.quote(name)}")
            values = payload.get("values") or []
        except (urllib.error.URLError, OSError, json.JSONDecodeError, ValueError):
            continue
        # A metric that exists but has no value in this version is None, and stays None:
        # printing 0 for it would be a measurement this analysis never made.
        out[name] = {"value": values[-1] if values else None,
                     "label": metric.get("displayName") or name}
    return out


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
        # The area is a Paragraph, NOT a bare string. reportlab does not wrap a plain string
        # in a table cell: it draws it at full width and lets it run over the neighbouring
        # column, which is why "Coverage (Coco — unit + integration)" printed on top of its
        # own status cell. Only the evidence column was wrapped, so only the labels collided.
        rows.append([Paragraph(area, rep.s["cell"]), status_cell(result, verdict_colour(ok, warn)),
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
               f" · MC/DC (clang) {coverage['mcdc'] or '—'}")
    add("Coverage", coverage["lines"] is not None,
        "measured" if coverage["lines"] is not None else "not run", cov_txt,
        warn=coverage["lines"] is None)
    # Coco as its own row: the numbers, not merely the fact that a licence exists.
    def _levels(source):
        def _lvl(name):
            data = source.get(name) or {}
            if not data:
                return f"{name} —"
            return (f"{name} {data.get('percent', 0):.1f}% "
                    f"({data.get('covered', 0)}/{data.get('total', 0)})")
        return " · ".join(_lvl(n) for n in ("statement", "decision", "condition", "mcdc"))

    # THREE Coco figures, never one. A blended number hides what is worth knowing: unit
    # tests cover pure domain logic and can be near-total, integration tests drive the
    # services against a mock server and are necessarily thinner, and the GUI suite reaches
    # a different layer again. Averaged together, a strong domain figure conceals a weak
    # services one — so each is reported on its own row, with its own source named.
    for key, label, how in (
            ("coco_unit", "Coverage (Coco — unit tests)",
             "tools/coverage.sh coco → coverage/coco-unit; the domain layer, tested pure"),
            ("coco_integration", "Coverage (Coco — integration tests)",
             "tools/coverage.sh coco → coverage/coco-integration; the services layer driven "
             "against the in-process mock server"),
            ("coco_gui", "Coverage (Coco — GUI tests)",
             "tools/coverage.sh coco-gui → coverage/coco-gui; instruments src/ui and measures "
             "what the Squish workflows actually reach")):
        data = coverage.get(key) or {}
        if data:
            add(label, True, "measured", _levels(data))
        else:
            add(label, False, "not run", f"not measured here — {how}", warn=True)
    # The combined figure stays too, LAST and clearly marked, because the three above are the
    # ones to read and a total is only useful for comparing whole runs.
    coco = coverage.get("coco") or {}
    if coco:
        add("Coverage (Coco — all suites combined)", True, "measured", _levels(coco))
    add("Sanitizers", san_findings == 0,
        "CLEAN" if san_findings == 0 else f"{san_findings} FINDINGS",
        " · ".join(f"{name}: {n}" for name, n in sanitizers) or "not run")
    # Licence-bound tools are never a gate here: they run when a licence is present
    # and skip cleanly otherwise (exit code 3). The report is where that shows, so a
    # reader knows the difference between "clean" and "not measured on this machine".
    for name, available, detail in licensed_tools():
        add(name, available, "licensed" if available else "no licence here", detail,
            warn=not available)
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

    # WHICH tool produced each verdict above, at what version, invoked how, writing where.
    # The table above names counts; this one makes them traceable. Without it the bundle
    # could report "cppcheck: 0" and be unable to say which cppcheck that was — the
    # versions used to live only in the console log.
    tools = collect_tool_versions()
    if tools:
        rep.h2("Tool reference — version, invocation, evidence")
        rows = [["Tool + version", "Invocation", "Output artefact", "Result"]]
        for entry in tools:
            version = entry.get("version") or "unknown"
            artefact = entry.get("artefact", "")
            absent = version == "not installed"
            values, colour = _artefact_verdict(artefact, absent)
            rows.append([Paragraph(f"<b>{entry.get('tool','?')}</b><br/>{version}",
                                   rep.s["cell"]),
                         Paragraph(entry.get("invocation", ""), rep.s["cell"]),
                         Paragraph(artefact, rep.s["cell"]),
                         status_cell(values, colour)])
        rep.table(rows, [40 * mm, 60 * mm, 48 * mm, 28 * mm])
        rep.text("Recorded by tools/record_tool_versions.py into "
                 "analysis-results/tool-versions.json during the analysis stage, so this "
                 "table is reconstructible from the artefacts alone rather than from the "
                 "console. \"not installed\" is reported as its own state: a check that did "
                 "not run is not a check that found nothing.", "small")

    # Every analyzer above ALSO lands on the Axivion dashboard, one provider per tool, so
    # the whole toolchain is triageable in one place. Stated explicitly because otherwise
    # "eight analyzers: 0" and "Axivion: 154,758 style violations" read as two unrelated
    # facts rather than one pipeline feeding the other.
    imported, wired = collect_external_import()
    if wired:
        rep.h2("Other analyzers imported into the Axivion dashboard")
        if imported:
            rows = [["Provider", "Findings imported", "Arrives as"]]
            for provider, count in imported:
                rows.append([provider, status_cell(str(count), AMBER),
                             Paragraph("Axivion style violation, provider "
                                       f"<b>{provider}</b>", rep.s["cell"])])
            rep.table(rows, [40 * mm, 32 * mm, 104 * mm])
        else:
            rep.text("<b>Nothing to import.</b> Every gated analyzer reported zero, so the "
                     "merged log is empty — which is the intended state, not a broken "
                     "import.", "small")
        silent = [w for w in wired if w not in dict(imported)]
        rep.text(f"axivion/external_import.py wires {len(wired)} providers via the Suite's "
                 f"ImportExternalAnalysisOutput mechanism: {', '.join(wired)}. "
                 f"{len(imported)} contributed on this run; the other {len(silent)} are "
                 f"silent because their analyzer found nothing, is not installed on this "
                 f"host, or needs a separate export step (sonarqube via tools/sonar_scan, "
                 f"coverity via tools/coverity_findings.py). A silent provider is not a "
                 f"failed import — but it is also not evidence, which is why the tool table "
                 f"above states each one's own result.", "small")

    rep.h2("Axivion (MISRA C++ 2023 / CERT / CWE + architecture)")
    if axivion is None:
        rep.text("<b>not run.</b> The Axivion Suite is license-bound and its dashboard "
                 "answered nothing at <b>AXIVION_DASHBOARD_URL</b> (default "
                 "http://localhost:9090/axivion). Run <b>axivion/start_analysis.sh</b> — or "
                 "the <b>axivion</b> stage of build_all — and regenerate this report to fill "
                 "this section in.")
    else:
        # The CL note used to read "Axivion's own clone check is off". That was FALSE —
        # C++CloneDetection is _active in axivion/rule_config.json and finds clones — and
        # the row was additionally coloured as a hard gate, so a passing build showed red
        # while the note claimed the check was not running. Two clone detectors run here at
        # different thresholds: PMD CPD (>= 100 tokens) is the GATE and must be 0; Axivion's
        # is informational and reported as such.
        kinds = [("AV", "Architecture violations", "layering rules — a hard gate"),
                 ("CL", "Clones",
                  "Axivion's own clone check IS on (C++CloneDetection); the GATE is PMD CPD "
                  "at &gt;= 100 tokens, which is separate and must be 0 — these are "
                  "informational at Axivion's own threshold"),
                 ("CY", "Cycles", "cyclic dependencies between components — informational"),
                 ("DE", "Dead code", "unreachable / unused (operator-new false positives known)"),
                 ("MV", "Metric violations", "complexity thresholds; lizard is the ratchet"),
                 ("SV", "Style violations", "MISRA C++ 2023 + CERT + CWE + imported tool logs")]
        rows = [["Kind", "What it is", "Total", "Added", "Removed"]]
        for key, label, note in kinds:
            entry = axivion["counts"].get(key) or {}
            kind_total = entry.get("Total", 0)
            # AV alone is the hard gate. CL is NOT: the clone gate is PMD CPD, reported in
            # the analyzer table above, and colouring Axivion's own clone count red would
            # fail a build on a threshold this project does not gate on.
            hard = key == "AV"
            colour = ((GREEN if kind_total == 0 else RED) if hard
                      else (GREY if kind_total == 0 else AMBER))
            rows.append([f"{key} — {label}", Paragraph(note, rep.s["cell"]),
                         status_cell(str(kind_total), colour),
                         f"+{entry.get('Added', 0)}", f"-{entry.get('Removed', 0)}"])
        rep.table(rows, [46 * mm, 74 * mm, 20 * mm, 18 * mm, 18 * mm])

        # The ABSOLUTE measurements behind those verdicts. Without them a reader cannot
        # judge any of the counts above: 154,758 style violations over 21,292 lines is a
        # different statement from the same number over ten million.
        # NOT named `metrics`: that name already holds the lizard data this function uses
        # further down (metrics["worst"]), and shadowing it made the report die with
        # KeyError 'worst' — the same class of defect cppcheck's shadowFunction check
        # catches in the C++.
        ax_metrics = axivion.get("metrics") or {}
        if ax_metrics:
            rep.h2("Axivion absolute metrics (System Entity, latest version)")
            groups = [
                ("Size", [("Metric.Lines.LOC.sum", "lines of code (total)"),
                          ("Metric.Lines.LOC.cnt", "entities measured"),
                          ("Metric.Lines.LOC.avg", "lines per entity (average)"),
                          ("Metric.Lines.LOC.max", "largest entity (lines)"),
                          ("Metric.Lines.Comment.sum", "comment lines (total)"),
                          ("Metric.Number_Of_Statements.sum", "statements (total)"),
                          ("Metric.Number_Of_Statements.max", "most statements in one entity")]),
                ("Complexity", [("Metric.McCabe_Complexity.sum", "McCabe complexity (total)"),
                                ("Metric.McCabe_Complexity.avg", "McCabe per routine (average)"),
                                ("Metric.McCabe_Complexity.max", "worst routine (McCabe)"),
                                ("Metric.McCabe_Complexity.cnt", "routines measured")]),
                ("Findings as metrics", [("Metric.Violations.Style", "style violations"),
                                         ("Metric.Violations.Clone", "clones"),
                                         ("Metric.Violations.Cycle", "cyclic dependencies"),
                                         ("Metric.Violations.Dead_Entity", "dead entities"),
                                         ("Metric.Violations.Metric", "metric violations"),
                                         ("Metric.Violations.Architecture",
                                          "architecture violations")]),
            ]
            rows = [["Group", "Metric", "Value"]]
            shown = set()
            for group, items in groups:
                for key, label in items:
                    entry = ax_metrics.get(key)
                    if entry is None:
                        continue
                    shown.add(key)
                    value = entry["value"]
                    # None means the metric exists but this version stored no value. It is
                    # printed as "not measured", never as 0 — a 0 here would assert a
                    # measurement that was not taken. Metric.Violations.Architecture is the
                    # real case: the AV COUNT above is the evidence, not this row.
                    if value is None:
                        cell = status_cell("not measured", GREY)
                    elif float(value).is_integer():
                        cell = f"{int(value):,}"
                    else:
                        cell = f"{float(value):,.2f}"
                    rows.append([group, Paragraph(label, rep.s["cell"]), cell])
            rep.table(rows, [30 * mm, 96 * mm, 50 * mm])
            rep.text(f"{len(ax_metrics)} metrics are stored against the System Entity; the "
                     f"{len(shown)} above are the ones worth a verdict. Axivion counts "
                     f"'lines of code' as empty + comment + code, so it does not match "
                     f"gcov's line total, and its McCabe figures are its own — lizard's "
                     f"ratchet above is a separate measurement with separate thresholds. "
                     f"Two numbers that disagree here are two different definitions, not a "
                     f"defect.", "small")
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
