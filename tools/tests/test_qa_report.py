# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/qa_report.py — the SUP.1 process-conformance report.

`qa_report.run()` (git / subprocess-executing helper) is monkeypatched to a
stub in every test that needs it, so nothing here shells out for real or
depends on this repository's actual git history/process docs. ROOT is
monkeypatched to a synthetic tmp_path tree.
"""

import os
import time

import qa_report as qa


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------------------
# run() — the real subprocess wrapper (every check_* above stubs this out;
# these two exercise the wrapper itself, harmlessly, with a real interpreter).
# ---------------------------------------------------------------------------


def test_run_executes_command_and_returns_output():
    import sys as _sys

    rc, out = qa.run([_sys.executable, "-c", "print('hello')"])
    assert rc == 0
    assert out == "hello"


def test_run_returns_1_on_missing_executable():
    rc, out = qa.run(["definitely-not-a-real-executable-xyz"])
    assert rc == 1
    assert "could not run" in out


# ---------------------------------------------------------------------------
# check_sup1
# ---------------------------------------------------------------------------


def test_check_sup1_confirmed_on_rc_zero(monkeypatch):
    monkeypatch.setattr(qa, "run", lambda cmd: (0, "check_process_docs: OK"))
    v = qa.check_sup1()
    assert v.status == "CONFIRMED"
    assert v.evidence == ["check_process_docs: OK"]


def test_check_sup1_partial_on_nonzero_rc(monkeypatch):
    monkeypatch.setattr(qa, "run", lambda cmd: (1, "some finding"))
    v = qa.check_sup1()
    assert v.status == "PARTIAL"


# ---------------------------------------------------------------------------
# check_swe1
# ---------------------------------------------------------------------------


def test_check_swe1_not_found_when_sdoc_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    v = qa.check_swe1()
    assert v.status == "NOT FOUND"


def test_check_swe1_confirmed_when_all_have_verification(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(
        tmp_path / "requirements" / "requirements.sdoc",
        "[REQUIREMENT]\nUID: REQ-F-001\nVERIFICATION: T\n\n"
        "[REQUIREMENT]\nUID: REQ-F-002\nVERIFICATION: A\n",
    )
    v = qa.check_swe1()
    assert v.status == "CONFIRMED"
    assert "2 requirements, 0 missing" in v.evidence[0]


def test_check_swe1_partial_when_some_missing_verification(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(
        tmp_path / "requirements" / "requirements.sdoc",
        "[REQUIREMENT]\nUID: REQ-F-001\nVERIFICATION: T\n\n"
        "[REQUIREMENT]\nUID: REQ-F-002\nTITLE: no verification here\n",
    )
    v = qa.check_swe1()
    assert v.status == "PARTIAL"
    assert "1 missing a VERIFICATION field" in v.evidence[0]


# ---------------------------------------------------------------------------
# check_swe2_swe3
# ---------------------------------------------------------------------------


def test_check_swe2_swe3_confirmed_when_both_present(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "docs" / "architecture.md", "line1\nline2\n")
    write(tmp_path / "docs" / "design.md", "line1\n")
    v = qa.check_swe2_swe3()
    assert v.status == "CONFIRMED"


def test_check_swe2_swe3_not_found_when_one_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "docs" / "architecture.md", "line1\n")
    v = qa.check_swe2_swe3()
    assert v.status == "NOT FOUND"
    assert any("design.md: NOT FOUND" in e for e in v.evidence)


# ---------------------------------------------------------------------------
# check_swe4_5_6
# ---------------------------------------------------------------------------


def test_check_swe4_5_6_not_found_when_no_results(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    v = qa.check_swe4_5_6()
    assert v.status == "NOT FOUND"


def test_check_swe4_5_6_confirmed_when_clean_and_fresh(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    results = tmp_path / "test-results" / "one.xml"
    write(results, '<testsuite tests="2" failures="0" errors="0"></testsuite>')
    # No tracked sources at all -> "git ls-files" stub returns nothing, so
    # nothing can be stale.
    monkeypatch.setattr(qa, "run", lambda cmd: (0, ""))
    v = qa.check_swe4_5_6()
    assert v.status == "CONFIRMED"
    assert "stale (a tracked source newer than every test result): False" in v.evidence[1]


def test_check_swe4_5_6_not_found_on_failures(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "test-results" / "one.xml", '<testsuite tests="2" failures="1" errors="0"></testsuite>')
    monkeypatch.setattr(qa, "run", lambda cmd: (0, ""))
    v = qa.check_swe4_5_6()
    assert v.status == "NOT FOUND"


def test_check_swe4_5_6_not_found_on_parse_error(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "test-results" / "broken.xml", "<not-xml")
    monkeypatch.setattr(qa, "run", lambda cmd: (0, ""))
    v = qa.check_swe4_5_6()
    assert v.status == "NOT FOUND"  # malformed xml counts as a failure


def test_check_swe4_5_6_not_found_when_source_is_stale(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    results = tmp_path / "test-results" / "one.xml"
    write(results, '<testsuite tests="1" failures="0" errors="0"></testsuite>')
    now = time.time()
    os.utime(results, (now - 100, now - 100))

    src = tmp_path / "src" / "main.cpp"
    write(src, "int main() {}\n")
    os.utime(src, (now, now))  # newer than the result

    monkeypatch.setattr(qa, "run", lambda cmd: (0, "src/main.cpp"))
    v = qa.check_swe4_5_6()
    assert v.status == "NOT FOUND"
    assert "stale (a tracked source newer than every test result): True" in v.evidence[1]


def test_check_swe4_5_6_git_ls_files_failure_treated_as_not_stale(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "test-results" / "one.xml", '<testsuite tests="1" failures="0" errors="0"></testsuite>')
    monkeypatch.setattr(qa, "run", lambda cmd: (1, "git error"))
    v = qa.check_swe4_5_6()
    assert v.status == "CONFIRMED"


# ---------------------------------------------------------------------------
# check_sup8
# ---------------------------------------------------------------------------


def test_check_sup8_confirmed_when_tags_exist(monkeypatch):
    monkeypatch.setattr(qa, "run", lambda cmd: (0, "v1.0.0\nv1.1.0\n"))
    v = qa.check_sup8()
    assert v.status == "CONFIRMED"
    assert "2 found" in v.evidence[0]


def test_check_sup8_not_found_when_no_tags(monkeypatch):
    monkeypatch.setattr(qa, "run", lambda cmd: (0, ""))
    v = qa.check_sup8()
    assert v.status == "NOT FOUND"


def test_check_sup8_not_found_when_git_fails(monkeypatch):
    monkeypatch.setattr(qa, "run", lambda cmd: (1, "error"))
    v = qa.check_sup8()
    assert v.status == "NOT FOUND"
    assert v.evidence == ["git tag --list failed"]


# ---------------------------------------------------------------------------
# check_sup9_10
# ---------------------------------------------------------------------------


def test_check_sup9_10_confirmed_when_all_templates_present(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    templates_dir = tmp_path / ".github" / "ISSUE_TEMPLATE"
    templates_dir.mkdir(parents=True)
    for name in ("problem_report.md", "change_request.md", "project_risk.md",
                 "qa_nonconformance.md", "process_improvement.md"):
        write(templates_dir / name, "template")
    v = qa.check_sup9_10()
    assert v.status == "CONFIRMED"


def test_check_sup9_10_partial_when_dir_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    v = qa.check_sup9_10()
    assert v.status == "PARTIAL"
    assert "missing:" in v.evidence[1]


def test_check_sup9_10_partial_when_some_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    templates_dir = tmp_path / ".github" / "ISSUE_TEMPLATE"
    templates_dir.mkdir(parents=True)
    write(templates_dir / "problem_report.md", "template")
    v = qa.check_sup9_10()
    assert v.status == "PARTIAL"


# ---------------------------------------------------------------------------
# check_man5
# ---------------------------------------------------------------------------


def test_check_man5_not_found_when_register_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    v = qa.check_man5()
    assert v.status == "NOT FOUND"


def test_check_man5_confirmed_with_risk_rows(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "process" / "risk-register.md", "| RISK-001 | some risk |\nother line\n")
    v = qa.check_man5()
    assert v.status == "CONFIRMED"
    assert "1 risk row(s)" in v.evidence[0]


def test_check_man5_not_found_when_no_risk_rows(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "process" / "risk-register.md", "no risk rows here\n")
    v = qa.check_man5()
    assert v.status == "NOT FOUND"


# ---------------------------------------------------------------------------
# check_process_framework_self
# ---------------------------------------------------------------------------


def test_check_process_framework_self_confirmed(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "process" / "VERSION", "1.0\n")
    write(tmp_path / "process" / "CHANGELOG.md", "# changelog\n")
    v = qa.check_process_framework_self()
    assert v.status == "CONFIRMED"


def test_check_process_framework_self_not_found_when_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "process" / "VERSION", "1.0\n")
    v = qa.check_process_framework_self()
    assert v.status == "NOT FOUND"


# ---------------------------------------------------------------------------
# check_devops
# ---------------------------------------------------------------------------


def test_check_devops_confirmed(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    workflows = tmp_path / ".github" / "workflows"
    workflows.mkdir(parents=True)
    write(workflows / "ci.yml", "name: ci\n")
    write(tmp_path / "setup.sh", "#!/bin/sh\n")
    write(tmp_path / "setup.ps1", "# ps1\n")
    v = qa.check_devops()
    assert v.status == "CONFIRMED"


def test_check_devops_partial_when_no_workflows_dir(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "setup.sh", "#!/bin/sh\n")
    write(tmp_path / "setup.ps1", "# ps1\n")
    v = qa.check_devops()
    assert v.status == "PARTIAL"


def test_check_devops_partial_when_setup_ps1_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    workflows = tmp_path / ".github" / "workflows"
    workflows.mkdir(parents=True)
    write(workflows / "ci.yml", "name: ci\n")
    write(tmp_path / "setup.sh", "#!/bin/sh\n")
    v = qa.check_devops()
    assert v.status == "PARTIAL"


# ---------------------------------------------------------------------------
# check_application_changelog
# ---------------------------------------------------------------------------


def test_check_application_changelog_confirmed(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    write(tmp_path / "CHANGELOG.md", "# changelog\n")
    v = qa.check_application_changelog()
    assert v.status == "CONFIRMED"


def test_check_application_changelog_not_found(tmp_path, monkeypatch):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    v = qa.check_application_changelog()
    assert v.status == "NOT FOUND"
    assert "RISK-003" in v.evidence[0]


# ---------------------------------------------------------------------------
# render()
# ---------------------------------------------------------------------------


def test_render_no_hard_fail_when_swe456_confirmed():
    verdicts = [
        qa.Verdict("SWE.4/5/6 Verification & Qualification", "CONFIRMED", ["ok"]),
        qa.Verdict("SUP.1 Quality Assurance", "CONFIRMED", ["ok"]),
    ]
    out = qa.render(verdicts)
    assert "No hard fail" in out
    assert "HARD FAIL" not in out.split("No hard fail")[0]


def test_render_hard_fail_when_swe456_not_found():
    verdicts = [
        qa.Verdict("SWE.4/5/6 Verification & Qualification", "NOT FOUND", ["missing"]),
    ]
    out = qa.render(verdicts)
    assert "**HARD FAIL: test evidence is missing or stale.**" in out


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def _stub_all_checks(monkeypatch, swe456_status="CONFIRMED"):
    monkeypatch.setattr(qa, "check_sup1", lambda: qa.Verdict("SUP.1 Quality Assurance", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_swe1", lambda: qa.Verdict("SWE.1 Requirements Elicitation", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_swe2_swe3", lambda: qa.Verdict("SWE.2/SWE.3 Architecture & Detailed Design", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_swe4_5_6", lambda: qa.Verdict("SWE.4/5/6 Verification & Qualification", swe456_status, ["e"]))
    monkeypatch.setattr(qa, "check_sup8", lambda: qa.Verdict("SUP.8 Configuration Management", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_sup9_10", lambda: qa.Verdict("SUP.9/SUP.10 Problem & Change Management", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_man5", lambda: qa.Verdict("MAN.5 Risk Management", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_process_framework_self", lambda: qa.Verdict("Process Framework (self, SUP.8-style)", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_devops", lambda: qa.Verdict("DevOps Principles", "CONFIRMED", ["e"]))
    monkeypatch.setattr(qa, "check_application_changelog", lambda: qa.Verdict("MAN.3 Project Management (planning artefact)", "CONFIRMED", ["e"]))


def test_main_writes_report_and_returns_0_when_all_confirmed(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    _stub_all_checks(monkeypatch)
    out_path = tmp_path / "downloads" / "qa-report.md"
    monkeypatch.setattr("sys.argv", ["qa_report.py", "--out", str(out_path)])

    rc = qa.main()
    out = capsys.readouterr().out
    assert rc == 0
    assert out_path.is_file()
    assert "wrote" in out
    assert "CONFIRMED" in out


def test_main_returns_1_and_warns_on_swe456_hard_fail(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(qa, "ROOT", tmp_path)
    _stub_all_checks(monkeypatch, swe456_status="NOT FOUND")
    out_path = tmp_path / "downloads" / "qa-report.md"
    monkeypatch.setattr("sys.argv", ["qa_report.py", "--out", str(out_path)])

    rc = qa.main()
    captured = capsys.readouterr()
    assert rc == 1
    assert "QA HARD FAIL" in captured.err
    text = out_path.read_text(encoding="utf-8")
    assert "HARD FAIL: test evidence is missing or stale." in text
