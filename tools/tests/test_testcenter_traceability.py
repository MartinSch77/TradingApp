# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/testcenter_traceability.py — exports
requirements/requirements.sdoc as a Squish Test Center generic-traceability
CSV.

git calls (repo_web_url/default_branch) are stubbed out — these tests never
invoke a real `git` subprocess — and ROOT/SDOC/DEFAULT_OUT are monkeypatched
to a tmp_path fixture tree so nothing here depends on this repository's own
requirements.
"""

import io
import subprocess

import pytest

import testcenter_traceability as tct


def write_sdoc(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


ONE_REQ_SDOC = (
    "[REQUIREMENT]\n"
    "UID: REQ-F-001\n"
    "TITLE: Do the thing\n"
    "VERIFICATION: T\n"
)

TWO_REQ_SDOC = ONE_REQ_SDOC + (
    "[REQUIREMENT]\n"
    "UID: REQ-N-002\n"
    "TITLE: Non-functional thing\n"
    "VERIFICATION: A\n"
)


# ---------------------------------------------------------------------------
# Requirement.name
# ---------------------------------------------------------------------------


def test_requirement_name_format():
    req = tct.Requirement("REQ-F-001", "Do the thing", "T")
    assert req.name == "REQ-F-001 [T] Do the thing"


# ---------------------------------------------------------------------------
# parse_requirements
# ---------------------------------------------------------------------------


def test_parse_requirements_reads_full_block(tmp_path):
    path = tmp_path / "requirements.sdoc"
    write_sdoc(path, ONE_REQ_SDOC)
    reqs = tct.parse_requirements(path)
    assert len(reqs) == 1
    assert reqs[0].uid == "REQ-F-001"
    assert reqs[0].title == "Do the thing"
    assert reqs[0].verification == "T"


def test_parse_requirements_multiple_blocks(tmp_path):
    path = tmp_path / "requirements.sdoc"
    write_sdoc(path, TWO_REQ_SDOC)
    reqs = tct.parse_requirements(path)
    assert [r.uid for r in reqs] == ["REQ-F-001", "REQ-N-002"]


def test_parse_requirements_block_ended_by_other_bracket(tmp_path):
    # A [TEXT] section right after [REQUIREMENT] but before VERIFICATION ends
    # the block, so an incomplete requirement is silently skipped.
    path = tmp_path / "requirements.sdoc"
    write_sdoc(
        path,
        "[REQUIREMENT]\n"
        "UID: REQ-F-001\n"
        "TITLE: Do the thing\n"
        "[TEXT]\n"
        "some free text\n" + ONE_REQ_SDOC.replace("REQ-F-001", "REQ-F-002"),
    )
    reqs = tct.parse_requirements(path)
    assert [r.uid for r in reqs] == ["REQ-F-002"]


def test_parse_requirements_no_blocks_at_all(tmp_path):
    path = tmp_path / "requirements.sdoc"
    write_sdoc(path, "just prose, no [REQUIREMENT] blocks\n")
    assert tct.parse_requirements(path) == []


# ---------------------------------------------------------------------------
# repo_web_url
# ---------------------------------------------------------------------------


def _fake_run(stdout="", returncode=0, raise_exc=None):
    def _run(cmd, **kwargs):
        if raise_exc is not None:
            raise raise_exc
        return subprocess.CompletedProcess(cmd, returncode, stdout=stdout, stderr="")

    return _run


def test_repo_web_url_converts_ssh_remote(monkeypatch):
    monkeypatch.setattr(
        tct.subprocess, "run", _fake_run("git@github.com:acme/repo.git\n")
    )
    assert tct.repo_web_url() == "https://github.com/acme/repo"


def test_repo_web_url_keeps_https_remote(monkeypatch):
    monkeypatch.setattr(
        tct.subprocess, "run", _fake_run("https://github.com/acme/repo.git\n")
    )
    assert tct.repo_web_url() == "https://github.com/acme/repo"


def test_repo_web_url_empty_stdout_is_none(monkeypatch):
    monkeypatch.setattr(tct.subprocess, "run", _fake_run(""))
    assert tct.repo_web_url() is None


def test_repo_web_url_non_http_non_ssh_is_none(monkeypatch):
    monkeypatch.setattr(tct.subprocess, "run", _fake_run("some/local/path\n"))
    assert tct.repo_web_url() is None


def test_repo_web_url_subprocess_failure_is_none(monkeypatch):
    monkeypatch.setattr(
        tct.subprocess, "run",
        _fake_run(raise_exc=subprocess.CalledProcessError(1, ["git"])),
    )
    assert tct.repo_web_url() is None


def test_repo_web_url_os_error_is_none(monkeypatch):
    monkeypatch.setattr(tct.subprocess, "run", _fake_run(raise_exc=OSError("no git")))
    assert tct.repo_web_url() is None


# ---------------------------------------------------------------------------
# default_branch
# ---------------------------------------------------------------------------


def test_default_branch_normal(monkeypatch):
    monkeypatch.setattr(tct.subprocess, "run", _fake_run("feature/x\n"))
    assert tct.default_branch() == "feature/x"


def test_default_branch_detached_head_falls_back_to_main(monkeypatch):
    monkeypatch.setattr(tct.subprocess, "run", _fake_run("HEAD\n"))
    assert tct.default_branch() == "main"


def test_default_branch_empty_falls_back_to_main(monkeypatch):
    monkeypatch.setattr(tct.subprocess, "run", _fake_run(""))
    assert tct.default_branch() == "main"


def test_default_branch_subprocess_failure_falls_back_to_main(monkeypatch):
    monkeypatch.setattr(
        tct.subprocess, "run",
        _fake_run(raise_exc=subprocess.TimeoutExpired(["git"], 10)),
    )
    assert tct.default_branch() == "main"


# ---------------------------------------------------------------------------
# uri_for
# ---------------------------------------------------------------------------


def test_uri_for_with_base():
    req = tct.Requirement("REQ-F-001", "Do the thing", "T")
    assert tct.uri_for(req, "https://github.com/acme/repo", "main") == (
        "https://github.com/acme/repo/blob/main/docs/requirements.md#req-f-001"
    )


def test_uri_for_without_base_is_relative_path():
    req = tct.Requirement("REQ-F-001", "Do the thing", "T")
    assert tct.uri_for(req, None, "main") == "docs/requirements.md#req-f-001"


# ---------------------------------------------------------------------------
# write_csv
# ---------------------------------------------------------------------------


def test_write_csv_rows(tmp_path):
    reqs = [tct.Requirement("REQ-F-001", "Do the thing", "T")]
    stream = io.StringIO()
    tct.write_csv(reqs, stream, "https://github.com/acme/repo", "main")
    text = stream.getvalue()
    lines = text.splitlines()
    assert lines[0] == "id,name,uri,project"
    assert lines[1] == (
        "REQ-F-001,REQ-F-001 [T] Do the thing,"
        "https://github.com/acme/repo/blob/main/docs/requirements.md#req-f-001,SRS-TRADINGAPP"
    )


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def _wire(tmp_path, monkeypatch, argv):
    monkeypatch.setattr(tct, "ROOT", tmp_path)
    monkeypatch.setattr(tct, "SDOC", tmp_path / "requirements" / "requirements.sdoc")
    monkeypatch.setattr(tct, "DEFAULT_OUT", tmp_path / "test-results" / "testcenter-traceability.csv")
    monkeypatch.setattr(tct.subprocess, "run", _fake_run("https://github.com/acme/repo.git\n"))
    monkeypatch.setattr("sys.argv", ["testcenter_traceability.py"] + argv)


def test_main_missing_sdoc_returns_1(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, [])
    rc = tct.main()
    assert rc == 1
    assert "requirements file not found" in capsys.readouterr().err


def test_main_no_requirement_blocks_returns_1(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, [])
    write_sdoc(tct.SDOC, "no requirements here\n")
    rc = tct.main()
    assert rc == 1
    assert "refusing to write an empty traceability file" in capsys.readouterr().err


def test_main_duplicate_uids_returns_1(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, [])
    write_sdoc(tct.SDOC, ONE_REQ_SDOC + ONE_REQ_SDOC)
    rc = tct.main()
    assert rc == 1
    assert "duplicate requirement ids" in capsys.readouterr().err


def test_main_check_flag_writes_nothing(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, ["--check"])
    write_sdoc(tct.SDOC, ONE_REQ_SDOC)
    rc = tct.main()
    assert rc == 0
    assert "check only" in capsys.readouterr().err
    assert not tct.DEFAULT_OUT.exists()


def test_main_stdout_flag_prints_csv(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, ["--stdout"])
    write_sdoc(tct.SDOC, ONE_REQ_SDOC)
    rc = tct.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert captured.out.splitlines()[0] == "id,name,uri,project"
    assert "REQ-F-001" in captured.out
    # progress notes go to stderr, never mixed into the CSV on stdout
    assert "REQ-F-001" not in captured.err
    assert not tct.DEFAULT_OUT.exists()


def test_main_default_writes_csv_file(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, [])
    write_sdoc(tct.SDOC, TWO_REQ_SDOC)
    rc = tct.main()
    assert rc == 0
    assert tct.DEFAULT_OUT.is_file()
    text = tct.DEFAULT_OUT.read_text(encoding="utf-8")
    assert "REQ-F-001" in text
    assert "REQ-N-002" in text
    err = capsys.readouterr().err
    assert "requirements: 2 (1 functional, 1 non-functional)" in err
    assert "wrote" in err


def test_main_custom_out_path(tmp_path, monkeypatch):
    _wire(tmp_path, monkeypatch, [])
    write_sdoc(tct.SDOC, ONE_REQ_SDOC)
    custom = tmp_path / "elsewhere" / "trace.csv"
    monkeypatch.setattr("sys.argv", ["testcenter_traceability.py", "--out", str(custom)])
    rc = tct.main()
    assert rc == 0
    assert custom.is_file()
