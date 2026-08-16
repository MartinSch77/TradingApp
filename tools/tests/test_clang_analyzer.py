# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/clang_analyzer.py."""

import json
import subprocess

import pytest

import clang_analyzer as ca


# --------------------------------------------------------------------------
# _find_compiler
# --------------------------------------------------------------------------

def test_find_compiler_env_override_found(monkeypatch):
    monkeypatch.setenv("CLANG_ANALYZER_CXX", "myclang")
    monkeypatch.setattr(ca.shutil, "which", lambda name: "/usr/bin/myclang")
    assert ca._find_compiler("g++") == "/usr/bin/myclang"


def test_find_compiler_env_override_not_found_returns_literal(monkeypatch):
    monkeypatch.setenv("CLANG_ANALYZER_CXX", "myclang")
    monkeypatch.setattr(ca.shutil, "which", lambda name: None)
    assert ca._find_compiler("g++") == "myclang"


def test_find_compiler_msvc_style_prefers_clang_cl(monkeypatch):
    monkeypatch.delenv("CLANG_ANALYZER_CXX", raising=False)
    monkeypatch.setattr(ca.shutil, "which",
                        lambda name: "/usr/bin/clang-cl" if name == "clang-cl" else None)
    assert ca._find_compiler("cl.exe") == "/usr/bin/clang-cl"


def test_find_compiler_non_msvc_prefers_clang18(monkeypatch):
    monkeypatch.delenv("CLANG_ANALYZER_CXX", raising=False)
    def which(name):
        return "/usr/bin/clang++-18" if name == "clang++-18" else None
    monkeypatch.setattr(ca.shutil, "which", which)
    assert ca._find_compiler("g++") == "/usr/bin/clang++-18"


def test_find_compiler_falls_back_to_plain_clangxx(monkeypatch):
    monkeypatch.delenv("CLANG_ANALYZER_CXX", raising=False)
    def which(name):
        return "/usr/bin/clang++" if name == "clang++" else None
    monkeypatch.setattr(ca.shutil, "which", which)
    assert ca._find_compiler("g++") == "/usr/bin/clang++"


def test_find_compiler_none_found(monkeypatch):
    monkeypatch.delenv("CLANG_ANALYZER_CXX", raising=False)
    monkeypatch.setattr(ca.shutil, "which", lambda name: None)
    assert ca._find_compiler("g++") is None


# --------------------------------------------------------------------------
# _supports_z3
# --------------------------------------------------------------------------

def test_supports_z3_true(monkeypatch):
    class Result:
        returncode = 0
        stderr = ""

    monkeypatch.setattr(ca.subprocess, "run", lambda *a, **k: Result())
    assert ca._supports_z3("clang++") is True


def test_supports_z3_false_on_error_message(monkeypatch):
    class Result:
        returncode = 1
        stderr = "LLVM was not compiled with Z3 support"

    monkeypatch.setattr(ca.subprocess, "run", lambda *a, **k: Result())
    assert ca._supports_z3("clang++") is False


def test_supports_z3_false_when_z3_mentioned_even_if_rc_zero(monkeypatch):
    class Result:
        returncode = 0
        stderr = "warning: Z3 something"

    monkeypatch.setattr(ca.subprocess, "run", lambda *a, **k: Result())
    assert ca._supports_z3("clang++") is False


# --------------------------------------------------------------------------
# _analyzer_flags
# --------------------------------------------------------------------------

def test_analyzer_flags_without_z3():
    flags = ca._analyzer_flags(with_z3=False)
    assert "--analyze" in flags
    assert "crosscheck-with-z3=true" not in flags
    assert "-analyzer-checker=optin.cplusplus.UninitializedObject" in flags


def test_analyzer_flags_with_z3():
    flags = ca._analyzer_flags(with_z3=True)
    assert "crosscheck-with-z3=true" in flags


# --------------------------------------------------------------------------
# _tu_arguments
# --------------------------------------------------------------------------

def test_tu_arguments_strips_o_c_and_werror_from_arguments_list():
    entry = {"arguments": ["g++", "-c", "-o", "out.o", "-Werror", "-Werror=all",
                           "-DFOO=1", "-Ipath", "file.cpp"]}
    flags = ["--analyze"]
    result = ca._tu_arguments(entry, "clang++", flags)
    assert result[0] == "clang++"
    assert "--analyze" in result
    assert "-o" not in result[:len(result) - 1] or result[-2] == "-o"
    # -Werror variants removed, -c and its paired -o value removed
    assert "-Werror" not in result
    assert "-Werror=all" not in result
    assert "out.o" not in result
    assert "-DFOO=1" in result
    assert result[-2:] == ["-o", ca.os.devnull]


def test_tu_arguments_command_string_and_msvc_style_flags():
    entry = {"command": "cl.exe /c /Fo out.obj /WX /DFOO file.cpp"}
    result = ca._tu_arguments(entry, "clang-cl", [])
    assert "/c" not in result
    assert "/WX" not in result
    assert "out.obj" not in result
    assert "/DFOO" in result
    assert "file.cpp" in result


# --------------------------------------------------------------------------
# _run
# --------------------------------------------------------------------------

def test_run_timeout(monkeypatch):
    def fake_run(*a, **k):
        raise subprocess.TimeoutExpired(cmd="x", timeout=1)

    monkeypatch.setattr(ca.subprocess, "run", fake_run)
    entry = {"file": "src/a.cpp", "directory": "/tmp", "arguments": ["g++", "a.cpp"]}
    result = ca._run(entry, "clang++", [], "/root")
    assert "clang-analyzer-timeout" in result[0]


def test_run_nonzero_reports_failure(monkeypatch):
    class Result:
        returncode = 1
        stdout = ""
        stderr = "crash trace\nlast line here"

    monkeypatch.setattr(ca.subprocess, "run", lambda *a, **k: Result())
    entry = {"file": "src/a.cpp", "directory": "/tmp", "arguments": ["g++", "a.cpp"]}
    result = ca._run(entry, "clang++", [], "/root")
    assert "clang-analyzer-failed" in result[0]
    assert "last line here" in result[0]


def test_run_nonzero_no_stderr_reports_no_diagnostics(monkeypatch):
    class Result:
        returncode = 2
        stdout = ""
        stderr = ""

    monkeypatch.setattr(ca.subprocess, "run", lambda *a, **k: Result())
    entry = {"file": "src/a.cpp", "directory": "/tmp", "arguments": ["g++", "a.cpp"]}
    result = ca._run(entry, "clang++", [], "/root")
    assert "no diagnostics" in result[0]


def test_run_success_filters_by_root_prefix(monkeypatch, tmp_path):
    root = str(tmp_path)
    in_root = tmp_path / "src" / "a.cpp"
    in_root.parent.mkdir(parents=True)
    in_root.write_text("", encoding="utf-8")
    outside = "/somewhere/else/b.cpp"

    stderr = (
        f"{in_root}:10:2: warning: uninitialized value [core.CallAndMessage]\n"
        f"{outside}:5:1: warning: something [core.NullDereference]\n"
        "not a diagnostic line at all\n"
    )

    class Result:
        returncode = 0
        stdout = ""

    result_obj = Result()
    result_obj.stderr = stderr
    monkeypatch.setattr(ca.subprocess, "run", lambda *a, **k: result_obj)
    entry = {"file": str(in_root), "directory": str(tmp_path), "arguments": ["g++", str(in_root)]}
    kept = ca._run(entry, "clang++", [], root)
    assert len(kept) == 1
    assert str(in_root) in kept[0]


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def _write_db(tmp_path, entries):
    db_path = tmp_path / "compile_commands.json"
    db_path.write_text(json.dumps(entries), encoding="utf-8")
    return db_path


def test_main_wrong_args_exits(monkeypatch):
    monkeypatch.setattr("sys.argv", ["clang_analyzer.py", "a", "b"])
    with pytest.raises(SystemExit):
        ca.main()


def test_main_no_project_tus_returns_1(tmp_path, monkeypatch, capsys):
    root = tmp_path / "proj"
    (root / "src").mkdir(parents=True)
    db_path = _write_db(tmp_path, [
        {"file": "/outside/other.cpp", "directory": "/tmp", "arguments": ["g++", "other.cpp"]}
    ])
    out_path = tmp_path / "clang-analyzer.txt"
    monkeypatch.setattr("sys.argv",
                        ["clang_analyzer.py", str(db_path), str(root), str(out_path)])
    rc = ca.main()
    assert rc == 1


def test_main_no_compiler_found_exits_3(tmp_path, monkeypatch, capsys):
    root = tmp_path / "proj"
    (root / "src").mkdir(parents=True)
    src_file = root / "src" / "a.cpp"
    db_path = _write_db(tmp_path, [
        {"file": str(src_file), "directory": str(root), "arguments": ["g++", str(src_file)]}
    ])
    out_path = tmp_path / "clang-analyzer.txt"
    monkeypatch.setattr(ca, "_find_compiler", lambda first: None)
    monkeypatch.setattr("sys.argv",
                        ["clang_analyzer.py", str(db_path), str(root), str(out_path)])
    rc = ca.main()
    assert rc == 3
    assert out_path.read_text() == ""


def test_main_full_run_writes_sorted_findings(tmp_path, monkeypatch, capsys):
    root = tmp_path / "proj"
    (root / "src").mkdir(parents=True)
    src_file = root / "src" / "a.cpp"
    db_path = _write_db(tmp_path, [
        {"file": str(src_file), "directory": str(root), "arguments": ["g++", str(src_file)]}
    ])
    out_path = tmp_path / "clang-analyzer.txt"

    monkeypatch.setattr(ca, "_find_compiler", lambda first: "clang++")
    monkeypatch.setattr(ca, "_supports_z3", lambda compiler: False)
    monkeypatch.setattr(ca, "_run", lambda entry, compiler, flags, root_: ["finding-line-1"])
    monkeypatch.setattr("sys.argv",
                        ["clang_analyzer.py", str(db_path), str(root), str(out_path)])
    rc = ca.main()
    assert rc == 0
    assert out_path.read_text().strip() == "finding-line-1"
    captured = capsys.readouterr()
    assert "1 findings over 1 TUs" in captured.out
