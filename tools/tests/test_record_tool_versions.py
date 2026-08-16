# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/record_tool_versions.py — records installed tool
versions into analysis-results/tool-versions.json for the quality report.

shutil.which and subprocess.run are stubbed so these tests never depend on
what is actually installed on the machine running the suite.
"""

import json
import subprocess

import record_tool_versions as rtv


# ---------------------------------------------------------------------------
# first_line()
# ---------------------------------------------------------------------------


def test_first_line_not_installed_when_which_fails(monkeypatch):
    monkeypatch.setattr(rtv.shutil, "which", lambda name: None)
    assert rtv.first_line(["nosuchtool", "--version"]) == "not installed"


def test_first_line_reads_stdout_first_line(monkeypatch):
    monkeypatch.setattr(rtv.shutil, "which", lambda name: "/usr/bin/cppcheck")

    def fake_run(cmd, **kwargs):
        return subprocess.CompletedProcess(cmd, 0, stdout="Cppcheck 2.13.0\nextra\n", stderr="")

    monkeypatch.setattr(rtv.subprocess, "run", fake_run)
    assert rtv.first_line(["cppcheck", "--version"]) == "Cppcheck 2.13.0"


def test_first_line_falls_back_to_stderr(monkeypatch):
    monkeypatch.setattr(rtv.shutil, "which", lambda name: "/usr/bin/foo")

    def fake_run(cmd, **kwargs):
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="  foo version 1.0  \n")

    monkeypatch.setattr(rtv.subprocess, "run", fake_run)
    assert rtv.first_line(["foo", "--version"]) == "foo version 1.0"


def test_first_line_unknown_when_no_output(monkeypatch):
    monkeypatch.setattr(rtv.shutil, "which", lambda name: "/usr/bin/foo")

    def fake_run(cmd, **kwargs):
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    monkeypatch.setattr(rtv.subprocess, "run", fake_run)
    assert rtv.first_line(["foo", "--version"]) == "unknown"


def test_first_line_not_installed_on_subprocess_error(monkeypatch):
    monkeypatch.setattr(rtv.shutil, "which", lambda name: "/usr/bin/foo")

    def fake_run(cmd, **kwargs):
        raise subprocess.SubprocessError("boom")

    monkeypatch.setattr(rtv.subprocess, "run", fake_run)
    assert rtv.first_line(["foo", "--version"]) == "not installed"


def test_first_line_not_installed_on_os_error(monkeypatch):
    monkeypatch.setattr(rtv.shutil, "which", lambda name: "/usr/bin/foo")

    def fake_run(cmd, **kwargs):
        raise OSError("boom")

    monkeypatch.setattr(rtv.subprocess, "run", fake_run)
    assert rtv.first_line(["foo", "--version"]) == "not installed"


# ---------------------------------------------------------------------------
# pmd_version()
# ---------------------------------------------------------------------------


def test_pmd_version_not_installed_when_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(rtv, "ROOT", tmp_path)
    (tmp_path / "tools" / "third-party").mkdir(parents=True)
    assert rtv.pmd_version() == "not installed"


def test_pmd_version_picks_newest_of_several(tmp_path, monkeypatch):
    monkeypatch.setattr(rtv, "ROOT", tmp_path)
    third_party = tmp_path / "tools" / "third-party"
    third_party.mkdir(parents=True)
    (third_party / "pmd-bin-6.55.0").mkdir()
    (third_party / "pmd-bin-7.0.0").mkdir()
    assert rtv.pmd_version() == "7.0.0"


def test_pmd_version_no_third_party_dir_at_all(tmp_path, monkeypatch):
    monkeypatch.setattr(rtv, "ROOT", tmp_path)
    assert rtv.pmd_version() == "not installed"


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def test_main_writes_json_with_all_tools_and_extras(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(rtv, "ROOT", tmp_path)
    monkeypatch.setattr(rtv.shutil, "which", lambda name: None)  # nothing installed
    monkeypatch.setattr("sys.argv", ["record_tool_versions.py"])

    rc = rtv.main()
    out = capsys.readouterr().out
    assert rc == 0

    target = tmp_path / "analysis-results" / "tool-versions.json"
    assert target.is_file()
    payload = json.loads(target.read_text(encoding="utf-8"))
    names = [t["tool"] for t in payload["tools"]]
    # every declared external tool, plus the two extras appended by hand
    assert names[: len(rtv.TOOLS)] == [label for label, *_ in rtv.TOOLS]
    assert "PMD CPD" in names
    assert "objectName check" in names
    # every real TOOLS entry is "not installed" (which() stubbed to None); PMD CPD
    # is likewise absent; "objectName check" isn't a probed tool at all — its
    # "version" field is a fixed description, always present.
    assert all(t["version"] == "not installed" for t in payload["tools"]
               if t["tool"] not in ("PMD CPD", "objectName check"))
    pmd_entry = next(t for t in payload["tools"] if t["tool"] == "PMD CPD")
    assert pmd_entry["version"] == "not installed"
    # "objectName check"'s version field is a fixed description, never "not
    # installed", so it alone counts as "installed" here.
    assert f"1 of {len(names)} tools recorded" in out


def test_main_custom_output_directory_via_argv(tmp_path, monkeypatch):
    monkeypatch.setattr(rtv, "ROOT", tmp_path)
    monkeypatch.setattr(rtv.shutil, "which", lambda name: None)
    monkeypatch.setattr("sys.argv", ["record_tool_versions.py", "custom-analysis-dir"])

    rtv.main()
    assert (tmp_path / "custom-analysis-dir" / "tool-versions.json").is_file()


def test_main_counts_installed_tools(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(rtv, "ROOT", tmp_path)
    monkeypatch.setattr(rtv.shutil, "which", lambda name: f"/usr/bin/{name}")

    def fake_run(cmd, **kwargs):
        return subprocess.CompletedProcess(cmd, 0, stdout="1.0.0\n", stderr="")

    monkeypatch.setattr(rtv.subprocess, "run", fake_run)
    monkeypatch.setattr("sys.argv", ["record_tool_versions.py"])

    rtv.main()
    out = capsys.readouterr().out
    # all TOOLS installed + "objectName check" (fixed non-"not installed"
    # description) = len(TOOLS) + 1; PMD CPD alone stays "not installed"
    # (no tools/third-party dir in this tmp_path fixture).
    assert f"{len(rtv.TOOLS) + 1} of {len(rtv.TOOLS) + 2} tools recorded" in out
