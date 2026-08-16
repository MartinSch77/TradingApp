# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/cpd_scan.py."""

import pytest

import cpd_scan


# --------------------------------------------------------------------------
# _pmd_executable
# --------------------------------------------------------------------------

def test_pmd_executable_via_pmd_home(tmp_path, monkeypatch):
    home = tmp_path / "pmdhome"
    bin_dir = home / "bin"
    bin_dir.mkdir(parents=True)
    pmd = bin_dir / "pmd"
    pmd.write_text("", encoding="utf-8")
    monkeypatch.setenv("PMD_HOME", str(home))
    result = cpd_scan._pmd_executable(tmp_path)
    assert result == pmd


def test_pmd_executable_pmd_home_missing_file_falls_through(tmp_path, monkeypatch):
    monkeypatch.setenv("PMD_HOME", str(tmp_path / "nonexistent"))
    monkeypatch.setattr(cpd_scan.shutil, "which", lambda name: None)
    result = cpd_scan._pmd_executable(tmp_path)
    assert result is None


def test_pmd_executable_bundled_third_party(tmp_path, monkeypatch):
    monkeypatch.delenv("PMD_HOME", raising=False)
    third = tmp_path / "tools" / "third-party" / "pmd-bin-7.5.0" / "bin"
    third.mkdir(parents=True)
    pmd = third / "pmd"
    pmd.write_text("", encoding="utf-8")
    result = cpd_scan._pmd_executable(tmp_path)
    assert result == pmd


def test_pmd_executable_picks_highest_version_when_multiple(tmp_path, monkeypatch):
    monkeypatch.delenv("PMD_HOME", raising=False)
    for version in ("7.1.0", "7.9.0"):
        d = tmp_path / "tools" / "third-party" / f"pmd-bin-{version}" / "bin"
        d.mkdir(parents=True)
        (d / "pmd").write_text("", encoding="utf-8")
    result = cpd_scan._pmd_executable(tmp_path)
    assert "7.9.0" in str(result)


def test_pmd_executable_falls_back_to_path(tmp_path, monkeypatch):
    monkeypatch.delenv("PMD_HOME", raising=False)
    monkeypatch.setattr(cpd_scan.shutil, "which", lambda name: "/usr/bin/pmd")
    result = cpd_scan._pmd_executable(tmp_path)
    assert result == cpd_scan.Path("/usr/bin/pmd")


def test_pmd_executable_none_found(tmp_path, monkeypatch):
    monkeypatch.delenv("PMD_HOME", raising=False)
    monkeypatch.setattr(cpd_scan.shutil, "which", lambda name: None)
    assert cpd_scan._pmd_executable(tmp_path) is None


# --------------------------------------------------------------------------
# _run_cpd
# --------------------------------------------------------------------------

def test_run_cpd_success_returns_stdout(tmp_path, monkeypatch):
    (tmp_path / "src").mkdir()

    class Result:
        stdout = "csv,output\n"
        stderr = ""
        returncode = 0

    monkeypatch.setattr(cpd_scan.subprocess, "run", lambda *a, **k: Result())
    out = cpd_scan._run_cpd(cpd_scan.Path("/bin/pmd"), tmp_path, 100)
    assert out == "csv,output\n"


def test_run_cpd_nonzero_raises(tmp_path, monkeypatch):
    class Result:
        stdout = ""
        stderr = "explosion"
        returncode = 2

    monkeypatch.setattr(cpd_scan.subprocess, "run", lambda *a, **k: Result())
    with pytest.raises(SystemExit, match="pmd cpd failed"):
        cpd_scan._run_cpd(cpd_scan.Path("/bin/pmd"), tmp_path, 100)


# --------------------------------------------------------------------------
# _rows
# --------------------------------------------------------------------------

def test_rows_parses_two_occurrence_clone(tmp_path):
    root = tmp_path
    fileA = root / "src" / "a.cpp"
    fileB = root / "src" / "b.cpp"
    csv_text = (
        "lines,tokens,occurrences,firstFileStartLine,firstFileName,"
        "secondFileStartLine,secondFileName\n"
        f"12,150,2,10,{fileA},20,{fileB}\n"
    )
    findings, clones = cpd_scan._rows(csv_text, root)
    assert clones == 1
    assert len(findings) == 2
    assert "src/a.cpp|10|warning|pmd-cpd-duplicate|" in findings[0]
    assert "src/b.cpp:20" in findings[0]
    assert "src/b.cpp|20|warning|pmd-cpd-duplicate|" in findings[1]
    assert "src/a.cpp:10" in findings[1]


def test_rows_skips_header_and_short_records(tmp_path):
    csv_text = "lines,tokens,occurrences,line,file\nnotdigit,x,y,z,w\n"
    findings, clones = cpd_scan._rows(csv_text, tmp_path)
    assert findings == []
    assert clones == 0


def test_rows_file_outside_root_falls_back_to_raw_path(tmp_path):
    root = tmp_path / "proj"
    root.mkdir()
    outside = tmp_path / "elsewhere" / "c.cpp"
    csv_text = f"5,110,2,1,{outside},2,{outside}\n"
    findings, clones = cpd_scan._rows(csv_text, root)
    assert clones == 1
    assert any("elsewhere" in f for f in findings)


def test_rows_three_occurrence_clone_lists_the_other_two(tmp_path):
    root = tmp_path
    files = [root / "src" / n for n in ("a.cpp", "b.cpp", "c.cpp")]
    csv_text = f"8,120,3,1,{files[0]},2,{files[1]},3,{files[2]}\n"
    findings, clones = cpd_scan._rows(csv_text, root)
    assert clones == 1
    assert len(findings) == 3
    # each finding names the OTHER two occurrences, not itself
    assert "src/a.cpp:1" not in findings[0]
    assert "src/b.cpp:2" in findings[0] and "src/c.cpp:3" in findings[0]


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def test_main_wrong_args_exits(monkeypatch):
    monkeypatch.setattr("sys.argv", ["cpd_scan.py", "onlyone"])
    with pytest.raises(SystemExit):
        cpd_scan.main()


def test_main_pmd_not_installed_exits_3(tmp_path, monkeypatch, capsys):
    out_path = tmp_path / "pmd-cpd.txt"
    monkeypatch.setattr(cpd_scan, "_pmd_executable", lambda root: None)
    monkeypatch.setattr("sys.argv", ["cpd_scan.py", str(tmp_path), str(out_path)])
    rc = cpd_scan.main()
    assert rc == 3
    assert out_path.read_text() == ""


def test_main_no_java_exits_3(tmp_path, monkeypatch, capsys):
    out_path = tmp_path / "pmd-cpd.txt"
    monkeypatch.setattr(cpd_scan, "_pmd_executable", lambda root: cpd_scan.Path("/bin/pmd"))
    monkeypatch.setattr(cpd_scan.shutil, "which", lambda name: None)
    monkeypatch.setattr("sys.argv", ["cpd_scan.py", str(tmp_path), str(out_path)])
    rc = cpd_scan.main()
    assert rc == 3
    assert out_path.read_text() == ""


def test_main_full_run_writes_findings(tmp_path, monkeypatch, capsys):
    (tmp_path / "src").mkdir()
    out_path = tmp_path / "pmd-cpd.txt"
    fileA = tmp_path / "src" / "a.cpp"
    fileB = tmp_path / "src" / "b.cpp"
    csv_out = f"10,120,2,1,{fileA},2,{fileB}\n"

    monkeypatch.setattr(cpd_scan, "_pmd_executable", lambda root: cpd_scan.Path("/bin/pmd"))
    monkeypatch.setattr(cpd_scan.shutil, "which",
                        lambda name: "/usr/bin/java" if name == "java" else None)
    monkeypatch.setattr(cpd_scan, "_run_cpd", lambda pmd, root, min_tokens: csv_out)
    monkeypatch.setattr("sys.argv", ["cpd_scan.py", str(tmp_path), str(out_path)])

    rc = cpd_scan.main()
    assert rc == 0
    text = out_path.read_text()
    assert "src/a.cpp|1|warning|pmd-cpd-duplicate" in text
    captured = capsys.readouterr()
    assert "1 duplicated blocks" in captured.out


def test_main_min_tokens_flag_after_positionals_is_read(tmp_path, monkeypatch):
    """The documented usage (`cpd_scan.py <root> <out> --min-tokens N`) must work:
    main() now consumes --min-tokens' OWN value as it scans argv, rather than
    only filtering tokens starting with "--" and leaving the value to be
    miscounted as a third positional (the bug tools/tests/test_cpd_scan.py
    found; fixed in tools/cpd_scan.py)."""
    out_path = tmp_path / "pmd-cpd.txt"
    seen = {}

    def fake_run_cpd(pmd, root, min_tokens):
        seen["min_tokens"] = min_tokens
        return ""

    monkeypatch.setattr(cpd_scan, "_pmd_executable", lambda root: cpd_scan.Path("/bin/pmd"))
    monkeypatch.setattr(cpd_scan.shutil, "which",
                        lambda name: "/usr/bin/java" if name == "java" else None)
    monkeypatch.setattr(cpd_scan, "_run_cpd", fake_run_cpd)
    monkeypatch.setattr("sys.argv",
                        ["cpd_scan.py", str(tmp_path), str(out_path), "--min-tokens", "50"])

    rc = cpd_scan.main()
    assert rc == 0
    assert seen["min_tokens"] == 50


def test_main_min_tokens_flag_before_positionals_is_read(tmp_path, monkeypatch):
    """--min-tokens works regardless of where it falls relative to the two
    positionals, since main() now scans and consumes it wherever it appears."""
    out_path = tmp_path / "pmd-cpd.txt"
    seen = {}

    def fake_run_cpd(pmd, root, min_tokens):
        seen["min_tokens"] = min_tokens
        return ""

    monkeypatch.setattr(cpd_scan, "_pmd_executable", lambda root: cpd_scan.Path("/bin/pmd"))
    monkeypatch.setattr(cpd_scan.shutil, "which",
                        lambda name: "/usr/bin/java" if name == "java" else None)
    monkeypatch.setattr(cpd_scan, "_run_cpd", fake_run_cpd)
    monkeypatch.setattr("sys.argv",
                        ["cpd_scan.py", "--min-tokens", "100", str(tmp_path), str(out_path)])

    rc = cpd_scan.main()
    assert rc == 0
    assert seen["min_tokens"] == 100


def test_main_default_min_tokens_used_when_flag_absent(tmp_path, monkeypatch):
    (tmp_path / "src").mkdir()
    out_path = tmp_path / "pmd-cpd.txt"
    seen = {}

    def fake_run_cpd(pmd, root, min_tokens):
        seen["min_tokens"] = min_tokens
        return ""

    monkeypatch.setattr(cpd_scan, "_pmd_executable", lambda root: cpd_scan.Path("/bin/pmd"))
    monkeypatch.setattr(cpd_scan.shutil, "which",
                        lambda name: "/usr/bin/java" if name == "java" else None)
    monkeypatch.setattr(cpd_scan, "_run_cpd", fake_run_cpd)
    monkeypatch.setattr("sys.argv", ["cpd_scan.py", str(tmp_path), str(out_path)])

    rc = cpd_scan.main()
    assert rc == 0
    assert seen["min_tokens"] == cpd_scan.MIN_TOKENS
