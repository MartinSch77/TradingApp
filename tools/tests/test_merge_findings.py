# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/merge_findings.py."""

import csv

import merge_findings


def _write(path, text):
    path.write_text(text, encoding="utf-8")


def test_main_merges_pipe_and_gcc_style_and_skips_missing(tmp_path, monkeypatch, capsys):
    out = tmp_path
    # pipe-format tool: valid line + malformed line (too few pipe fields)
    _write(out / "cppcheck.txt",
           "src/a.cpp|10|warning|nullPointer|possible null pointer\n"
           "not-enough-fields|only-two\n")
    # another pipe tool present but empty
    _write(out / "codespell.txt", "")
    # lizard.txt and pmd-cpd.txt intentionally absent -> skipped without error

    # gcc-style tool: matching + non-matching line
    _write(out / "clang-tidy.txt",
           "src/b.cpp:42:5: warning: unused variable 'x' [misc-unused]\n"
           "this line matches nothing\n")
    # clazy/gcc-analyzer/clang-analyzer/msvc-analyze absent -> skipped

    monkeypatch.setattr("sys.argv", ["merge_findings.py", str(out)])
    merge_findings.main()

    result_path = out / "external_findings.csv"
    assert result_path.is_file()
    with open(result_path, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f, delimiter=";"))
    assert rows[0] == ["tool", "file", "line", "rule", "severity", "message"]
    body = rows[1:]
    assert ["cppcheck", "src/a.cpp", "10", "nullPointer", "warning",
            "possible null pointer"] in body
    assert ["clang-tidy", "src/b.cpp", "42", "misc-unused", "warning",
            "unused variable 'x'"] in body
    # malformed / non-matching lines never made it in
    assert len(body) == 2

    captured = capsys.readouterr()
    assert "merged: 2 findings" in captured.out


def test_main_no_files_present_writes_header_only(tmp_path, monkeypatch):
    monkeypatch.setattr("sys.argv", ["merge_findings.py", str(tmp_path)])
    merge_findings.main()
    result_path = tmp_path / "external_findings.csv"
    with open(result_path, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f, delimiter=";"))
    assert rows == [["tool", "file", "line", "rule", "severity", "message"]]


def test_main_wrong_arg_count_exits(monkeypatch):
    monkeypatch.setattr("sys.argv", ["merge_findings.py"])
    try:
        merge_findings.main()
        assert False, "expected SystemExit"
    except SystemExit:
        pass


def test_gcc_style_error_severity_and_all_tools(tmp_path, monkeypatch):
    out = tmp_path
    for tool in merge_findings.GCC_STYLE_TOOLS:
        _write(out / f"{tool}.txt",
               f"src/{tool}.cpp:1:1: error: bad thing [{tool}-rule]\n")
    monkeypatch.setattr("sys.argv", ["merge_findings.py", str(out)])
    merge_findings.main()
    with open(out / "external_findings.csv", newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f, delimiter=";"))
    assert len(rows) - 1 == len(merge_findings.GCC_STYLE_TOOLS)
    severities = {r[4] for r in rows[1:]}
    assert severities == {"error"}
