# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/gates_to_junit.py."""

import subprocess
import xml.etree.ElementTree as ET

import pytest

import gates_to_junit as g


# --------------------------------------------------------------------------
# findings_of
# --------------------------------------------------------------------------

def test_findings_of_missing_file_returns_empty(tmp_path):
    assert g.findings_of(tmp_path / "nope.txt") == []


def test_findings_of_plain_artefact_counts_nonblank_lines(tmp_path):
    path = tmp_path / "cppcheck.txt"
    path.write_text("a|1|warning|x|msg\n\n  \nb|2|warning|y|msg2\n", encoding="utf-8")
    assert g.findings_of(path) == ["a|1|warning|x|msg", "b|2|warning|y|msg2"]


def test_findings_of_marker_filters_clean_success_lines(tmp_path):
    path = tmp_path / "object-names.txt"
    path.write_text(
        "every member widget in src/ui has a stable objectName\n"
        "src/ui/Foo.h:12 has no objectName: myWidget\n",
        encoding="utf-8",
    )
    lines = g.findings_of(path)
    assert lines == ["src/ui/Foo.h:12 has no objectName: myWidget"]


def test_findings_of_marker_all_clean_yields_no_findings(tmp_path):
    path = tmp_path / "object-names.txt"
    path.write_text("every member widget in src/ui has a stable objectName\n",
                    encoding="utf-8")
    assert g.findings_of(path) == []


# --------------------------------------------------------------------------
# newest_tracked_source_mtime
# --------------------------------------------------------------------------

def test_newest_tracked_source_mtime_picks_latest(tmp_path, monkeypatch):
    f1 = tmp_path / "a.cpp"
    f2 = tmp_path / "b.cpp"
    f1.write_text("x", encoding="utf-8")
    f2.write_text("y", encoding="utf-8")
    import os
    os.utime(f1, (1000, 1000))
    os.utime(f2, (2000, 2000))

    class FakeResult:
        stdout = "a.cpp\nb.cpp\nmissing.cpp\n"

    def fake_run(*args, **kwargs):
        return FakeResult()

    monkeypatch.setattr(g, "ROOT", tmp_path)
    monkeypatch.setattr(g.subprocess, "run", fake_run)
    newest = g.newest_tracked_source_mtime()
    assert newest == 2000


def test_newest_tracked_source_mtime_handles_empty_listing(tmp_path, monkeypatch):
    class FakeResult:
        stdout = ""

    monkeypatch.setattr(g, "ROOT", tmp_path)
    monkeypatch.setattr(g.subprocess, "run", lambda *a, **k: FakeResult())
    assert g.newest_tracked_source_mtime() is None


def test_newest_tracked_source_mtime_returns_none_on_git_failure(monkeypatch):
    def fake_run(*args, **kwargs):
        raise subprocess.SubprocessError("no git")

    monkeypatch.setattr(g.subprocess, "run", fake_run)
    assert g.newest_tracked_source_mtime() is None


# --------------------------------------------------------------------------
# file_gate
# --------------------------------------------------------------------------

def test_file_gate_missing_artefact_is_error(tmp_path, monkeypatch):
    monkeypatch.setattr(g, "ANALYSIS", tmp_path)
    case = g.file_gate("cppcheck.txt", "cppcheck", "seven-analyzers", None)
    assert case.status == "error"
    assert "no artefact" in case.message


def test_file_gate_stale_artefact_is_error(tmp_path, monkeypatch):
    monkeypatch.setattr(g, "ANALYSIS", tmp_path)
    path = tmp_path / "cppcheck.txt"
    path.write_text("", encoding="utf-8")
    import os
    os.utime(path, (1000, 1000))
    case = g.file_gate("cppcheck.txt", "cppcheck", "seven-analyzers", newest=5000)
    assert case.status == "error"
    assert "stale artefact" in case.message


def test_file_gate_clean_passes(tmp_path, monkeypatch):
    monkeypatch.setattr(g, "ANALYSIS", tmp_path)
    path = tmp_path / "cppcheck.txt"
    path.write_text("", encoding="utf-8")
    case = g.file_gate("cppcheck.txt", "cppcheck", "seven-analyzers", None)
    assert case.status == "passed"
    assert case.count == 0


def test_file_gate_with_findings_fails_and_truncates_detail(tmp_path, monkeypatch):
    monkeypatch.setattr(g, "ANALYSIS", tmp_path)
    path = tmp_path / "cppcheck.txt"
    lines = [f"f{i}.cpp|{i}|warning|rule|msg{i}" for i in range(g.MAX_MESSAGE_FINDINGS + 5)]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    case = g.file_gate("cppcheck.txt", "cppcheck", "seven-analyzers", None)
    assert case.status == "failed"
    assert case.count == g.MAX_MESSAGE_FINDINGS + 5
    assert "more in cppcheck.txt" in case.detail
    assert case.detail.count("\n") == g.MAX_MESSAGE_FINDINGS  # shown lines + trailer


def test_file_gate_uses_marker_for_object_names(tmp_path, monkeypatch):
    monkeypatch.setattr(g, "ANALYSIS", tmp_path)
    path = tmp_path / "object-names.txt"
    path.write_text("every member widget in src/ui has a stable objectName\n",
                    encoding="utf-8")
    case = g.file_gate("object-names.txt", "gui-object-names", "gui-testability", None)
    assert case.status == "passed"


# --------------------------------------------------------------------------
# rerun_gate
# --------------------------------------------------------------------------

def test_rerun_gate_passes_on_exit_zero(monkeypatch):
    class Done:
        returncode = 0
        stdout = "all good\n"
        stderr = ""

    monkeypatch.setattr(g.subprocess, "run", lambda *a, **k: Done())
    case = g.rerun_gate("lizard-metrics-ratchet", ["true"], "metrics")
    assert case.status == "passed"


def test_rerun_gate_skips_on_exit_three(monkeypatch):
    class Done:
        returncode = 3
        stdout = ""
        stderr = "no licence"

    monkeypatch.setattr(g.subprocess, "run", lambda *a, **k: Done())
    case = g.rerun_gate("x", ["true"], "metrics")
    assert case.status == "skipped"
    assert "skipped" in case.message


def test_rerun_gate_fails_on_other_nonzero(monkeypatch):
    class Done:
        returncode = 1
        stdout = "bad\n"
        stderr = ""

    monkeypatch.setattr(g.subprocess, "run", lambda *a, **k: Done())
    case = g.rerun_gate("x", ["true"], "metrics")
    assert case.status == "failed"
    assert "exit 1" in case.message


def test_rerun_gate_timeout(monkeypatch):
    def fake_run(*a, **k):
        raise subprocess.TimeoutExpired(cmd="x", timeout=1)

    monkeypatch.setattr(g.subprocess, "run", fake_run)
    case = g.rerun_gate("x", ["true"], "metrics")
    assert case.status == "error"
    assert "timed out" in case.message


def test_rerun_gate_cannot_run(monkeypatch):
    def fake_run(*a, **k):
        raise OSError("no such file")

    monkeypatch.setattr(g.subprocess, "run", fake_run)
    case = g.rerun_gate("x", ["true"], "metrics")
    assert case.status == "error"
    assert "could not run" in case.message


# --------------------------------------------------------------------------
# build_suite
# --------------------------------------------------------------------------

def test_build_suite_counts_and_nodes():
    cases = []
    passed = g.Case("p", "grp")
    cases.append(passed)
    failed = g.Case("f", "grp")
    failed.status = "failed"
    failed.message = "1 finding(s)"
    failed.detail = "detail"
    cases.append(failed)
    error = g.Case("e", "grp")
    error.status = "error"
    error.message = "boom"
    error.detail = "trace"
    cases.append(error)
    skipped = g.Case("s", "grp")
    skipped.status = "skipped"
    skipped.message = "skip msg"
    cases.append(skipped)
    passed_with_detail = g.Case("p2", "grp")
    passed_with_detail.detail = "some system-out"
    cases.append(passed_with_detail)

    suite = g.build_suite(cases)
    assert suite.attrib["tests"] == "5"
    assert suite.attrib["failures"] == "1"
    assert suite.attrib["errors"] == "1"
    assert suite.attrib["skipped"] == "1"

    testcases = suite.findall("testcase")
    assert len(testcases) == 5
    assert testcases[1].find("failure") is not None
    assert testcases[2].find("error") is not None
    assert testcases[3].find("skipped") is not None
    assert testcases[4].find("system-out") is not None
    assert testcases[0].find("system-out") is None
    assert testcases[0].find("failure") is None


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def _stub_rerun_gates(monkeypatch):
    """Avoid the RERUN_GATES actually invoking real scripts."""
    class Done:
        returncode = 0
        stdout = ""
        stderr = ""

    monkeypatch.setattr(g.subprocess, "run", lambda *a, **k: Done())


def test_main_returns_error_no_analysis_dir(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(g, "ROOT", tmp_path)
    monkeypatch.setattr(g, "ANALYSIS", tmp_path / "missing")
    monkeypatch.setattr("sys.argv", ["gates_to_junit.py"])
    rc = g.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "run tools/static_analysis.sh" in captured.err


def test_main_print_mode_writes_nothing(tmp_path, monkeypatch, capsys):
    analysis = tmp_path / "analysis-results"
    analysis.mkdir()
    for artefact, _, _ in g.FILE_GATES:
        (analysis / artefact).write_text("", encoding="utf-8")
    monkeypatch.setattr(g, "ROOT", tmp_path)
    monkeypatch.setattr(g, "ANALYSIS", analysis)
    monkeypatch.setattr(g, "newest_tracked_source_mtime", lambda: None)
    _stub_rerun_gates(monkeypatch)
    monkeypatch.setattr("sys.argv", ["gates_to_junit.py", "--print"])

    rc = g.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert "gates green" in captured.out
    assert not (tmp_path / "test-results").exists()


def test_main_writes_junit_xml(tmp_path, monkeypatch, capsys):
    analysis = tmp_path / "analysis-results"
    analysis.mkdir()
    for artefact, _, _ in g.FILE_GATES:
        (analysis / artefact).write_text("", encoding="utf-8")
    out_file = tmp_path / "out" / "gates.xml"
    monkeypatch.setattr(g, "ROOT", tmp_path)
    monkeypatch.setattr(g, "ANALYSIS", analysis)
    monkeypatch.setattr(g, "newest_tracked_source_mtime", lambda: None)
    _stub_rerun_gates(monkeypatch)
    monkeypatch.setattr("sys.argv", ["gates_to_junit.py", "--out", str(out_file)])

    rc = g.main()
    assert rc == 0
    assert out_file.is_file()
    tree = ET.parse(out_file)
    root = tree.getroot()
    assert root.tag == "testsuite"
    assert int(root.attrib["tests"]) == len(g.FILE_GATES) + len(g.RERUN_GATES)


def test_main_reports_findings_but_still_exits_zero(tmp_path, monkeypatch, capsys):
    analysis = tmp_path / "analysis-results"
    analysis.mkdir()
    for artefact, _, _ in g.FILE_GATES:
        (analysis / artefact).write_text("", encoding="utf-8")
    (analysis / "cppcheck.txt").write_text("f.cpp|1|warning|x|bad\n", encoding="utf-8")
    out_file = tmp_path / "gates.xml"
    monkeypatch.setattr(g, "ROOT", tmp_path)
    monkeypatch.setattr(g, "ANALYSIS", analysis)
    monkeypatch.setattr(g, "newest_tracked_source_mtime", lambda: None)
    _stub_rerun_gates(monkeypatch)
    monkeypatch.setattr("sys.argv", ["gates_to_junit.py", "--out", str(out_file)])

    rc = g.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert "of" in captured.out and "gates green" in captured.out
    assert "FAIL" in captured.out
