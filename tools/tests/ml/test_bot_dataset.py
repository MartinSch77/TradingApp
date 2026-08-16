# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/ml/bot_dataset.py — the bot-decision dataset builder built from
PredictionLedger's JSONL. Covers the path-outcome label (mirrors domain/PathOutcome.cpp),
the no-lookahead ledger filtering, and the CLI wiring (including that `splits` delegates
verbatim to crowd_dataset.cmd_splits)."""

from __future__ import annotations

import json

import pytest

import bot_dataset as bd
import crowd_dataset as cd


class Args:
    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)


# --------------------------------------------------------------------------- fail


def test_fail_raises_systemexit_1(capsys):
    with pytest.raises(SystemExit) as excinfo:
        bd.fail("boom")
    assert excinfo.value.code == 1
    assert "boom" in capsys.readouterr().err


# --------------------------------------------------------------------------- load_ledger


def test_load_ledger_happy_path(tmp_path):
    p = tmp_path / "ledger.jsonl"
    p.write_text(
        '{"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500", "price": 100.0}\n'
        '{"at": "2024-01-02T00:00:00.000Z", "symbol": "SPX500", "price": 101.0}\n',
        encoding="utf-8")
    rows = bd.load_ledger(p)
    assert len(rows) == 2
    assert rows[0]["symbol"] == "SPX500"


def test_load_ledger_skips_blank_lines(tmp_path):
    p = tmp_path / "ledger.jsonl"
    p.write_text(
        '{"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500", "price": 100.0}\n'
        '\n'
        '   \n'
        '{"at": "2024-01-02T00:00:00.000Z", "symbol": "SPX500", "price": 101.0}\n',
        encoding="utf-8")
    rows = bd.load_ledger(p)
    assert len(rows) == 2


def test_load_ledger_skips_unparsable_json(tmp_path):
    p = tmp_path / "ledger.jsonl"
    p.write_text(
        '{"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500", "price": 100.0}\n'
        'not-json-at-all{{{\n'
        '{"at": "2024-01-02T00:00:00.000Z", "symbol": "SPX500", "price": 101.0}\n',
        encoding="utf-8")
    rows = bd.load_ledger(p)
    assert len(rows) == 2


def test_load_ledger_skips_rows_missing_required_keys(tmp_path):
    p = tmp_path / "ledger.jsonl"
    p.write_text(
        '{"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500"}\n'  # no price
        '{"symbol": "SPX500", "price": 101.0}\n'  # no at
        '{"at": "2024-01-02T00:00:00.000Z", "price": 101.0}\n'  # no symbol
        '{"at": "2024-01-03T00:00:00.000Z", "symbol": "SPX500", "price": 102.0}\n',
        encoding="utf-8")
    rows = bd.load_ledger(p)
    assert len(rows) == 1
    assert rows[0]["price"] == 102.0


def test_load_ledger_truncated_final_line_costs_only_that_line(tmp_path):
    p = tmp_path / "ledger.jsonl"
    p.write_text(
        '{"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500", "price": 100.0}\n'
        '{"at": "2024-01-02T00:00:00.000Z", "symbol": "SPX500", "pri',  # truncated, no newline
        encoding="utf-8")
    rows = bd.load_ledger(p)
    assert len(rows) == 1


# --------------------------------------------------------------------------- walk_path / resolve_label


DEFAULT_CFG = {"stop_fraction": 0.01, "stop_r": 1.0, "target_r": 1.0,
              "cost_buffer_fraction": 0.0015, "max_bars": 20}


def test_walk_path_long_target_hit():
    entry = 100.0
    # target distance = 100*0.01*1 + 100*0.0015 = 1.15 -> target = 101.15
    prices = [100.5, 101.2]
    assert bd.walk_path(prices, entry, True, DEFAULT_CFG) is True


def test_walk_path_long_stop_hit_first():
    entry = 100.0
    # stop distance = 1.0 -> stop = 99.0
    prices = [98.5, 105.0]  # stop breached before any target chance
    assert bd.walk_path(prices, entry, True, DEFAULT_CFG) is False


def test_walk_path_stop_checked_before_target_same_point():
    # A single point that would satisfy both stop and target simultaneously is impossible for
    # a long/short here, but verify the conservative order: stop wins whenever it is hit.
    entry = 100.0
    cfg = dict(DEFAULT_CFG)
    prices = [99.0]  # exactly the stop price -> stop_hit True (<=)
    assert bd.walk_path(prices, entry, True, cfg) is False


def test_walk_path_short_target_hit():
    entry = 100.0
    prices = [99.5, 98.7]  # target = entry - 1.15 = 98.85 -> 98.7 clears it
    assert bd.walk_path(prices, entry, False, DEFAULT_CFG) is True


def test_walk_path_short_stop_hit():
    entry = 100.0
    prices = [101.5]  # stop = entry + 1.0 = 101.0, breached
    assert bd.walk_path(prices, entry, False, DEFAULT_CFG) is False


def test_walk_path_neither_hit_within_max_bars():
    entry = 100.0
    prices = [100.1] * 5
    assert bd.walk_path(prices, entry, True, DEFAULT_CFG) is False


def test_walk_path_zero_entry_price_returns_false():
    assert bd.walk_path([100.0], 0.0, True, DEFAULT_CFG) is False


def test_walk_path_zero_stop_fraction_returns_false():
    cfg = dict(DEFAULT_CFG, stop_fraction=0.0)
    assert bd.walk_path([100.0], 100.0, True, cfg) is False


def test_walk_path_respects_max_bars_limit():
    entry = 100.0
    cfg = dict(DEFAULT_CFG, max_bars=1)
    # target-hitting price appears only after max_bars window
    prices = [100.1, 999.0]
    assert bd.walk_path(prices, entry, True, cfg) is False


def test_resolve_label_long():
    entry = 100.0
    assert bd.resolve_label([101.2], entry, DEFAULT_CFG) == "LONG"


def test_resolve_label_short():
    entry = 100.0
    assert bd.resolve_label([98.7], entry, DEFAULT_CFG) == "SHORT"


def test_resolve_label_no_trade():
    entry = 100.0
    assert bd.resolve_label([100.05, 99.98], entry, DEFAULT_CFG) == "NO_TRADE"


# --------------------------------------------------------------------------- cmd_build


def write_ledger(path, rows):
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row) + "\n")


def build_args(ledger, instrument, out, manifest, **overrides):
    defaults = dict(ledger=str(ledger), instrument=instrument, out=str(out),
                    manifest=str(manifest), stop_fraction=0.01, target_r=1.0, stop_r=1.0,
                    cost_buffer_fraction=0.0015, max_bars=20)
    defaults.update(overrides)
    return Args(**defaults)


def test_cmd_build_ledger_not_found(tmp_path):
    args = build_args(tmp_path / "missing.jsonl", "SPX500", tmp_path / "out.csv",
                      tmp_path / "manifest.json")
    with pytest.raises(SystemExit):
        bd.cmd_build(args)


def test_cmd_build_no_rows_for_instrument(tmp_path):
    ledger_path = tmp_path / "ledger.jsonl"
    write_ledger(ledger_path, [{"at": "2024-01-01T00:00:00.000Z", "symbol": "NSDQ100",
                               "price": 100.0}])
    args = build_args(ledger_path, "SPX500", tmp_path / "out.csv", tmp_path / "manifest.json")
    with pytest.raises(SystemExit):
        bd.cmd_build(args)


def test_cmd_build_last_row_dropped_no_later_rows(tmp_path):
    # A single decision for the instrument has nothing later to resolve against.
    ledger_path = tmp_path / "ledger.jsonl"
    write_ledger(ledger_path, [{"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500",
                               "price": 100.0}])
    args = build_args(ledger_path, "SPX500", tmp_path / "out.csv", tmp_path / "manifest.json")
    with pytest.raises(SystemExit):
        bd.cmd_build(args)  # "no labelable rows"


def test_cmd_build_writes_dataset_and_manifest(tmp_path, capsys):
    ledger_path = tmp_path / "ledger.jsonl"
    rows = [
        {"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500", "price": 100.0,
         "strength": 55.0, "measured": 5, "unknowns": 1, "priorMoveDir": 1, "vwapSide": 1,
         "dir": 1, "regime": "trend"},
        {"at": "2024-01-01T00:05:00.000Z", "symbol": "SPX500", "price": 101.5},
        {"at": "2024-01-01T00:10:00.000Z", "symbol": "OTHER", "price": 999.0},
        {"at": "2024-01-01T00:15:00.000Z", "symbol": "SPX500", "price": 98.0,
         "strength": 60.0, "dir": -1, "regime": "range"},
        {"at": "2024-01-01T00:20:00.000Z", "symbol": "SPX500", "price": 96.5},
    ]
    write_ledger(ledger_path, rows)
    out_path = tmp_path / "out.csv"
    manifest_path = tmp_path / "manifest.json"
    args = build_args(ledger_path, "SPX500", out_path, manifest_path)
    rc = bd.cmd_build(args)
    assert rc == 0
    assert out_path.is_file()
    assert manifest_path.is_file()

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["version"] == bd.MANIFEST_VERSION
    assert manifest["instrument"] == "SPX500"
    assert manifest["features"] == bd.FEATURE_NAMES
    assert manifest["label_classes"] == bd.LABEL_CLASSES

    lines = out_path.read_text(encoding="utf-8").splitlines()
    header = lines[0].split(",")
    assert header == bd.META_COLUMNS + bd.FEATURE_NAMES
    # 4 SPX500 decisions (00:00, 00:05, 00:15, 00:20); only the last (00:20) has nothing later
    # to resolve against and is dropped -> 3 labelable rows.
    assert len(lines) - 1 == 3
    out = capsys.readouterr().out
    assert "wrote 3 rows" in out


def test_cmd_build_regime_one_hot_and_defaults(tmp_path):
    ledger_path = tmp_path / "ledger.jsonl"
    rows = [
        {"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500", "price": 100.0},  # no regime key
        {"at": "2024-01-01T00:05:00.000Z", "symbol": "SPX500", "price": 100.05},
    ]
    write_ledger(ledger_path, rows)
    out_path = tmp_path / "out.csv"
    manifest_path = tmp_path / "manifest.json"
    args = build_args(ledger_path, "SPX500", out_path, manifest_path)
    rc = bd.cmd_build(args)
    assert rc == 0
    lines = out_path.read_text(encoding="utf-8").splitlines()
    header = lines[0].split(",")
    row = lines[1].split(",")
    unknown_idx = header.index("regime_unknown")
    assert row[unknown_idx] == "1"
    trend_idx = header.index("regime_trend")
    assert row[trend_idx] == "0"


def test_cmd_build_max_bars_limits_window(tmp_path):
    ledger_path = tmp_path / "ledger.jsonl"
    rows = [{"at": f"2024-01-01T00:{i:02d}:00.000Z", "symbol": "SPX500", "price": 100.0 + i}
           for i in range(5)]
    write_ledger(ledger_path, rows)
    out_path = tmp_path / "out.csv"
    manifest_path = tmp_path / "manifest.json"
    args = build_args(ledger_path, "SPX500", out_path, manifest_path, max_bars=2)
    rc = bd.cmd_build(args)
    assert rc == 0
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["path_outcome_config"]["max_bars"] == 2


# --------------------------------------------------------------------------- main() CLI dispatch


def test_main_build_subcommand(tmp_path):
    ledger_path = tmp_path / "ledger.jsonl"
    write_ledger(ledger_path, [
        {"at": "2024-01-01T00:00:00.000Z", "symbol": "SPX500", "price": 100.0},
        {"at": "2024-01-01T00:05:00.000Z", "symbol": "SPX500", "price": 100.05},
    ])
    out_path = tmp_path / "out.csv"
    manifest_path = tmp_path / "manifest.json"
    rc = bd.main(["build", "--ledger", str(ledger_path), "--instrument", "SPX500",
                 "--out", str(out_path), "--manifest", str(manifest_path)])
    assert rc == 0
    assert out_path.is_file()


def test_main_splits_delegates_to_crowd_dataset(tmp_path, capsys):
    # bot_dataset's "splits" subcommand sets func=crowd_dataset.cmd_splits directly — verify
    # the delegation actually runs crowd_dataset's implementation end to end.
    dataset_path = tmp_path / "dataset.csv"
    n = 10
    from datetime import datetime, timedelta, timezone as tz
    base = datetime(2024, 1, 1, 21, 0, 0, tzinfo=tz.utc)
    with dataset_path.open("w", encoding="utf-8") as handle:
        handle.write("decision_time,label_end_time\n")
        for i in range(n):
            d = cd.to_utc_iso(base + timedelta(days=i))
            e = cd.to_utc_iso(base + timedelta(days=i + 1))
            handle.write(f"{d},{e}\n")
    rc = bd.main(["splits", "--dataset", str(dataset_path), "--folds", "2"])
    assert rc == 0
    out = capsys.readouterr().out
    parsed = json.loads(out)
    assert parsed["row_count"] == n


def test_main_requires_subcommand():
    with pytest.raises(SystemExit):
        bd.main([])
