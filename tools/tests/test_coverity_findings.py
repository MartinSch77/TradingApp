# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/coverity_findings.py."""

import json

import pytest

import coverity_findings as cf


# --------------------------------------------------------------------------
# _flatten
# --------------------------------------------------------------------------

def test_flatten_merges_nested_dict_without_overriding_top_level():
    record = {
        "file": "a.cpp",
        "checkerProperties": {"subcategoryShortDescription": "desc", "file": "nested-should-not-win"},
        "properties": {"list_value": [1, 2], "extra": "kept"},
    }
    flat = cf._flatten(record)
    assert flat["file"] == "a.cpp"  # top-level wins
    assert flat["subcategoryShortDescription"] == "desc"
    assert flat["extra"] == "kept"
    assert "list_value" not in flat  # lists inside nested dicts are dropped


# --------------------------------------------------------------------------
# _first
# --------------------------------------------------------------------------

def test_first_returns_first_present_nonempty():
    flat = {"a": "", "b": None, "c": "value"}
    assert cf._first(flat, ("a", "b", "c"), "default") == "value"


def test_first_returns_default_when_nothing_found():
    assert cf._first({}, ("a", "b"), "default") == "default"


# --------------------------------------------------------------------------
# _main_event_text
# --------------------------------------------------------------------------

def test_main_event_text_finds_main_event():
    record = {"events": [{"main": False, "eventDescription": "not this"},
                         {"main": True, "eventDescription": "the real one"}]}
    assert cf._main_event_text(record) == "the real one"


def test_main_event_text_no_events_key():
    assert cf._main_event_text({}) == ""


def test_main_event_text_no_main_event():
    record = {"events": [{"main": False, "eventDescription": "x"}]}
    assert cf._main_event_text(record) == ""


# --------------------------------------------------------------------------
# _clean
# --------------------------------------------------------------------------

def test_clean_collapses_whitespace_and_strips_pipes():
    assert cf._clean("  a | b\nc\t d ") == "a / b c d"


# --------------------------------------------------------------------------
# _relative
# --------------------------------------------------------------------------

def test_relative_strip_prefix_wins_first(tmp_path):
    root = tmp_path / "TradingApp"
    root.mkdir()
    path = "/home/runner/work/TradingApp/TradingApp/src/domain/Foo.cpp"
    result = cf._relative(path, root, ["/home/runner/work/TradingApp/TradingApp"])
    assert result == "src/domain/Foo.cpp"


def test_relative_project_root_prefix(tmp_path):
    root = tmp_path / "proj"
    root.mkdir()
    path = str(root / "src" / "Foo.cpp")
    assert cf._relative(path, root, []) == "src/Foo.cpp"


def test_relative_marker_fallback_folds_doubled_checkout(tmp_path):
    root = tmp_path / "TradingApp"
    root.mkdir()
    path = "/home/runner/work/TradingApp/TradingApp/src/domain/Foo.cpp"
    result = cf._relative(path, root, [])
    assert result == "src/domain/Foo.cpp"


def test_relative_no_match_returns_lstripped_text(tmp_path):
    root = tmp_path / "proj"
    root.mkdir()
    result = cf._relative("/completely/unrelated/path.cpp", root, [])
    assert result == "completely/unrelated/path.cpp"


def test_relative_empty_path_returns_empty(tmp_path):
    assert cf._relative("", tmp_path, []) == ""


def test_relative_backslashes_normalized(tmp_path):
    root = tmp_path / "proj"
    root.mkdir()
    path = str(root).replace("/", "\\") + "\\src\\Foo.cpp"
    result = cf._relative(path, root, [])
    assert result == "src/Foo.cpp"


# --------------------------------------------------------------------------
# _records
# --------------------------------------------------------------------------

def test_records_from_plain_list():
    payload = [{"a": 1}, "not-a-dict", {"b": 2}]
    assert cf._records(payload) == [{"a": 1}, {"b": 2}]


def test_records_from_issues_key():
    payload = {"issues": [{"a": 1}]}
    assert cf._records(payload) == [{"a": 1}]


def test_records_from_view_contents_rows():
    payload = {"viewContentsV1": {"rows": [{"a": 1}]}}
    assert cf._records(payload) == [{"a": 1}]


def test_records_no_recognizable_shape_returns_empty():
    assert cf._records({"nothing": "useful"}) == []
    assert cf._records("just a string") == []


def test_records_nested_search_finds_deep_list():
    payload = {"outer": {"defects": [{"a": 1}]}}
    assert cf._records(payload) == [{"a": 1}]


# --------------------------------------------------------------------------
# _parse
# --------------------------------------------------------------------------

def test_parse_json_array():
    text = json.dumps([{"file": "a.cpp"}])
    assert cf._parse(text) == [{"file": "a.cpp"}]


def test_parse_json_object():
    text = json.dumps({"issues": [{"file": "a.cpp"}]})
    assert cf._parse(text) == [{"file": "a.cpp"}]


def test_parse_empty_text_returns_empty_list():
    assert cf._parse("") == []
    assert cf._parse("   ") == []


def test_parse_csv_text():
    text = "File,Checker,Line\na.cpp,NULL_RETURNS,10\n"
    rows = cf._parse(text)
    assert rows == [{"File": "a.cpp", "Checker": "NULL_RETURNS", "Line": "10"}]


def test_parse_csv_semicolon_dialect():
    text = "File;Checker;Line\na.cpp;NULL_RETURNS;10\n"
    rows = cf._parse(text)
    assert rows[0]["File"] == "a.cpp"


def test_parse_csv_sniffer_failure_falls_back_to_excel_dialect():
    # A single column with no delimiter characters at all defeats csv.Sniffer,
    # which raises csv.Error("Could not determine delimiter") — the except
    # branch then falls back to the excel dialect.
    text = "onlyoneheader\nonlyonevalue\n"
    rows = cf._parse(text)
    assert rows == [{"onlyoneheader": "onlyonevalue"}]


# --------------------------------------------------------------------------
# _convert
# --------------------------------------------------------------------------

def test_convert_basic_record(tmp_path):
    root = tmp_path / "proj"
    root.mkdir()
    records = [{
        "file": str(root / "src" / "Foo.cpp"),
        "checker": "RESOURCE_LEAK",
        "line": "42",
        "impact": "High",
        "cid": "1234",
        "description": "leak here",
    }]
    lines, skipped, unrecognized = cf._convert(records, root, [], include_triaged=False)
    assert skipped == 0
    assert unrecognized == 0
    assert lines == ["src/Foo.cpp|42|high|RESOURCE_LEAK|CID 1234: leak here"]


def test_convert_skips_triaged_by_default(tmp_path):
    root = tmp_path
    records = [{
        "file": str(root / "a.cpp"), "checker": "X", "line": "1",
        "classification": "False Positive",
    }]
    lines, skipped, unrecognized = cf._convert(records, root, [], include_triaged=False)
    assert lines == []
    assert skipped == 1


def test_convert_include_triaged_flag_keeps_them(tmp_path):
    root = tmp_path
    records = [{
        "file": str(root / "a.cpp"), "checker": "X", "line": "1",
        "classification": "Intentional",
    }]
    lines, skipped, unrecognized = cf._convert(records, root, [], include_triaged=True)
    assert len(lines) == 1
    assert skipped == 0


def test_convert_unrecognized_record_missing_file_or_checker(tmp_path):
    root = tmp_path
    records = [{"line": "1"}]  # no file, no checker
    lines, skipped, unrecognized = cf._convert(records, root, [], include_triaged=False)
    assert lines == []
    assert unrecognized == 1


def test_convert_defaults_line_to_1_on_bad_value(tmp_path):
    root = tmp_path
    records = [{"file": str(root / "a.cpp"), "checker": "X", "line": "not-a-number"}]
    lines, _, _ = cf._convert(records, root, [], include_triaged=False)
    assert lines[0].split("|")[1] == "1"


def test_convert_defaults_severity_to_warning(tmp_path):
    root = tmp_path
    records = [{"file": str(root / "a.cpp"), "checker": "X"}]
    lines, _, _ = cf._convert(records, root, [], include_triaged=False)
    assert "|warning|" in lines[0]


def test_convert_message_falls_back_to_main_event_then_checker(tmp_path):
    root = tmp_path
    records = [{
        "file": str(root / "a.cpp"), "checker": "X",
        "events": [{"main": True, "eventDescription": "from main event"}],
    }]
    lines, _, _ = cf._convert(records, root, [], include_triaged=False)
    assert lines[0].endswith("from main event")

    records2 = [{"file": str(root / "a.cpp"), "checker": "OnlyChecker"}]
    lines2, _, _ = cf._convert(records2, root, [], include_triaged=False)
    assert lines2[0].endswith("OnlyChecker")


def test_convert_line_negative_or_zero_floored_to_one(tmp_path):
    root = tmp_path
    records = [{"file": str(root / "a.cpp"), "checker": "X", "line": "0"}]
    lines, _, _ = cf._convert(records, root, [], include_triaged=False)
    assert lines[0].split("|")[1] == "1"


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def test_main_export_not_found(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr("sys.argv", ["coverity_findings.py", str(tmp_path / "nope.csv")])
    rc = cf.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "no such export" in captured.err


def test_main_parse_error_returns_1(tmp_path, monkeypatch, capsys):
    bad_json = tmp_path / "export.json"
    bad_json.write_text("{not valid json", encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["coverity_findings.py", str(bad_json)])
    rc = cf.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "cannot parse" in captured.err


def test_main_stdin_input(tmp_path, monkeypatch, capsys):
    root = tmp_path / "proj"
    root.mkdir()
    out = tmp_path / "coverity.txt"
    payload = json.dumps([{
        "file": str(root / "src" / "a.cpp"), "checker": "X", "line": "3",
    }])
    monkeypatch.setattr("sys.stdin", __import__("io").StringIO(payload))
    monkeypatch.setattr("sys.argv",
                        ["coverity_findings.py", "-", "--root", str(root), "-o", str(out)])
    rc = cf.main()
    assert rc == 0
    text = out.read_text()
    assert "src/a.cpp|3" in text


def test_main_writes_output_and_reports_unrecognized(tmp_path, monkeypatch, capsys):
    root = tmp_path / "proj"
    root.mkdir()
    export = tmp_path / "export.json"
    export.write_text(json.dumps([
        {"file": str(root / "a.cpp"), "checker": "X", "line": "1"},
        {"no_file_or_checker": True},
    ]), encoding="utf-8")
    out = tmp_path / "out.txt"
    monkeypatch.setattr("sys.argv",
                        ["coverity_findings.py", str(export), "--root", str(root),
                         "-o", str(out)])
    rc = cf.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert "1 defects" in captured.out
    assert "WARNING 1 of 2 records" in captured.err


def test_main_empty_export_reports_no_defects(tmp_path, monkeypatch, capsys):
    root = tmp_path / "proj"
    root.mkdir()
    export = tmp_path / "export.json"
    export.write_text("[]", encoding="utf-8")
    out = tmp_path / "out.txt"
    monkeypatch.setattr("sys.argv",
                        ["coverity_findings.py", str(export), "--root", str(root),
                         "-o", str(out)])
    rc = cf.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert "no defect records" in captured.out


def test_main_default_output_path_used_when_not_specified(tmp_path, monkeypatch):
    root = tmp_path / "proj"
    root.mkdir()
    export = tmp_path / "export.json"
    export.write_text("[]", encoding="utf-8")
    monkeypatch.setattr("sys.argv",
                        ["coverity_findings.py", str(export), "--root", str(root)])
    rc = cf.main()
    assert rc == 0
    assert (root / "analysis-results" / "coverity.txt").is_file()


def test_main_skipped_triaged_reported_in_summary(tmp_path, monkeypatch, capsys):
    root = tmp_path / "proj"
    root.mkdir()
    export = tmp_path / "export.json"
    export.write_text(json.dumps([
        {"file": str(root / "a.cpp"), "checker": "X", "line": "1",
         "classification": "Ignore"},
    ]), encoding="utf-8")
    out = tmp_path / "out.txt"
    monkeypatch.setattr("sys.argv",
                        ["coverity_findings.py", str(export), "--root", str(root),
                         "-o", str(out)])
    rc = cf.main()
    assert rc == 0
    captured = capsys.readouterr()
    assert "1 triaged, not imported" in captured.out
