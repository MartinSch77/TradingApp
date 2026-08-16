# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for the PARSING and COMPUTATION logic in tools/make_report.py.

Rendering (reportlab Canvas/Table/Chart calls) is covered separately in
test_make_report_render.py with smoke tests; this file targets every function
that turns an artefact (JUnit XML, CSV, JSON, ini, lcov tracefile, HTTP JSON)
into a plain Python data structure, plus the small derived-value helpers.

No test depends on this repository's own test-results/analysis-results/
coverage/ trees: every artefact is synthesized under tmp_path and
make_report.ROOT is monkeypatched to point at it, so the assertions never
drift with the real pipeline's output.
"""

import json
import subprocess
import urllib.error

import pytest

import make_report as mr


# ---------------------------------------------------------------------------
# verdict_colour
# ---------------------------------------------------------------------------
def test_verdict_colour_ok():
    assert mr.verdict_colour(True) == mr.GREEN


def test_verdict_colour_not_ok():
    assert mr.verdict_colour(False) == mr.RED


def test_verdict_colour_warn_overrides_ok():
    assert mr.verdict_colour(True, warn=True) == mr.AMBER
    assert mr.verdict_colour(False, warn=True) == mr.AMBER


# ---------------------------------------------------------------------------
# git_info
# ---------------------------------------------------------------------------
def test_git_info_normal(monkeypatch):
    def fake_run(args, cwd, capture_output, text, timeout, check):
        mapping = {
            ("git", "status", "--porcelain"): "M file1.cpp\n M file2.cpp\n",
            ("git", "rev-parse", "--short", "HEAD"): "abc1234\n",
            ("git", "log", "-1", "--pretty=%s"): "Fix the thing\n",
            ("git", "rev-parse", "--abbrev-ref", "HEAD"): "main\n",
        }
        out = mapping.get(tuple(args), "")
        return subprocess.CompletedProcess(args, 0, stdout=out, stderr="")

    monkeypatch.setattr(mr.subprocess, "run", fake_run)
    info = mr.git_info()
    assert info == {"commit": "abc1234", "subject": "Fix the thing",
                     "branch": "main", "dirty": 2}


def test_git_info_empty_outputs_fall_back_to_na(monkeypatch):
    def fake_run(args, cwd, capture_output, text, timeout, check):
        return subprocess.CompletedProcess(args, 0, stdout="", stderr="")

    monkeypatch.setattr(mr.subprocess, "run", fake_run)
    info = mr.git_info()
    assert info["commit"] == "n/a"
    assert info["subject"] == "n/a"
    assert info["branch"] == "n/a"
    assert info["dirty"] == 0


def test_git_info_subprocess_raises_oserror(monkeypatch):
    def fake_run(*a, **k):
        raise OSError("git not found")

    monkeypatch.setattr(mr.subprocess, "run", fake_run)
    info = mr.git_info()
    assert info["commit"] == "n/a"
    assert info["dirty"] == 0


# ---------------------------------------------------------------------------
# toolchain_info
# ---------------------------------------------------------------------------
def test_toolchain_info_reads_cmake_cache(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text(
        "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n"
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "Qt6_DIR:PATH=/opt/Qt/6.10.0/gcc_64/lib/cmake/Qt6\n"
        "CMAKE_CXX_STANDARD:STRING=23\n"
        "UNRELATED:STRING=ignored\n",
        encoding="utf-8",
    )
    info = mr.toolchain_info("build")
    assert info["C++ compiler"] == "/usr/bin/clang++"
    assert info["Build type"] == "Release"
    assert info["Qt kit"].endswith("Qt6")
    assert info["C++ standard"] == "23"
    assert "Host" in info
    assert "Python" in info


def test_toolchain_info_missing_cache_still_reports_host(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    info = mr.toolchain_info("build-does-not-exist")
    assert "C++ compiler" not in info
    assert "Host" in info
    assert "Python" in info


# ---------------------------------------------------------------------------
# collect_tests
# ---------------------------------------------------------------------------
def _write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def test_collect_tests_no_directory(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    assert mr.collect_tests() == []


def test_collect_tests_single_testsuite_root(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    xml = """<?xml version="1.0"?>
<testsuite name="tst_Foo" time="1.5">
  <testcase name="initTestCase" time="0.01"/>
  <testcase name="testOne" time="0.20"/>
  <testcase name="testFails" time="0.30"><failure message="boom"/></testcase>
  <testcase name="testErrors" time="0.10"><error message="oops"/></testcase>
  <testcase name="testSkipped" time="0.05"><skipped message="n/a"/></testcase>
  <testcase name="cleanupTestCase" time="0.01"/>
</testsuite>
"""
    _write(tmp_path / "test-results" / "tst_Foo.xml", xml)
    suites = mr.collect_tests()
    assert len(suites) == 1
    suite = suites[0]
    assert suite["suite"] == "tst_Foo"
    assert suite["total"] == 4  # init/cleanup filtered out
    assert suite["passed"] == 1
    assert suite["failed"] == 2
    assert suite["skipped"] == 1
    assert suite["seconds"] == pytest.approx(1.5)
    names = [c[0] for c in suite["cases"]]
    assert "initTestCase" not in names
    assert "cleanupTestCase" not in names


def test_collect_tests_testsuites_wrapper_multiple_nodes(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    xml = """<?xml version="1.0"?>
<testsuites>
  <testsuite name="A" time="1.0">
    <testcase name="a1" time="0.5"/>
  </testsuite>
  <testsuite name="B" time="2.0">
    <testcase name="b1" time="1.0"/>
  </testsuite>
</testsuites>
"""
    _write(tmp_path / "test-results" / "wrapped.xml", xml)
    suites = mr.collect_tests()
    assert len(suites) == 1
    assert suites[0]["total"] == 2
    assert suites[0]["seconds"] == pytest.approx(3.0)


def test_collect_tests_skips_malformed_xml(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    _write(tmp_path / "test-results" / "broken.xml", "<testsuite><not closed")
    assert mr.collect_tests() == []


def test_collect_tests_missing_time_attrs_default_zero(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    xml = '<testsuite name="X"><testcase name="t"/></testsuite>'
    _write(tmp_path / "test-results" / "x.xml", xml)
    suites = mr.collect_tests()
    assert suites[0]["seconds"] == 0.0
    assert suites[0]["cases"][0][2] == 0.0


# ---------------------------------------------------------------------------
# collect_trace — the underlying trace_report parsers are stubbed out so the
# assertions describe collect_trace's OWN join/aggregation logic, not
# trace_report's file parsing (that belongs to trace_report's own tests).
# ---------------------------------------------------------------------------
def test_collect_trace_none_when_trace_report_unimportable(monkeypatch):
    import sys as _sys

    # A None entry in sys.modules makes `import trace_report` raise
    # ImportError directly (the documented meaning of that sentinel), which is
    # exactly the branch collect_trace's `except ImportError: return None` guards.
    monkeypatch.setitem(_sys.modules, "trace_report", None)
    assert mr.collect_trace() is None


def test_collect_trace_joins_and_aggregates(monkeypatch):
    import trace_report as tr

    monkeypatch.setattr(tr, "parse_requirements", lambda: {
        "REQ-F-001": "does a thing",
        "REQ-F-002": "does another thing",
        "REQ-F-003": "manual only",
    })
    monkeypatch.setattr(tr, "parse_design", lambda: {"DES-X": {"REQ-F-001"}})
    monkeypatch.setattr(tr, "parse_test_spec", lambda: {"TS-A-001": "row", "TS-A-002": "row"})
    monkeypatch.setattr(tr, "parse_test_impl", lambda: {
        "TS-A-001": {"function": "testOne", "verifies": {"REQ-F-001"}},
        "TS-A-002": {"function": "testTwo", "verifies": {"REQ-F-002"}},
    })
    monkeypatch.setattr(tr, "parse_results", lambda: {
        "testOne": ("PASS", "suite"),
        "testTwo": ("FAIL", "suite"),
    })

    result = mr.collect_trace()
    assert result is not None
    assert result["hard_gaps"] == []
    assert result["per_req"]["REQ-F-001"]["executed"] == ["TS-A-001"]
    assert result["per_req"]["REQ-F-001"]["failed"] == []
    assert result["per_req"]["REQ-F-002"]["failed"] == ["TS-A-002"]
    assert result["per_req"]["REQ-F-003"]["tests"] == []
    # only REQ-F-001 is executed and not failed
    assert result["covered"] == 1


def test_collect_trace_hard_gap_when_spec_and_impl_disagree(monkeypatch):
    import trace_report as tr

    monkeypatch.setattr(tr, "parse_requirements", lambda: {})
    monkeypatch.setattr(tr, "parse_design", lambda: {})
    monkeypatch.setattr(tr, "parse_test_spec", lambda: {"TS-A-001": "row", "TS-A-999": "row"})
    monkeypatch.setattr(tr, "parse_test_impl", lambda: {"TS-A-001": {"function": "f",
                                                                     "verifies": set()}})
    monkeypatch.setattr(tr, "parse_results", lambda: {})

    result = mr.collect_trace()
    assert result["hard_gaps"] == ["TS-A-999"]


# ---------------------------------------------------------------------------
# collect_analysis
# ---------------------------------------------------------------------------
def test_collect_analysis_mix_of_states(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "cppcheck.txt").write_text("", encoding="utf-8")  # clean, empty file
    (an / "clang-tidy.txt").write_text("warning: something bad\nwarning: else\n",
                                       encoding="utf-8")
    # clang-analyzer.txt, gcc-analyzer.txt, clazy.txt, pmd-cpd.txt, qmllint.txt,
    # codespell.txt are simply absent -> "not run"

    out = mr.collect_analysis()
    by_name = {name: (count, detail) for name, count, detail in out}
    assert by_name["cppcheck"] == (0, "clean")
    count, detail = by_name["clang-tidy"]
    assert count == 2
    assert detail == "warning: something bad"
    assert by_name["clazy"] == (None, "not run")
    assert len(out) == 8


# ---------------------------------------------------------------------------
# collect_metrics
# ---------------------------------------------------------------------------
def test_collect_metrics_no_files(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    result = mr.collect_metrics()
    assert result == {"baselined": 0, "worst": []}


def test_collect_metrics_reads_baseline_and_csv_sorted_by_ccn(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "lizard_baseline.json").write_text(
        json.dumps({"a::b": {"ccn": 10}, "c::d": {"nloc": 200}}), encoding="utf-8")
    an = tmp_path / "analysis-results"
    an.mkdir()
    csv_text = (
        "file;function;line;end;nloc;ccn;tokens;params\n"
        "src/a.cpp;low;1;10;5;3;20;1\n"
        "src/b.cpp;high;1;50;40;25;300;4\n"
        "src/c.cpp;bad_numbers;1;5;notanumber;alsobad;1;1\n"
    )
    (an / "lizard-metrics.csv").write_text(csv_text, encoding="utf-8")

    result = mr.collect_metrics()
    assert result["baselined"] == 2
    # bad row dropped, remaining two sorted by descending ccn
    assert len(result["worst"]) == 2
    assert result["worst"][0][1] == 25
    assert "high" in result["worst"][0][0]
    assert result["worst"][1][1] == 3


def test_collect_metrics_invalid_baseline_json_is_ignored(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "lizard_baseline.json").write_text("{not json", encoding="utf-8")
    result = mr.collect_metrics()
    assert result["baselined"] == 0


def test_collect_metrics_worst_capped_at_eight(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    rows = ["file;function;line;end;nloc;ccn;tokens;params"]
    for i in range(12):
        rows.append(f"src/f{i}.cpp;fn{i};1;2;10;{i};5;1")
    (an / "lizard-metrics.csv").write_text("\n".join(rows), encoding="utf-8")
    result = mr.collect_metrics()
    assert len(result["worst"]) == 8
    assert result["worst"][0][1] == 11  # highest ccn first


# ---------------------------------------------------------------------------
# collect_coverage
# ---------------------------------------------------------------------------
def test_collect_coverage_nothing_present(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    result = mr.collect_coverage()
    assert result["lines"] is None
    assert result["functions"] is None
    assert result["mcdc"] is None
    assert result["mcdc_rows"] == []
    assert result["coco"] == {}


def test_collect_coverage_lcov_and_mcdc_and_coco(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    gcov = tmp_path / "coverage" / "gcov"
    gcov.mkdir(parents=True)
    (gcov / "coverage.info").write_text(
        "LF:100\nLH:80\nFNF:10\nFNH:9\nLF:50\nLH:50\nFNF:5\nFNH:5\n", encoding="utf-8")

    mcdc = tmp_path / "coverage" / "mcdc"
    mcdc.mkdir(parents=True)
    (mcdc / "summary.txt").write_text(
        "Filename                Regions Missed  Cover  MC/DC\n"
        "src/domain/Foo.cpp        10      2     80.0%   75.0%\n"
        "TOTAL                     10      2     80.0%   88.5%\n",
        encoding="utf-8")

    for folder in ("coco", "coco-unit", "coco-integration", "coco-gui"):
        d = tmp_path / "coverage" / folder
        d.mkdir(parents=True)
        (d / "summary.json").write_text(json.dumps({
            "statement": {"percent": 90.0, "covered": 9, "total": 10}
        }), encoding="utf-8")

    result = mr.collect_coverage()
    assert result["lines"] == pytest.approx(100.0 * 130 / 150)
    assert result["functions"] == pytest.approx(100.0 * 14 / 15)
    assert result["mcdc"] == "88.5%"
    assert len(result["mcdc_rows"]) == 1
    assert result["mcdc_rows"][0][0] == "src/domain/Foo.cpp"
    assert result["coco"]["statement"]["percent"] == 90.0
    assert result["coco_unit"]["statement"]["covered"] == 9
    assert result["coco_gui"]["statement"]["total"] == 10


def test_collect_coverage_invalid_coco_json_becomes_empty_dict(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    d = tmp_path / "coverage" / "coco"
    d.mkdir(parents=True)
    (d / "summary.json").write_text("{not json", encoding="utf-8")
    result = mr.collect_coverage()
    assert result["coco"] == {}


def test_collect_coverage_ignores_unrelated_lcov_lines_and_total_without_pct(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    gcov = tmp_path / "coverage" / "gcov"
    gcov.mkdir(parents=True)
    # SF:/end_of_record and similar lcov lines match none of the LF/LH/FNF/FNH
    # prefixes, exercising the "no branch matched" fall-through of the loop.
    (gcov / "coverage.info").write_text(
        "SF:src/a.cpp\nLF:10\nLH:5\nend_of_record\n", encoding="utf-8")
    mcdc = tmp_path / "coverage" / "mcdc"
    mcdc.mkdir(parents=True)
    # A TOTAL line with no "%"-suffixed column: the `if pct:` guard stays False.
    (mcdc / "summary.txt").write_text("TOTAL 10 2\n", encoding="utf-8")
    result = mr.collect_coverage()
    assert result["lines"] == pytest.approx(50.0)
    assert result["functions"] is None
    assert result["mcdc"] is None


def test_collect_coverage_zero_denominators_leave_none(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    gcov = tmp_path / "coverage" / "gcov"
    gcov.mkdir(parents=True)
    (gcov / "coverage.info").write_text("LF:0\nLH:0\nFNF:0\nFNH:0\n", encoding="utf-8")
    result = mr.collect_coverage()
    assert result["lines"] is None
    assert result["functions"] is None


# ---------------------------------------------------------------------------
# _testcentercmd_has_token
# ---------------------------------------------------------------------------
def test_testcentercmd_has_token_true(tmp_path, monkeypatch):
    monkeypatch.setattr(mr.Path, "home", classmethod(lambda cls: tmp_path))
    ini_dir = tmp_path / ".squish" / "ver1"
    ini_dir.mkdir(parents=True)
    (ini_dir / "testcentercmd.ini").write_text(
        "[localhost:8800]\ntoken = \"abc123\"\n", encoding="utf-8")
    assert mr._testcentercmd_has_token("http://localhost:8800") is True


def test_testcentercmd_has_token_false_when_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(mr.Path, "home", classmethod(lambda cls: tmp_path))
    assert mr._testcentercmd_has_token("http://localhost:8800") is False


def test_testcentercmd_has_token_false_when_section_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(mr.Path, "home", classmethod(lambda cls: tmp_path))
    ini_dir = tmp_path / ".squish" / "ver1"
    ini_dir.mkdir(parents=True)
    (ini_dir / "testcentercmd.ini").write_text("[localhost:8800]\n", encoding="utf-8")
    assert mr._testcentercmd_has_token("http://localhost:8800") is False


def test_testcentercmd_has_token_checks_appdata_candidate(tmp_path, monkeypatch):
    monkeypatch.setattr(mr.Path, "home", classmethod(lambda cls: tmp_path / "nohome"))
    appdata = tmp_path / "AppData"
    ini_dir = appdata / "froglogic" / "Squish" / "ver1"
    ini_dir.mkdir(parents=True)
    (ini_dir / "testcentercmd.ini").write_text(
        "[localhost:8800]\ntoken = mytoken\n", encoding="utf-8")
    monkeypatch.setenv("APPDATA", str(appdata))
    assert mr._testcentercmd_has_token("http://localhost:8800") is True


def test_testcentercmd_has_token_malformed_ini_is_ignored(tmp_path, monkeypatch):
    monkeypatch.setattr(mr.Path, "home", classmethod(lambda cls: tmp_path))
    ini_dir = tmp_path / ".squish" / "ver1"
    ini_dir.mkdir(parents=True)
    # duplicate section header -> configparser.DuplicateSectionError
    (ini_dir / "testcentercmd.ini").write_text(
        "[localhost:8800]\ntoken=a\n[localhost:8800]\ntoken=b\n", encoding="utf-8")
    assert mr._testcentercmd_has_token("http://localhost:8800") is False


# ---------------------------------------------------------------------------
# _runs_ok
# ---------------------------------------------------------------------------
def test_runs_ok_true(monkeypatch):
    monkeypatch.setattr(mr.subprocess, "run",
                        lambda *a, **k: subprocess.CompletedProcess(a, 0))
    assert mr._runs_ok(["true"]) is True


def test_runs_ok_false_on_nonzero_exit(monkeypatch):
    monkeypatch.setattr(mr.subprocess, "run",
                        lambda *a, **k: subprocess.CompletedProcess(a, 1))
    assert mr._runs_ok(["false"]) is False


def test_runs_ok_false_on_oserror(monkeypatch):
    def raiser(*a, **k):
        raise OSError("no such file")

    monkeypatch.setattr(mr.subprocess, "run", raiser)
    assert mr._runs_ok(["missing-binary"]) is False


# ---------------------------------------------------------------------------
# collect_external_import
# ---------------------------------------------------------------------------
def test_collect_external_import_nothing_present(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    rows, wired = mr.collect_external_import()
    assert rows == []
    assert wired == []


def test_collect_external_import_counts_and_wired_providers(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "external_findings.csv").write_text(
        "tool;file;line;message\n"
        "cppcheck;a.cpp;1;msg\n"
        "cppcheck;b.cpp;2;msg\n"
        "clazy;a.cpp;3;msg\n",
        encoding="utf-8")
    axdir = tmp_path / "axivion"
    axdir.mkdir()
    (axdir / "external_import.py").write_text(
        "PROVIDERS = {\n"
        "    'cppcheck': ('x', 'y'),\n"
        "    'clazy': ('x', 'y'),\n"
        "    'sonarqube': ('x', 'y'),\n"
        "}\n",
        encoding="utf-8")
    rows, wired = mr.collect_external_import()
    assert rows == [("cppcheck", 2), ("clazy", 1)]
    assert wired == ["clazy", "cppcheck", "sonarqube"]


def test_collect_external_import_bad_csv_returns_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    # A directory instead of a file trips OSError on open().
    bad = an / "external_findings.csv"
    bad.mkdir()
    rows, wired = mr.collect_external_import()
    assert rows == []
    assert wired == []


def test_collect_external_import_csv_reader_error_returns_empty(tmp_path, monkeypatch):
    import csv as _csv

    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "external_findings.csv").write_text("tool;file;line;message\n", encoding="utf-8")

    def raiser(*a, **k):
        raise _csv.Error("malformed csv")

    monkeypatch.setattr(mr.csv, "reader", raiser)
    rows, wired = mr.collect_external_import()
    assert rows == []
    assert wired == []


def test_collect_external_import_importer_unreadable_leaves_wired_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    axdir = tmp_path / "axivion"
    axdir.mkdir()
    (axdir / "external_import.py").write_text("irrelevant", encoding="utf-8")
    orig_read_text = mr.Path.read_text

    def raiser(self, *a, **k):
        if self.name == "external_import.py":
            raise OSError("permission denied")
        return orig_read_text(self, *a, **k)

    monkeypatch.setattr(mr.Path, "read_text", raiser)
    rows, wired = mr.collect_external_import()
    assert wired == []


# ---------------------------------------------------------------------------
# collect_tool_versions
# ---------------------------------------------------------------------------
def test_collect_tool_versions_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    assert mr.collect_tool_versions() == []


def test_collect_tool_versions_present(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "tool-versions.json").write_text(
        json.dumps({"tools": [{"tool": "cppcheck", "version": "2.13"}]}), encoding="utf-8")
    tools = mr.collect_tool_versions()
    assert tools == [{"tool": "cppcheck", "version": "2.13"}]


def test_collect_tool_versions_invalid_json(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "tool-versions.json").write_text("{not json", encoding="utf-8")
    assert mr.collect_tool_versions() == []


# ---------------------------------------------------------------------------
# _artefact_verdict
# ---------------------------------------------------------------------------
def test_artefact_verdict_absent():
    text, colour = mr._artefact_verdict("analysis-results/x.txt", absent=True)
    assert text == "not run"
    assert colour == mr.GREY


def test_artefact_verdict_no_artefact_name():
    text, colour = mr._artefact_verdict("", absent=False)
    assert text == "—"
    assert colour == mr.GREY


def test_artefact_verdict_file_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    text, colour = mr._artefact_verdict("analysis-results/missing.txt", absent=False)
    assert text == "no artefact"
    assert colour == mr.GREY


def test_artefact_verdict_findings_present_and_clean(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "cppcheck.txt").write_text("finding one\nfinding two\n", encoding="utf-8")
    (an / "clean.txt").write_text("", encoding="utf-8")
    text, colour = mr._artefact_verdict("analysis-results/cppcheck.txt", absent=False)
    assert text == "2 findings"
    assert colour == mr.RED
    text, colour = mr._artefact_verdict("analysis-results/clean.txt", absent=False)
    assert text == "0 — pass"
    assert colour == mr.GREEN


def test_artefact_verdict_unreadable_file(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "cppcheck.txt").write_text("finding\n", encoding="utf-8")
    orig_read_text = mr.Path.read_text

    def raiser(self, *a, **k):
        if self.name == "cppcheck.txt":
            raise OSError("permission denied")
        return orig_read_text(self, *a, **k)

    monkeypatch.setattr(mr.Path, "read_text", raiser)
    text, colour = mr._artefact_verdict("analysis-results/cppcheck.txt", absent=False)
    assert text == "unreadable"
    assert colour == mr.GREY


def test_artefact_verdict_object_names_special_case(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "object-names.txt").write_text(
        "SUCCESS: every widget has an objectName\n", encoding="utf-8")
    text, colour = mr._artefact_verdict("analysis-results/object-names.txt", absent=False)
    assert text == "0 — pass"
    assert colour == mr.GREEN

    (an / "object-names.txt").write_text(
        "QPushButton has no objectName\nQLabel has no objectName\n"
        "some other unrelated line\n", encoding="utf-8")
    text, colour = mr._artefact_verdict("analysis-results/object-names.txt", absent=False)
    assert text == "2 findings"
    assert colour == mr.RED


# ---------------------------------------------------------------------------
# collect_axivion / _axivion_system_metrics
# ---------------------------------------------------------------------------
class _FakeResponse:
    def __init__(self, payload):
        self._payload = json.dumps(payload).encode("utf-8")

    def read(self):
        return self._payload

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def test_collect_axivion_none_when_unreachable(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)

    def raiser(*a, **k):
        raise urllib.error.URLError("connection refused")

    monkeypatch.setattr(mr.urllib.request, "urlopen", raiser)
    assert mr.collect_axivion() is None


def test_collect_axivion_none_when_no_counts(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    monkeypatch.setattr(mr.urllib.request, "urlopen",
                        lambda *a, **k: _FakeResponse({"endVersion": {"index": 1}}))
    assert mr.collect_axivion() is None


def test_collect_axivion_success_reads_ci_config_project_name(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    axdir = tmp_path / "axivion"
    axdir.mkdir()
    (axdir / "ci_config.json").write_text(json.dumps({
        "Project": {"Project-GlobalOptions": {"name": "MyProject"}}
    }), encoding="utf-8")

    payload = {"endVersion": {"index": 42, "name": "v42",
                              "issueCounts": {"SV": {"Total": 3}}}}
    monkeypatch.setattr(mr.urllib.request, "urlopen",
                        lambda *a, **k: _FakeResponse(payload))
    monkeypatch.setattr(mr, "_axivion_system_metrics", lambda *a, **k: {})

    result = mr.collect_axivion()
    assert result["project"] == "MyProject"
    assert result["version"] == 42
    assert result["counts"]["SV"]["Total"] == 3


def test_collect_axivion_bad_ci_config_keeps_default_project(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    axdir = tmp_path / "axivion"
    axdir.mkdir()
    (axdir / "ci_config.json").write_text("{not json", encoding="utf-8")
    payload = {"endVersion": {"index": 1, "issueCounts": {"SV": {"Total": 0}}}}
    monkeypatch.setattr(mr.urllib.request, "urlopen",
                        lambda *a, **k: _FakeResponse(payload))
    monkeypatch.setattr(mr, "_axivion_system_metrics", lambda *a, **k: {})
    result = mr.collect_axivion()
    assert result["project"] == "TradingApp"


def test_axivion_system_metrics_empty_when_no_entities(monkeypatch):
    monkeypatch.setattr(mr.urllib.request, "urlopen",
                        lambda *a, **k: _FakeResponse({"entities": []}))
    assert mr._axivion_system_metrics("http://x", "token", "Proj") == {}


def test_axivion_system_metrics_returns_values(monkeypatch):
    calls = []

    def fake_urlopen(req, timeout=None):
        url = req.full_url
        calls.append(url)
        if "getSystemEntity" in url:
            return _FakeResponse({"entities": [{"id": "E1"}]})
        if "getMetrics" in url:
            return _FakeResponse({"metrics": [
                {"name": "Metric.Lines.LOC.sum", "displayName": "LOC"},
                {"name": "Metric.Broken", "displayName": "Broken"},
            ]})
        if "Metric.Lines.LOC.sum" in url:
            return _FakeResponse({"values": [100, 200]})
        if "Metric.Broken" in url:
            raise urllib.error.URLError("boom")
        raise AssertionError(f"unexpected url {url}")

    monkeypatch.setattr(mr.urllib.request, "urlopen", fake_urlopen)
    metrics = mr._axivion_system_metrics("http://x", "token", "Proj")
    assert metrics["Metric.Lines.LOC.sum"]["value"] == 200
    assert metrics["Metric.Lines.LOC.sum"]["label"] == "LOC"
    assert "Metric.Broken" not in metrics


def test_axivion_system_metrics_outer_failure_returns_empty(monkeypatch):
    def fake_urlopen(req, timeout=None):
        raise urllib.error.URLError("dashboard down")

    monkeypatch.setattr(mr.urllib.request, "urlopen", fake_urlopen)
    assert mr._axivion_system_metrics("http://x", "token", "Proj") == {}


def test_axivion_system_metrics_skips_unnamed_metric(monkeypatch):
    def fake_urlopen(req, timeout=None):
        url = req.full_url
        if "getSystemEntity" in url:
            return _FakeResponse({"entities": [{"id": "E1"}]})
        if "getMetrics" in url:
            # one metric dict carries no "name" key at all
            return _FakeResponse({"metrics": [{"displayName": "Nameless"}]})
        raise AssertionError("queryMetricValueRange should never be reached")

    monkeypatch.setattr(mr.urllib.request, "urlopen", fake_urlopen)
    metrics = mr._axivion_system_metrics("http://x", "token", "Proj")
    assert metrics == {}


def test_axivion_system_metrics_metric_with_no_values_is_none(monkeypatch):
    def fake_urlopen(req, timeout=None):
        url = req.full_url
        if "getSystemEntity" in url:
            return _FakeResponse({"entities": [{"id": "E1"}]})
        if "getMetrics" in url:
            return _FakeResponse({"metrics": [{"name": "Metric.X", "displayName": "X"}]})
        return _FakeResponse({"values": []})

    monkeypatch.setattr(mr.urllib.request, "urlopen", fake_urlopen)
    metrics = mr._axivion_system_metrics("http://x", "token", "Proj")
    assert metrics["Metric.X"]["value"] is None


# ---------------------------------------------------------------------------
# collect_sanitizers
# ---------------------------------------------------------------------------
def test_collect_sanitizers_none(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    assert mr.collect_sanitizers() == []


def test_collect_sanitizers_skips_raw_files(tmp_path, monkeypatch):
    monkeypatch.setattr(mr, "ROOT", tmp_path)
    an = tmp_path / "analysis-results"
    an.mkdir()
    (an / "sanitize-asan.txt").write_text("leak detected\n", encoding="utf-8")
    (an / "sanitize-asan.raw.txt").write_text("raw log noise\nmore noise\n", encoding="utf-8")
    (an / "sanitize-tsan.txt").write_text("", encoding="utf-8")
    out = dict(mr.collect_sanitizers())
    assert out == {"asan": 1, "tsan": 0}


# ---------------------------------------------------------------------------
# pct / _pct_value
# ---------------------------------------------------------------------------
def test_pct_none_is_dash():
    assert mr.pct(None) == "—"


def test_pct_formats_one_decimal():
    assert mr.pct(42.567) == "42.6%"


def test_pct_value_none_or_empty_is_zero():
    assert mr._pct_value(None) == 0.0
    assert mr._pct_value("") == 0.0


def test_pct_value_parses_percentage_string():
    assert mr._pct_value("22.75%") == pytest.approx(22.75)


def test_pct_value_unparsable_is_zero():
    assert mr._pct_value("n/a") == 0.0
