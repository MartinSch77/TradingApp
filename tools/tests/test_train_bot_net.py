# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/train_bot_net.py — the desktop twin of the C++
BotNet trainer. Focuses on branch coverage of the path-confinement checks,
the malformed-line handling in load(), the degenerate-dataset guards in
main() (too few samples, all-one-label, etc.) and the training arithmetic
(standardise/apply_norm/auc/Net)."""

import json
import math

import pytest

import train_bot_net as tbn


# --------------------------------------------------------------------------
# default_log
# --------------------------------------------------------------------------

def test_default_log_windows(monkeypatch):
    monkeypatch.setattr(tbn.sys, "platform", "win32")
    path = tbn.default_log()
    assert path.parent.parts[-4:] == ("AppData", "Local", "TradingApp", "eToro Trader")
    assert path.name == "botsim-experience.jsonl"


def test_default_log_macos(monkeypatch):
    monkeypatch.setattr(tbn.sys, "platform", "darwin")
    path = tbn.default_log()
    assert path.parent.parts[-4:] == ("Library", "Preferences", "TradingApp", "eToro Trader")


def test_default_log_linux(monkeypatch):
    monkeypatch.setattr(tbn.sys, "platform", "linux")
    path = tbn.default_log()
    assert path.parent.parts[-3:] == (".config", "TradingApp", "eToro Trader")


# --------------------------------------------------------------------------
# allowed_roots / checked_path
# --------------------------------------------------------------------------

def test_allowed_roots_default_has_no_extra():
    roots = tbn.allowed_roots()
    assert tbn.Path.cwd().resolve() in roots


def test_allowed_roots_with_extra(tmp_path):
    roots = tbn.allowed_roots([tmp_path])
    assert tmp_path.resolve() in roots


def test_checked_path_accepts_file_within_cwd(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    f = tmp_path / "log.jsonl"
    f.write_text("{}\n", encoding="utf-8")
    resolved = tbn.checked_path(f, must_exist=True)
    assert resolved == f.resolve()


def test_checked_path_rejects_outside_roots(tmp_path):
    outside = tmp_path / "outside" / "log.jsonl"
    outside.parent.mkdir()
    outside.write_text("{}\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="refusing to touch"):
        tbn.checked_path(outside, must_exist=True)


def test_checked_path_extra_root_widens_allowlist(tmp_path):
    outside = tmp_path / "scratch" / "log.jsonl"
    outside.parent.mkdir()
    outside.write_text("{}\n", encoding="utf-8")
    resolved = tbn.checked_path(outside, must_exist=True, extra_roots=[tmp_path / "scratch"])
    assert resolved == outside.resolve()


def test_checked_path_must_exist_but_missing(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    missing = tmp_path / "nope.jsonl"
    with pytest.raises(SystemExit, match="not a readable file"):
        tbn.checked_path(missing, must_exist=True)


def test_checked_path_must_exist_false_ok_when_parent_exists(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    out = tmp_path / "out.json"
    resolved = tbn.checked_path(out, must_exist=False)
    assert resolved == out.resolve()


def test_checked_path_must_exist_false_missing_parent(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    out = tmp_path / "nosuchdir" / "out.json"
    with pytest.raises(SystemExit, match="does not exist"):
        tbn.checked_path(out, must_exist=False)


# --------------------------------------------------------------------------
# load
# --------------------------------------------------------------------------

def _feature_row(**overrides):
    row = {name: 1.0 for name in tbn.FEATURES}
    row.update(overrides)
    return row


def test_load_skips_blank_lines_and_parses_valid_rows(tmp_path):
    path = tmp_path / "log.jsonl"
    good_win = json.dumps({"features": _feature_row(), "netPnl": 5.0})
    good_loss = json.dumps({"features": _feature_row(confidence=0.2), "netPnl": -2.0})
    path.write_text(f"\n  \n{good_win}\n{good_loss}\n", encoding="utf-8")
    rows, labels, problems = tbn.load(path)
    assert len(rows) == 2
    assert labels == [1.0, 0.0]
    assert problems == []


def test_load_reports_json_decode_error(tmp_path):
    path = tmp_path / "log.jsonl"
    path.write_text("not json at all\n", encoding="utf-8")
    rows, labels, problems = tbn.load(path)
    assert rows == [] and labels == []
    assert len(problems) == 1
    assert "line 1" in problems[0]


def test_load_reports_missing_features(tmp_path):
    path = tmp_path / "log.jsonl"
    feats = _feature_row()
    del feats["confidence"]
    rec = json.dumps({"features": feats, "netPnl": 1.0})
    path.write_text(rec + "\n", encoding="utf-8")
    rows, labels, problems = tbn.load(path)
    assert rows == []
    assert "missing confidence" in problems[0]


def test_load_reports_missing_outcome(tmp_path):
    path = tmp_path / "log.jsonl"
    rec = json.dumps({"features": _feature_row()})
    path.write_text(rec + "\n", encoding="utf-8")
    rows, labels, problems = tbn.load(path)
    assert rows == []
    assert "no outcome" in problems[0]


def test_load_netpnl_exactly_zero_is_a_loss(tmp_path):
    path = tmp_path / "log.jsonl"
    rec = json.dumps({"features": _feature_row(), "netPnl": 0.0})
    path.write_text(rec + "\n", encoding="utf-8")
    rows, labels, problems = tbn.load(path)
    assert labels == [0.0]


def test_load_missing_features_key_entirely(tmp_path):
    path = tmp_path / "log.jsonl"
    rec = json.dumps({"netPnl": 1.0})
    path.write_text(rec + "\n", encoding="utf-8")
    rows, labels, problems = tbn.load(path)
    assert rows == []
    assert problems


# --------------------------------------------------------------------------
# standardise / apply_norm
# --------------------------------------------------------------------------

def test_standardise_varying_column():
    rows = [[i] * len(tbn.FEATURES) for i in (1.0, 2.0, 3.0)]
    mean, sd = tbn.standardise(rows)
    assert mean[0] == pytest.approx(2.0)
    assert sd[0] > 0


def test_standardise_constant_column_gets_unit_stddev():
    rows = [[5.0] * len(tbn.FEATURES) for _ in range(4)]
    mean, sd = tbn.standardise(rows)
    assert mean[0] == pytest.approx(5.0)
    assert all(s == 1.0 for s in sd)


def test_apply_norm_centers_and_scales():
    rows = [[1.0] * len(tbn.FEATURES), [3.0] * len(tbn.FEATURES)]
    mean, sd = tbn.standardise(rows)
    normed = tbn.apply_norm(rows, mean, sd)
    assert normed[0][0] == pytest.approx(-1.0)
    assert normed[1][0] == pytest.approx(1.0)


# --------------------------------------------------------------------------
# auc
# --------------------------------------------------------------------------

def test_auc_perfect_separation():
    scores = [0.1, 0.2, 0.8, 0.9]
    labels = [0.0, 0.0, 1.0, 1.0]
    assert tbn.auc(scores, labels) == pytest.approx(1.0)


def test_auc_no_positives_returns_half():
    assert tbn.auc([0.1, 0.2, 0.3], [0.0, 0.0, 0.0]) == 0.5


def test_auc_no_negatives_returns_half():
    assert tbn.auc([0.1, 0.2, 0.3], [1.0, 1.0, 1.0]) == 0.5


def test_auc_handles_tied_scores():
    # Two tied scores split between a winner and a loser: rank-averaging
    # exercises the inner tie-loop rather than the fast path.
    scores = [0.5, 0.5, 0.9]
    labels = [0.0, 1.0, 1.0]
    result = tbn.auc(scores, labels)
    assert 0.0 <= result <= 1.0


# --------------------------------------------------------------------------
# Net
# --------------------------------------------------------------------------

def test_net_forward_predict_range():
    rng = tbn.random.Random(1)
    net = tbn.Net(len(tbn.FEATURES), 3, rng)
    x = [0.1] * len(tbn.FEATURES)
    p = net.predict(x)
    assert 0.0 < p < 1.0


def test_net_forward_clamps_extreme_z():
    rng = tbn.random.Random(1)
    net = tbn.Net(2, 2, rng)
    # Force huge weights so the pre-sigmoid z would overflow without clamping.
    net.w1 = [[1000.0, 1000.0], [1000.0, 1000.0]]
    net.b1 = [0.0, 0.0]
    net.w2 = [1000.0, 1000.0]
    net.b2 = 0.0
    h, p = net.forward([1.0, 1.0])
    assert p == pytest.approx(1.0)
    net.w2 = [-1000.0, -1000.0]
    _, p2 = net.forward([1.0, 1.0])
    assert p2 == pytest.approx(0.0)


def test_net_step_reduces_loss_over_iterations():
    rng = tbn.random.Random(42)
    net = tbn.Net(len(tbn.FEATURES), 4, rng)
    x = [0.5] * len(tbn.FEATURES)
    first = net.step(x, 1.0, lr=0.1, l2=0.0)
    for _ in range(50):
        last = net.step(x, 1.0, lr=0.1, l2=0.0)
    assert last < first


# --------------------------------------------------------------------------
# main() — the degenerate-dataset guards and the happy path
# --------------------------------------------------------------------------

def _write_log(path, rows):
    with path.open("w", encoding="utf-8") as handle:
        for feats, net_pnl in rows:
            handle.write(json.dumps({"features": feats, "netPnl": net_pnl}) + "\n")


def test_main_no_log_returns_3(tmp_path, monkeypatch, capsys):
    missing = tmp_path / "nope.jsonl"
    monkeypatch.setattr(tbn.sys, "argv", ["train_bot_net.py", "--log", str(missing)])
    assert tbn.main() == 3
    assert "no experience log" in capsys.readouterr().out


def test_main_too_few_samples_returns_3(tmp_path, monkeypatch, capsys):
    log = tmp_path / "log.jsonl"
    _write_log(log, [(_feature_row(), 1.0), (_feature_row(), -1.0)])
    monkeypatch.setattr(tbn.sys, "argv", [
        "train_bot_net.py", "--log", str(log), "--allow-root", str(tmp_path),
    ])
    assert tbn.main() == 3
    assert "too few to train on" in capsys.readouterr().out


def test_main_all_same_label_returns_3(tmp_path, monkeypatch, capsys):
    log = tmp_path / "log.jsonl"
    rows = [(_feature_row(confidence=float(i)), 1.0) for i in range(50)]
    _write_log(log, rows)
    monkeypatch.setattr(tbn.sys, "argv", [
        "train_bot_net.py", "--log", str(log), "--allow-root", str(tmp_path),
        "--min-samples", "10",
    ])
    assert tbn.main() == 3
    assert "there is nothing to separate yet" in capsys.readouterr().out


def _mixed_rows(n):
    rows = []
    for i in range(n):
        win = i % 2 == 0
        feats = _feature_row(confidence=float(i % 5), hourUtc=float(i % 24))
        rows.append((feats, 3.0 if win else -3.0))
    return rows


def test_main_problem_lines_truncated_message(tmp_path, monkeypatch, capsys):
    log = tmp_path / "log.jsonl"
    rows = _mixed_rows(50)
    with log.open("w", encoding="utf-8") as handle:
        for _ in range(7):
            handle.write("not json\n")
        for feats, net_pnl in rows:
            handle.write(json.dumps({"features": feats, "netPnl": net_pnl}) + "\n")
    monkeypatch.setattr(tbn.sys, "argv", [
        "train_bot_net.py", "--log", str(log), "--allow-root", str(tmp_path),
        "--epochs", "2", "--hidden", "2",
    ])
    code = tbn.main()
    out = capsys.readouterr().out
    assert "…and 2 more unusable lines" in out
    assert code == 0


def test_main_few_problem_lines_no_truncation_message(tmp_path, monkeypatch, capsys):
    log = tmp_path / "log.jsonl"
    rows = _mixed_rows(50)
    with log.open("w", encoding="utf-8") as handle:
        handle.write("not json\n")
        for feats, net_pnl in rows:
            handle.write(json.dumps({"features": feats, "netPnl": net_pnl}) + "\n")
    monkeypatch.setattr(tbn.sys, "argv", [
        "train_bot_net.py", "--log", str(log), "--allow-root", str(tmp_path),
        "--epochs", "2", "--hidden", "2",
    ])
    tbn.main()
    out = capsys.readouterr().out
    assert "…and" not in out
    assert "skipped line 1" in out


def test_main_happy_path_writes_model(tmp_path, monkeypatch, capsys):
    log = tmp_path / "log.jsonl"
    _write_log(log, _mixed_rows(60))
    out_path = tmp_path / "model.json"
    monkeypatch.setattr(tbn.sys, "argv", [
        "train_bot_net.py", "--log", str(log), "--out", str(out_path),
        "--allow-root", str(tmp_path), "--epochs", "10", "--hidden", "3",
        "--seed", "7",
    ])
    code = tbn.main()
    assert code == 0
    assert out_path.is_file()
    model = json.loads(out_path.read_text(encoding="utf-8"))
    assert model["features"] == tbn.FEATURES
    assert len(model["w1"]) == 3
    assert model["hidden"] == 3
    assert model["epochs"] == 10
    assert "valAuc" in model and "valAccuracy" in model
    out = capsys.readouterr().out
    assert "wrote" in out
    assert "held-out tail" in out


def test_main_default_out_path_beside_log(tmp_path, monkeypatch):
    log = tmp_path / "log.jsonl"
    _write_log(log, _mixed_rows(60))
    monkeypatch.setattr(tbn.sys, "argv", [
        "train_bot_net.py", "--log", str(log), "--allow-root", str(tmp_path),
        "--epochs", "3", "--hidden", "2",
    ])
    code = tbn.main()
    assert code == 0
    assert (tmp_path / "botnet.json").is_file()


def test_main_reports_when_val_auc_below_half(tmp_path, monkeypatch, capsys):
    log = tmp_path / "log.jsonl"
    _write_log(log, _mixed_rows(60))
    monkeypatch.setattr(tbn.sys, "argv", [
        "train_bot_net.py", "--log", str(log), "--allow-root", str(tmp_path),
        "--epochs", "2", "--hidden", "2",
    ])
    monkeypatch.setattr(tbn, "auc", lambda scores, labels: 0.2)
    code = tbn.main()
    out = capsys.readouterr().out
    assert code == 0
    assert "worse than a coin flip" in out
