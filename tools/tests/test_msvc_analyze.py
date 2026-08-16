# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/msvc_analyze.py.

The DIAG regex and the src-dir filter are both Windows-drive-letter-shaped
(`C:\\path\\file.cpp(120,9): warning C6011: ...`) and the module does its
path filtering with plain string prefix matching (no filesystem access), so
these are exercised with literal Windows-style strings even when the test
host is Linux — os.path.join/normpath on POSIX leave a backslash-containing
string untouched aside from '/' collapsing, so a consistently-styled ROOT/
src_dir/file triple behaves the same on either platform.
"""

import json
import subprocess

import pytest

import msvc_analyze as ma


# --------------------------------------------------------------------------
# split_command
# --------------------------------------------------------------------------

def test_split_command_uses_arguments_list_verbatim():
    entry = {"arguments": ["cl.exe", "/c", "file.cpp"]}
    assert ma.split_command(entry) == ["cl.exe", "/c", "file.cpp"]


def test_split_command_parses_quoted_command_string():
    entry = {"command": 'cl.exe /c "C:\\Program Files\\x\\file.cpp" /DFOO'}
    result = ma.split_command(entry)
    assert "C:\\Program Files\\x\\file.cpp" in result
    assert "/DFOO" in result
    assert "cl.exe" in result


# --------------------------------------------------------------------------
# run
# --------------------------------------------------------------------------

def test_run_timeout(monkeypatch):
    def fake_run(*a, **k):
        raise subprocess.TimeoutExpired(cmd="x", timeout=1)

    monkeypatch.setattr(ma.subprocess, "run", fake_run)
    entry = {"file": "C:\\proj\\src\\a.cpp", "directory": "C:\\proj",
             "arguments": ["cl.exe", "a.cpp"]}
    result = ma.run(entry, "C:\\proj")
    assert "msvc-analyze-timeout" in result[0]


def test_run_oserror(monkeypatch):
    def fake_run(*a, **k):
        raise OSError("cl.exe not found")

    monkeypatch.setattr(ma.subprocess, "run", fake_run)
    entry = {"file": "C:\\proj\\src\\a.cpp", "directory": "C:\\proj",
             "arguments": ["cl.exe", "a.cpp"]}
    result = ma.run(entry, "C:\\proj")
    assert "msvc-analyze-error" in result[0]


def test_run_drops_forbidden_flags_before_invoking(monkeypatch):
    captured_args = {}

    class Result:
        stdout = ""
        stderr = ""

    def fake_run(args, **kwargs):
        captured_args["args"] = args
        return Result()

    monkeypatch.setattr(ma.subprocess, "run", fake_run)
    entry = {
        "file": "C:\\proj\\src\\a.cpp",
        "directory": "C:\\proj",
        "arguments": ["cl.exe", "/c", "/showIncludes", "/Fosomething.obj",
                      "/MP4", "/DFOO", "a.cpp"],
    }
    ma.run(entry, "C:\\proj")
    args = captured_args["args"]
    assert "/c" not in args[:args.index("/analyze")]
    assert "/showIncludes" not in args
    assert not any(a.startswith("/Fo") and a != "/Fo" + ma.os.devnull for a in args)
    assert not any(a.startswith("/MP") for a in args)
    assert "/DFOO" in args
    assert "/analyze" in args
    assert "/analyze:only" in args
    assert "/analyze:WX-" in args


def test_run_filters_to_src_dir_and_keeps_matching(monkeypatch):
    root = "C:\\proj"
    # Built exactly the way msvc_analyze.run() builds src_dir itself, so the
    # prefix comparison (plain string startswith — no real filesystem access)
    # actually matches on a POSIX test host too.
    src_dir = ma.os.path.join(root, "src") + ma.os.sep
    in_file = src_dir + "a.cpp"
    out_file = ma.os.path.join(root, "other") + ma.os.sep + "b.cpp"
    stdout = (
        f"{in_file}(10,3): warning C6011: dereferencing NULL pointer\n"
        f"{out_file}(3,1): warning C6001: uninitialized memory\n"
        "not a diagnostic\n"
    )

    class Result:
        pass

    result = Result()
    result.stdout = stdout
    result.stderr = ""
    monkeypatch.setattr(ma.subprocess, "run", lambda *a, **k: result)
    entry = {"file": in_file, "directory": root, "arguments": ["cl.exe", in_file]}
    kept = ma.run(entry, root)
    assert len(kept) == 1
    assert "C6011" in kept[0]
    assert in_file in kept[0]


def test_run_diag_default_col_when_missing(monkeypatch):
    root = "C:\\proj"
    src_dir = ma.os.path.join(root, "src") + ma.os.sep
    in_file = src_dir + "a.cpp"
    stdout = f"{in_file}(10): warning C6011: no column given\n"

    class Result:
        pass

    result = Result()
    result.stdout = stdout
    result.stderr = ""
    monkeypatch.setattr(ma.subprocess, "run", lambda *a, **k: result)
    entry = {"file": in_file, "directory": root, "arguments": ["cl.exe", in_file]}
    kept = ma.run(entry, root)
    assert kept[0].startswith(f"{in_file}:10:1:")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def _write_db(tmp_path, entries):
    db_path = tmp_path / "compile_commands.json"
    db_path.write_text(json.dumps(entries), encoding="utf-8")
    return db_path


def test_main_wrong_args_exits(monkeypatch):
    monkeypatch.setattr("sys.argv", ["msvc_analyze.py", "a", "b"])
    with pytest.raises(SystemExit):
        ma.main()


def test_main_filters_entries_by_src_dir_and_runs(tmp_path, monkeypatch, capsys):
    root = tmp_path
    src_file = root / "src" / "a.cpp"
    other_file = root / "other" / "b.cpp"
    (root / "src").mkdir(parents=True)
    (root / "other").mkdir(parents=True)
    db_path = _write_db(tmp_path, [
        {"file": str(src_file), "directory": str(root), "arguments": ["cl.exe", str(src_file)]},
        {"file": str(other_file), "directory": str(root), "arguments": ["cl.exe", str(other_file)]},
    ])
    out_path = tmp_path / "msvc-analyze.txt"

    seen_entries = []

    def fake_run(entry, root_):
        seen_entries.append(entry["file"])
        return ["fake-finding"]

    monkeypatch.setattr(ma, "run", fake_run)
    monkeypatch.setattr("sys.argv",
                        ["msvc_analyze.py", str(db_path), str(root), str(out_path)])
    ma.main()

    # only the src/ entry should have been run
    assert seen_entries == [str(src_file)]
    assert out_path.read_text().strip() == "fake-finding"
    captured = capsys.readouterr()
    assert "1 findings over 1 TUs" in captured.out


def test_main_no_matching_entries_writes_empty(tmp_path, monkeypatch, capsys):
    root = tmp_path
    (root / "src").mkdir(parents=True)
    other_file = root / "other" / "b.cpp"
    (root / "other").mkdir(parents=True)
    db_path = _write_db(tmp_path, [
        {"file": str(other_file), "directory": str(root), "arguments": ["cl.exe", str(other_file)]},
    ])
    out_path = tmp_path / "msvc-analyze.txt"
    monkeypatch.setattr("sys.argv",
                        ["msvc_analyze.py", str(db_path), str(root), str(out_path)])
    ma.main()
    assert out_path.read_text() == ""
    captured = capsys.readouterr()
    assert "0 findings over 0 TUs" in captured.out
