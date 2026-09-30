# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/clang_analyzer.py."""

import json
import os
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


def test_find_compiler_non_msvc_prefers_the_newest_versioned_clangxx(monkeypatch, tmp_path):
    # PATH holds clang++-18 and clang++-23 (plus a too-old 16 and a look-alike):
    # the newest wins, so a compile database from a newer GCC still parses.
    for name in ("clang++-18", "clang++-23", "clang++-16", "clang++-23.bak", "clang++"):
        (tmp_path / name).write_text("")
    monkeypatch.setenv("PATH", str(tmp_path))
    monkeypatch.delenv("CLANG_ANALYZER_CXX", raising=False)
    monkeypatch.setattr(ca.shutil, "which", lambda name: f"/usr/bin/{name}")
    assert ca._versioned_clangxx() == ["clang++-23", "clang++-18"]
    assert ca._find_compiler("g++") == "/usr/bin/clang++-23"


def test_versioned_clangxx_ignores_unreadable_path_entries(monkeypatch, tmp_path):
    monkeypatch.setenv("PATH", os.pathsep.join([str(tmp_path / "missing"), str(tmp_path)]))
    (tmp_path / "clang++-19").write_text("")
    assert ca._versioned_clangxx() == ["clang++-19"]


def test_find_compiler_falls_back_to_plain_clangxx(monkeypatch, tmp_path):
    monkeypatch.setenv("PATH", str(tmp_path))   # no versioned clang++ anywhere
    monkeypatch.delenv("CLANG_ANALYZER_CXX", raising=False)
    def which(name):
        return "/usr/bin/clang++" if name == "clang++" else None
    monkeypatch.setattr(ca.shutil, "which", which)
    assert ca._find_compiler("g++") == "/usr/bin/clang++"


def test_find_compiler_none_found(monkeypatch, tmp_path):
    monkeypatch.setenv("PATH", str(tmp_path))
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

# --------------------------------------------------------------------------
# _available_checkers / _resolve_checkers
# --------------------------------------------------------------------------

HELP_23 = """OVERVIEW: Clang Static Analyzer Checkers List

USAGE: -analyzer-checker <CHECKER or PACKAGE,...>

CHECKERS:
  core.CallAndMessage           Check for logical errors
  optin.cplusplus.UninitializedObject
                                Reports uninitialized fields after object construction
  optin.cplusplus.VirtualCall   Check virtual function calls
  optin.core.EnumCastOutOfRange Check integer to enumeration casts for out of range values
  optin.portability.UnixAPI     Finds implementation-defined behavior
  security.FloatLoopCounter     Warn on using a floating point value
  security.VAList               Warn on misuse of va_list objects
  security.cert.env.InvalidPtr  Finds usages of possibly invalidated pointers
  nullability.NullableDereferenced
                                Warns when a nullable pointer is dereferenced.
  nullability.NullablePassedToNonnull
                                Warns when a nullable pointer is passed to a function
  nullability.NullableReturnedFromNonnull
                                Warns when a nullable pointer is returned from a function
"""


def _fake_run(stdout, returncode=0):
    def run(args, **kwargs):
        return subprocess.CompletedProcess(args, returncode, stdout=stdout, stderr="")
    return run


def test_available_checkers_parses_the_help_listing(monkeypatch):
    monkeypatch.setattr(ca.subprocess, "run", _fake_run(HELP_23))
    names = ca._available_checkers("/usr/bin/clang++-23")
    assert "security.VAList" in names and "core.CallAndMessage" in names
    # clang 23 prints a long name ALONE on its line and a medium one with a single
    # space before the description — both are checker names, the wrapped
    # description lines ("Warns…", "Reports…") are not.
    assert "optin.cplusplus.UninitializedObject" in names
    assert "optin.core.EnumCastOutOfRange" in names
    assert "nullability.NullableDereferenced" in names
    assert not {n for n in names if not any(c == "." for c in n)}
    assert "CHECKERS:" not in names and "USAGE:" not in names


def test_available_checkers_none_when_the_probe_fails(monkeypatch):
    monkeypatch.setattr(ca.subprocess, "run", _fake_run("", returncode=1))
    assert ca._available_checkers("/usr/bin/clang++") is None

    def boom(args, **kwargs):
        raise OSError("no such driver")
    monkeypatch.setattr(ca.subprocess, "run", boom)
    assert ca._available_checkers("/usr/bin/clang++") is None


def test_resolve_checkers_renames_valist_once_and_keeps_the_rest():
    available = {m.group(1) for m in map(ca.CHECKER_LINE.match, HELP_23.splitlines()) if m}
    enabled, dropped = ca._resolve_checkers(available)
    assert dropped == []
    assert enabled.count("security.VAList") == 1       # two old names, one new checker
    assert "valist.Uninitialized" not in enabled and "valist.CopyToSelf" not in enabled
    assert enabled[:2] == list(ca.EXTRA_CHECKERS[:2])   # order and the rest untouched


def test_resolve_checkers_drops_and_names_what_no_rename_covers():
    available = set(ca.EXTRA_CHECKERS) - {"security.FloatLoopCounter", "valist.CopyToSelf"}
    enabled, dropped = ca._resolve_checkers(available)
    assert dropped == ["security.FloatLoopCounter", "valist.CopyToSelf"]
    assert "security.FloatLoopCounter" not in enabled and "valist.Uninitialized" in enabled


def test_resolve_checkers_without_a_probe_keeps_the_verified_clang18_set():
    assert ca._resolve_checkers(None) == (list(ca.EXTRA_CHECKERS), [])


def test_analyzer_flags_take_the_resolved_checker_list():
    flags = ca._analyzer_flags(False, ["security.VAList"])
    assert flags.count("-analyzer-checker=security.VAList") == 1
    assert not any(f.startswith("-analyzer-checker=valist") for f in flags)


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
