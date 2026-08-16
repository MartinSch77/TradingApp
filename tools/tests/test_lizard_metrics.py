# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/lizard_metrics.py — the metrics ratchet gate is the
highest-value logic here: new debt / regression / stale-improved / stale-gone."""

import json

import pytest

import lizard_metrics as lm


# --------------------------------------------------------------------------
# _lizard_command
# --------------------------------------------------------------------------

def test_lizard_command_prefers_path_executable(monkeypatch):
    monkeypatch.setattr(lm.shutil, "which", lambda name: "/usr/bin/lizard")
    assert lm._lizard_command() == ["/usr/bin/lizard"]


def test_lizard_command_falls_back_to_module(monkeypatch):
    monkeypatch.setattr(lm.shutil, "which", lambda name: None)

    class Probe:
        returncode = 0

    monkeypatch.setattr(lm.subprocess, "run", lambda *a, **k: Probe())
    assert lm._lizard_command() == [lm.sys.executable, "-m", "lizard"]


def test_lizard_command_none_found(monkeypatch):
    monkeypatch.setattr(lm.shutil, "which", lambda name: None)

    class Probe:
        returncode = 1

    monkeypatch.setattr(lm.subprocess, "run", lambda *a, **k: Probe())
    assert lm._lizard_command() == []


# --------------------------------------------------------------------------
# _measure
# --------------------------------------------------------------------------

def _csv_row(file_, name, nloc=10, ccn=5, tokens=50, params=1, start=1, end=20):
    # lizard --csv column order:
    #  nloc, ccn, token_count, param_count, length, location, file, name,
    #  long_name, start_line, end_line
    return (f"{nloc},{ccn},{tokens},{params},{end - start},"
            f"{file_}:{start},{file_},{name},{name}(),{start},{end}")


def test_measure_parses_csv_and_skips_non_digit_ccn(tmp_path, monkeypatch):
    (tmp_path / "src").mkdir()
    good = _csv_row(str(tmp_path / "src" / "a.cpp"), "foo")
    bad = "not,a,valid,row,at,all,x,y,z,1,2"  # ccn field "a" is not digit

    class Result:
        stdout = good + "\n" + bad + "\n"
        stderr = ""
        returncode = 0

    monkeypatch.setattr(lm.subprocess, "run", lambda *a, **k: Result())
    rows = lm._measure(tmp_path, ["lizard"])
    assert len(rows) == 1
    assert rows[0][lm._CSV_NAME] == "foo"


def test_measure_raises_on_empty_stdout(tmp_path, monkeypatch):
    class Result:
        stdout = "   "
        stderr = "boom"
        returncode = 1

    monkeypatch.setattr(lm.subprocess, "run", lambda *a, **k: Result())
    with pytest.raises(SystemExit, match="no output"):
        lm._measure(tmp_path, ["lizard"])


def test_measure_raises_when_no_rows_parsed(tmp_path, monkeypatch):
    class Result:
        stdout = "garbage,line,with,no,digit,ccn,field,x,y,z,w\n"
        stderr = ""
        returncode = 0

    monkeypatch.setattr(lm.subprocess, "run", lambda *a, **k: Result())
    with pytest.raises(SystemExit, match="no functions"):
        lm._measure(tmp_path, ["lizard"])


# --------------------------------------------------------------------------
# _key
# --------------------------------------------------------------------------

def test_key_relative_to_root(tmp_path):
    root = tmp_path
    row = [None] * 11
    row[lm._CSV_FILE] = str(root / "src" / "a.cpp")
    row[lm._CSV_NAME] = "foo"
    assert lm._key(root, row) == "src/a.cpp::foo"


def test_key_outside_root_falls_back_to_raw_path(tmp_path):
    root = tmp_path / "proj"
    root.mkdir()
    other = tmp_path / "elsewhere" / "a.cpp"
    row = [None] * 11
    row[lm._CSV_FILE] = str(other)
    row[lm._CSV_NAME] = "foo"
    key = lm._key(root, row)
    assert key.endswith("::foo")
    assert "elsewhere" in key


# --------------------------------------------------------------------------
# _violations
# --------------------------------------------------------------------------

def _row(nloc=1, ccn=1, params=1):
    row = [None] * 11
    row[lm._CSV_NLOC] = str(nloc)
    row[lm._CSV_CCN] = str(ccn)
    row[lm._CSV_PARAMS] = str(params)
    return row


def test_violations_none_under_threshold():
    assert lm._violations(_row(nloc=10, ccn=5, params=2)) == []


def test_violations_all_three_over_threshold():
    row = _row(nloc=lm.NLOC_LIMIT + 1, ccn=lm.CCN_LIMIT + 1, params=lm.PARAM_LIMIT + 1)
    hits = lm._violations(row)
    metrics = {m for m, _, _ in hits}
    assert metrics == {"ccn", "nloc", "params"}


def test_violations_exactly_at_limit_is_not_a_violation():
    row = _row(nloc=lm.NLOC_LIMIT, ccn=lm.CCN_LIMIT, params=lm.PARAM_LIMIT)
    assert lm._violations(row) == []


# --------------------------------------------------------------------------
# main — the ratchet gate
# --------------------------------------------------------------------------

def _setup_project(tmp_path):
    root = tmp_path / "proj"
    (root / "src").mkdir(parents=True)
    (root / "tools").mkdir(parents=True)
    out = tmp_path / "analysis-results"
    return root, out


def _mock_lizard(monkeypatch, csv_rows):
    class Result:
        stdout = "\n".join(csv_rows) + "\n"
        stderr = ""
        returncode = 0

    monkeypatch.setattr(lm, "_lizard_command", lambda: ["lizard"])
    monkeypatch.setattr(lm.subprocess, "run", lambda *a, **k: Result())


def test_main_no_lizard_installed_exits_3(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    monkeypatch.setattr(lm, "_lizard_command", lambda: [])
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])
    rc = lm.main()
    assert rc == 3
    assert (out / "lizard.txt").read_text() == ""


def test_main_wrong_args_exits(monkeypatch):
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", "onlyone"])
    with pytest.raises(SystemExit):
        lm.main()


def test_main_new_over_threshold_function_fails_gate(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    over_ccn = lm.CCN_LIMIT + 5
    row = _csv_row(str(root / "src" / "a.cpp"), "bigFunc", ccn=over_ccn)
    _mock_lizard(monkeypatch, [row])
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])

    rc = lm.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "NEW over-threshold function" in captured.err
    assert "src/a.cpp::bigFunc" in captured.err


def test_main_update_baseline_writes_json(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    over_ccn = lm.CCN_LIMIT + 5
    row = _csv_row(str(root / "src" / "a.cpp"), "bigFunc", ccn=over_ccn)
    _mock_lizard(monkeypatch, [row])
    monkeypatch.setattr("sys.argv",
                        ["lizard_metrics.py", str(root), str(out), "--update-baseline"])

    rc = lm.main()
    assert rc == 0
    baseline = json.loads((root / lm.BASELINE).read_text())
    assert "src/a.cpp::bigFunc" in baseline
    assert baseline["src/a.cpp::bigFunc"]["ccn"] == over_ccn


def test_main_baseline_exact_match_passes(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    over_ccn = lm.CCN_LIMIT + 5
    row = _csv_row(str(root / "src" / "a.cpp"), "bigFunc", ccn=over_ccn)
    _mock_lizard(monkeypatch, [row])
    (root / lm.BASELINE).write_text(
        json.dumps({"src/a.cpp::bigFunc": {"ccn": over_ccn}}), encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])

    rc = lm.main()
    assert rc == 0


def test_main_regression_over_baseline_budget_fails(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    over_ccn = lm.CCN_LIMIT + 10
    row = _csv_row(str(root / "src" / "a.cpp"), "bigFunc", ccn=over_ccn)
    _mock_lizard(monkeypatch, [row])
    (root / lm.BASELINE).write_text(
        json.dumps({"src/a.cpp::bigFunc": {"ccn": lm.CCN_LIMIT + 1}}), encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])

    rc = lm.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "regressed" in captured.err


def test_main_new_metric_violation_on_baselined_function(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    # Baselined for ccn only, but now also over NLOC.
    over_ccn = lm.CCN_LIMIT + 1
    over_nloc = lm.NLOC_LIMIT + 1
    row = _csv_row(str(root / "src" / "a.cpp"), "bigFunc", ccn=over_ccn, nloc=over_nloc)
    _mock_lizard(monkeypatch, [row])
    (root / lm.BASELINE).write_text(
        json.dumps({"src/a.cpp::bigFunc": {"ccn": over_ccn}}), encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])

    rc = lm.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "NEW NLOC violation" in captured.err


def test_main_stale_entry_function_gone_fails(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    row = _csv_row(str(root / "src" / "a.cpp"), "smallFunc", ccn=1)  # under threshold
    _mock_lizard(monkeypatch, [row])
    (root / lm.BASELINE).write_text(
        json.dumps({"src/gone.cpp::vanished": {"ccn": 99}}), encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])

    rc = lm.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "function gone" in captured.err


def test_main_stale_entry_now_under_threshold_fails(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    # Function still exists (live) but no longer exceeds any threshold.
    row = _csv_row(str(root / "src" / "a.cpp"), "healedFunc", ccn=1, nloc=1, params=1)
    _mock_lizard(monkeypatch, [row])
    (root / lm.BASELINE).write_text(
        json.dumps({"src/a.cpp::healedFunc": {"ccn": 99}}), encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])

    rc = lm.main()
    assert rc == 1
    captured = capsys.readouterr()
    assert "now under every threshold" in captured.err


def test_main_writes_metrics_csv_and_findings_file(tmp_path, monkeypatch, capsys):
    root, out = _setup_project(tmp_path)
    row = _csv_row(str(root / "src" / "a.cpp"), "smallFunc", ccn=1, nloc=1, params=1)
    _mock_lizard(monkeypatch, [row])
    monkeypatch.setattr("sys.argv", ["lizard_metrics.py", str(root), str(out)])

    rc = lm.main()
    assert rc == 0
    csv_text = (out / "lizard-metrics.csv").read_text()
    assert "smallFunc" in csv_text
    assert (out / "lizard.txt").read_text() == ""
