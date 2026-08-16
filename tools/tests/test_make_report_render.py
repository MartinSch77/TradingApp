# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for tools/make_report.py's licence-detection, rendering and CLI.

licensed_tools() is exercised branch-by-branch with everything that reaches
outside the process (subprocess, the filesystem, urllib) mocked or redirected
into tmp_path, so these tests never depend on Squish/Coco actually being
installed or a Test Center server actually running.

The Report class and build_report() are reportlab CONSUMERS: per the task
brief, these are smoke-tested (no exception, a PDF file appears) rather than
asserting on rendered pixels — the interesting branches (verdict colour,
"not measured" vs a real bar chart, warn rows) are still forced explicitly so
the branches in Report.charts / build_report's row-adding logic are covered.
"""

import json
import subprocess
import urllib.error

import pytest

import make_report as mr


# ---------------------------------------------------------------------------
# licensed_tools()
# ---------------------------------------------------------------------------
class _FakeResponse:
    def __init__(self, status=200):
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _FakeOpener:
    def __init__(self, behavior):
        self._behavior = behavior

    def open(self, request, timeout=None):
        result = self._behavior()
        if isinstance(result, Exception):
            raise result
        return result


def _patch_common_licensed_tools(monkeypatch, tmp_path, *, coco_installed=False,
                                  coco_licensed=False, squish_found=False,
                                  opener_behavior=None):
    # This machine has a REAL Squish/Coco install (~/squish-for-qt-*, /opt/SquishCoco);
    # without redirecting Path.home() the squish_dir glob would find it and every
    # "not installed" assertion below would fail on this host but pass on a clean CI
    # box — exactly the kind of environment-dependent flake this suite must avoid.
    monkeypatch.setattr(mr.Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.setenv("COCO_DIR", str(tmp_path / "coco"))
    if coco_installed:
        coco_bin = tmp_path / "coco" / "bin"
        coco_bin.mkdir(parents=True)
        (coco_bin / "csg++").write_text("", encoding="utf-8")
        (coco_bin / "cocolic").write_text("", encoding="utf-8")
    monkeypatch.setattr(mr, "_runs_ok", lambda argv: coco_licensed)

    monkeypatch.setattr(mr.shutil, "which", lambda name: (
        str(tmp_path / "squishrunner") if squish_found and name == "squishrunner" else None))
    # Keep the real filesystem probe honest but harmless: point SQUISH_DIR at an
    # empty directory unless the test wants a discoverable install.
    monkeypatch.setenv("SQUISH_DIR", str(tmp_path / "no-squish-here"))

    if opener_behavior is not None:
        monkeypatch.setattr(mr.urllib.request, "build_opener",
                            lambda *_: _FakeOpener(opener_behavior))


def test_licensed_tools_all_absent(tmp_path, monkeypatch):
    monkeypatch.delenv("TESTCENTER_TOKEN", raising=False)
    monkeypatch.delenv("TESTCENTER_USER", raising=False)
    monkeypatch.delenv("TESTCENTER_PASSWORD", raising=False)
    monkeypatch.setattr(mr, "_testcentercmd_has_token", lambda url: False)
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, coco_installed=False, squish_found=False,
        opener_behavior=lambda: urllib.error.URLError("refused"))

    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Squish Coco"][0] is False
    assert "not installed" in out["Squish Coco"][1]
    assert out["Squish (GUI tests)"][0] is False
    assert out["Qt Test Center"][0] is False
    assert "no server answering" in out["Qt Test Center"][1]


def test_licensed_tools_coco_installed_but_unlicensed(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "_testcentercmd_has_token", lambda url: False)
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, coco_installed=True, coco_licensed=False,
        opener_behavior=lambda: urllib.error.URLError("refused"))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Squish Coco"][0] is False
    assert "licence check fails" in out["Squish Coco"][1]


def test_licensed_tools_coco_fully_licensed(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "_testcentercmd_has_token", lambda url: False)
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, coco_installed=True, coco_licensed=True,
        opener_behavior=lambda: urllib.error.URLError("refused"))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Squish Coco"][0] is True
    assert "licensed at" in out["Squish Coco"][1]


def test_licensed_tools_squish_dir_unconfigured(tmp_path, monkeypatch):
    """Neither SQUISH_DIR nor SQUISH_PREFIX set: the `if configured:` guard in
    licensed_tools' candidate list stays False (its only other exercised branch,
    via _patch_common_licensed_tools, always sets SQUISH_DIR)."""
    monkeypatch.setattr(mr, "_testcentercmd_has_token", lambda url: False)
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, opener_behavior=lambda: urllib.error.URLError("refused"))
    monkeypatch.delenv("SQUISH_DIR", raising=False)
    monkeypatch.delenv("SQUISH_PREFIX", raising=False)
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Squish (GUI tests)"][0] is False


def test_licensed_tools_squish_found_via_which(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "_testcentercmd_has_token", lambda url: False)
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, squish_found=True,
        opener_behavior=lambda: urllib.error.URLError("refused"))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Squish (GUI tests)"][0] is True
    assert "forced into simulation" in out["Squish (GUI tests)"][1]


def test_licensed_tools_testcenter_activated_with_token(tmp_path, monkeypatch):
    monkeypatch.setenv("TESTCENTER_TOKEN", "sekrit")
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, opener_behavior=lambda: _FakeResponse(200))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Qt Test Center"][0] is True
    assert "uploading to" in out["Qt Test Center"][1]


def test_licensed_tools_testcenter_not_activated(tmp_path, monkeypatch):
    monkeypatch.setenv("TESTCENTER_TOKEN", "sekrit")
    _patch_common_licensed_tools(
        monkeypatch, tmp_path,
        opener_behavior=lambda: mr._Redirected("http://localhost:8800/activation/index"))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Qt Test Center"][0] is False
    assert "NEVER BEEN ACTIVATED" in out["Qt Test Center"][1]


def test_licensed_tools_testcenter_redirect_elsewhere_counts_as_activated(tmp_path, monkeypatch):
    monkeypatch.setenv("TESTCENTER_TOKEN", "sekrit")
    _patch_common_licensed_tools(
        monkeypatch, tmp_path,
        opener_behavior=lambda: mr._Redirected("http://localhost:8800/dashboard"))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    # state becomes "activated" (redirect target has no /activation) but have_creds
    # is True so this is the "uploading to" branch.
    assert out["Qt Test Center"][0] is True


def test_licensed_tools_testcenter_reachable_no_credentials(tmp_path, monkeypatch):
    monkeypatch.delenv("TESTCENTER_TOKEN", raising=False)
    monkeypatch.delenv("TESTCENTER_USER", raising=False)
    monkeypatch.delenv("TESTCENTER_PASSWORD", raising=False)
    monkeypatch.setattr(mr, "_testcentercmd_has_token", lambda url: False)
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, opener_behavior=lambda: _FakeResponse(302))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Qt Test Center"][0] is False
    assert "no credential was found" in out["Qt Test Center"][1]


def test_licensed_tools_testcenter_user_password_pair_counts_as_creds(tmp_path, monkeypatch):
    monkeypatch.setenv("TESTCENTER_USER", "me")
    monkeypatch.setenv("TESTCENTER_PASSWORD", "pw")
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, opener_behavior=lambda: _FakeResponse(200))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Qt Test Center"][0] is True


def test_licensed_tools_testcenter_unreachable_on_value_error(tmp_path, monkeypatch):
    monkeypatch.setenv("TESTCENTER_TOKEN", "sekrit")
    _patch_common_licensed_tools(
        monkeypatch, tmp_path, opener_behavior=lambda: ValueError("bad url"))
    out = dict((name, (ok, detail)) for name, ok, detail in mr.licensed_tools())
    assert out["Qt Test Center"][0] is False
    assert "no server answering" in out["Qt Test Center"][1]


# ---------------------------------------------------------------------------
# Report — smoke tests over the reportlab consumer surface
# ---------------------------------------------------------------------------
def test_report_basic_story_and_build(tmp_path):
    rep = mr.Report(tmp_path / "out.pdf", "Test report")
    rep.h1("Title")
    rep.h2("Section")
    rep.text("Some body text.")
    rep.gap()
    rep.table([["A", "B"], ["1", "2"]], [40, 40])
    rep.hero("ALL CHECKS PASSED", True, "subtitle here", {"Revision": "abc123"})
    rep.build()
    assert (tmp_path / "out.pdf").is_file()
    assert (tmp_path / "out.pdf").stat().st_size > 0


def test_report_hero_failing_verdict(tmp_path):
    rep = mr.Report(tmp_path / "out2.pdf", "Test report")
    rep.hero("ATTENTION REQUIRED", False, "subtitle", {"k": "v"})
    rep.build()
    assert (tmp_path / "out2.pdf").is_file()


def test_report_charts_not_measured_branch(tmp_path):
    rep = mr.Report(tmp_path / "out3.pdf", "Test report")
    coverage = {"lines": None, "functions": None, "mcdc": None}
    rep.charts({"verified": 0, "not_executed": 0, "manual": 0, "failing": 0}, coverage)
    rep.build()
    assert (tmp_path / "out3.pdf").is_file()


def test_report_charts_with_values(tmp_path):
    rep = mr.Report(tmp_path / "out4.pdf", "Test report")
    coverage = {"lines": 87.3, "functions": 92.1, "mcdc": "55.0%"}
    rep.charts({"verified": 10, "not_executed": 2, "manual": 1, "failing": 3}, coverage)
    rep.build()
    assert (tmp_path / "out4.pdf").is_file()


def test_report_charts_all_zero_pie_slices_still_renders(tmp_path):
    # every req_counts value is 0 -> `shown` list is empty -> the "or [1]"/"or ['']"
    # fallbacks in Report.charts are exercised.
    rep = mr.Report(tmp_path / "out5.pdf", "Test report")
    coverage = {"lines": 0.0, "functions": 0.0, "mcdc": None}
    rep.charts({"verified": 0, "not_executed": 0, "manual": 0, "failing": 0}, coverage)
    rep.build()
    assert (tmp_path / "out5.pdf").is_file()


def test_status_cell_contains_colour_and_text():
    para = mr.status_cell("PASS", mr.GREEN)
    assert "PASS" in para.text
    assert mr.GREEN.hexval().lower() in para.text.lower()


# ---------------------------------------------------------------------------
# build_report — full integration over a synthetic artefact tree
# ---------------------------------------------------------------------------
def _seed_minimal_tree(tmp_path):
    """A tree with enough artefacts to make build_report's happy path run,
    without ever touching the real repository's own results."""
    (tmp_path / "test-results").mkdir()
    (tmp_path / "test-results" / "tst_Foo.xml").write_text(
        '<testsuite name="tst_Foo" time="0.5">'
        '<testcase name="testOne" time="0.5"/>'
        '</testsuite>', encoding="utf-8")
    (tmp_path / "analysis-results").mkdir()
    (tmp_path / "tools").mkdir()
    build = tmp_path / "build"
    build.mkdir()


@pytest.fixture
def isolated_root(tmp_path, monkeypatch):
    """Redirects make_report.ROOT to tmp_path and neutralises every collector
    that reaches outside the process (git, network), so build_report()
    becomes a pure function of the synthetic tree."""
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    monkeypatch.setattr(mr, "git_info", lambda: {
        "commit": "deadbee", "subject": "synthetic commit",
        "branch": "main", "dirty": 0})
    monkeypatch.setattr(mr, "collect_axivion", lambda: None)
    monkeypatch.setattr(mr, "licensed_tools", lambda: [
        ("Squish Coco", False, "not installed here"),
        ("Squish (GUI tests)", False, "not installed here"),
        ("Qt Test Center", False, "no server answering"),
    ])
    # trace_report import succeeds against the real repo's docs/tests trees
    # (relative to trace_report's OWN root, unaffected by ROOT here), which is
    # fine for this integration smoke test: only build_report's aggregation of
    # whatever collect_trace returns is under test, not trace_report's parsing.
    return tmp_path


def test_build_report_all_checks_passed(isolated_root, monkeypatch):
    _seed_minimal_tree(isolated_root)
    monkeypatch.setattr(mr, "collect_trace", lambda: None)
    out = isolated_root / "downloads" / "report.pdf"
    ok, total, failed = mr.build_report(out, "build")
    assert ok is True
    assert total == 1
    assert failed == 0
    assert out.is_file()


def test_build_report_failures_and_findings_flip_verdict(isolated_root, monkeypatch):
    _seed_minimal_tree(isolated_root)
    (isolated_root / "test-results" / "tst_Bar.xml").write_text(
        '<testsuite name="tst_Bar" time="0.1">'
        '<testcase name="testBroken" time="0.1"><failure message="x"/></testcase>'
        '</testsuite>', encoding="utf-8")
    (isolated_root / "analysis-results" / "cppcheck.txt").write_text(
        "warning: leak\n", encoding="utf-8")
    (isolated_root / "analysis-results" / "sanitize-asan.txt").write_text(
        "heap-use-after-free\n", encoding="utf-8")
    monkeypatch.setattr(mr, "collect_trace", lambda: None)

    out = isolated_root / "downloads" / "report.pdf"
    ok, total, failed = mr.build_report(out, "build")
    assert ok is False
    assert failed == 1
    assert out.is_file()


def test_build_report_with_trace_and_axivion_gates(isolated_root, monkeypatch):
    import trace_report as tr

    _seed_minimal_tree(isolated_root)
    # Four requirements, one in each traceability verdict state (failing test,
    # verified, test-not-executed, no automated test), so build_report's
    # per-requirement verdict branches (rep row colouring) are all exercised.
    monkeypatch.setattr(tr, "parse_requirements", lambda: {
        "REQ-F-001": "verified path", "REQ-F-002": "failing path",
        "REQ-F-003": "spec only, never executed", "REQ-F-004": "inspection only"})
    monkeypatch.setattr(tr, "parse_design", lambda: {})
    monkeypatch.setattr(tr, "parse_test_spec", lambda: {
        "TS-A-001": "row", "TS-A-002": "row", "TS-A-003": "row"})
    monkeypatch.setattr(tr, "parse_test_impl", lambda: {
        "TS-A-001": {"function": "testOne", "verifies": {"REQ-F-001"}},
        "TS-A-002": {"function": "testTwo", "verifies": {"REQ-F-002"}},
        "TS-A-003": {"function": "testThree", "verifies": {"REQ-F-003"}}})
    monkeypatch.setattr(tr, "parse_results", lambda: {
        "testOne": ("PASS", "tst_Foo"), "testTwo": ("FAIL", "tst_Foo")})

    monkeypatch.setattr(mr, "collect_axivion", lambda: {
        "project": "TradingApp", "version": 7, "name": "v7",
        "counts": {"AV": {"Total": 0, "Added": 0, "Removed": 0},
                   "CL": {"Total": 2, "Added": 0, "Removed": 0},
                   "CY": {"Total": 0, "Added": 0, "Removed": 0},
                   "DE": {"Total": 0, "Added": 0, "Removed": 0},
                   "MV": {"Total": 0, "Added": 0, "Removed": 0},
                   "SV": {"Total": 100, "Added": 5, "Removed": 1}},
        "metrics": {"Metric.Lines.LOC.sum": {"value": 21292, "label": "LOC"},
                    "Metric.McCabe_Complexity.avg": {"value": 4.273, "label": "avg"},
                    "Metric.McCabe_Complexity.max": {"value": None, "label": "worst"}},
    })

    out = isolated_root / "downloads" / "report.pdf"
    ok, total, failed = mr.build_report(out, "build")
    assert ok is True  # AV/CL not part of everything_ok's own gate list here
    assert out.is_file()


def test_build_report_with_axivion_metrics_empty_skips_metrics_section(isolated_root,
                                                                        monkeypatch):
    _seed_minimal_tree(isolated_root)
    monkeypatch.setattr(mr, "collect_trace", lambda: None)
    monkeypatch.setattr(mr, "collect_axivion", lambda: {
        "project": "TradingApp", "version": 1, "name": "v1",
        "counts": {"AV": {"Total": 0, "Added": 0, "Removed": 0}},
        "metrics": {},
    })
    out = isolated_root / "downloads" / "report.pdf"
    mr.build_report(out, "build")
    assert out.is_file()


def test_build_report_with_metrics_and_coverage_artefacts(isolated_root, monkeypatch):
    _seed_minimal_tree(isolated_root)
    monkeypatch.setattr(mr, "collect_trace", lambda: None)
    (isolated_root / "tools" / "lizard_baseline.json").write_text(
        json.dumps({"a::b": {"ccn": 10}}), encoding="utf-8")
    (isolated_root / "analysis-results" / "lizard-metrics.csv").write_text(
        "file;function;line;end;nloc;ccn;tokens;params\n"
        "src/a.cpp;fn;1;2;5;3;10;1\n", encoding="utf-8")
    gcov = isolated_root / "coverage" / "gcov"
    gcov.mkdir(parents=True)
    (gcov / "coverage.info").write_text("LF:10\nLH:8\nFNF:2\nFNH:2\n", encoding="utf-8")
    mcdc = isolated_root / "coverage" / "mcdc"
    mcdc.mkdir(parents=True)
    (mcdc / "summary.txt").write_text(
        "src/domain/Foo.cpp   10   2   80.0%   75.0%\n"
        "TOTAL                10   2   80.0%   88.5%\n", encoding="utf-8")
    for folder in ("coco-unit", "coco-integration", "coco-gui"):
        d = isolated_root / "coverage" / folder
        d.mkdir(parents=True)
        (d / "summary.json").write_text(json.dumps(
            {"statement": {"percent": 80, "covered": 8, "total": 10},
             "decision": {"percent": 70, "covered": 7, "total": 10},
             "condition": {"percent": 60, "covered": 6, "total": 10},
             "mcdc": {"percent": 50, "covered": 5, "total": 10}}), encoding="utf-8")
    # The combined "coco" figure deliberately omits "mcdc" so _levels' "no data
    # for this level" fallback (`if not data: return f"{name} —"`) is exercised.
    coco_all = isolated_root / "coverage" / "coco"
    coco_all.mkdir(parents=True)
    (coco_all / "summary.json").write_text(json.dumps(
        {"statement": {"percent": 80, "covered": 8, "total": 10}}), encoding="utf-8")
    (isolated_root / "analysis-results" / "tool-versions.json").write_text(json.dumps({
        "tools": [{"tool": "cppcheck", "version": "2.13",
                   "invocation": "cppcheck --enable=all",
                   "artefact": "analysis-results/cppcheck.txt"},
                  {"tool": "missing-tool", "version": "not installed",
                   "invocation": "n/a", "artefact": ""}]
    }), encoding="utf-8")
    (isolated_root / "analysis-results" / "external_findings.csv").write_text(
        "tool;file;line;message\ncppcheck;a.cpp;1;m\n", encoding="utf-8")
    (isolated_root / "axivion").mkdir()
    (isolated_root / "axivion" / "external_import.py").write_text(
        "PROVIDERS = {\n    'cppcheck': ('a', 'b'),\n    'sonarqube': ('a', 'b'),\n}\n",
        encoding="utf-8")

    out = isolated_root / "downloads" / "report.pdf"
    ok, total, failed = mr.build_report(out, "build")
    assert out.is_file()
    assert total == 1


def test_build_report_wired_providers_but_nothing_imported(isolated_root, monkeypatch):
    """A provider is wired into axivion/external_import.py but the merged CSV is
    empty (every gated analyzer reported zero) — the "Nothing to import" branch."""
    _seed_minimal_tree(isolated_root)
    monkeypatch.setattr(mr, "collect_trace", lambda: None)
    (isolated_root / "axivion").mkdir()
    (isolated_root / "axivion" / "external_import.py").write_text(
        "PROVIDERS = {\n    'cppcheck': ('a', 'b'),\n}\n", encoding="utf-8")
    out = isolated_root / "downloads" / "report.pdf"
    mr.build_report(out, "build")
    assert out.is_file()


# ---------------------------------------------------------------------------
# main() — CLI argument parsing and top-level error handling
# ---------------------------------------------------------------------------
def test_main_success_path(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(mr, "build_report", lambda out, build_dir: (True, 5, 0))
    monkeypatch.setattr(mr.sys, "argv", ["make_report.py", "--out", str(tmp_path / "r.pdf"),
                                        "--build-dir", "build-release"])
    rc = mr.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert "all checks passed" in captured.out


def test_main_reports_failure_summary(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(mr, "build_report", lambda out, build_dir: (False, 5, 2))
    monkeypatch.setattr(mr.sys, "argv", ["make_report.py", "--out", str(tmp_path / "r.pdf")])
    rc = mr.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert "attention required" in captured.out
    assert "2 failed" in captured.out


def test_main_relative_out_path_resolved_against_root(tmp_path, monkeypatch):
    seen = {}

    def fake_build_report(out, build_dir):
        seen["out"] = out
        return True, 1, 0

    monkeypatch.setattr(mr, "build_report", fake_build_report)
    monkeypatch.setattr(mr.sys, "argv", ["make_report.py", "--out", "downloads/x.pdf"])
    mr.main()
    assert seen["out"] == mr.ROOT / "downloads" / "x.pdf"


def test_main_returns_1_on_unexpected_exception(monkeypatch, capsys):
    def raiser(out, build_dir):
        raise RuntimeError("boom")

    monkeypatch.setattr(mr, "build_report", raiser)
    monkeypatch.setattr(mr.sys, "argv", ["make_report.py"])
    rc = mr.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "report FAILED" in captured.err
