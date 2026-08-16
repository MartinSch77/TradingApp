# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/ml/crowd_dataset.py — the crowd-model dataset builder.

Focuses on the three load-bearing rules the module's docstring states (as-of by received
time, missing-stays-missing, NO_TRADE as an outcome) plus the purged walk-forward splitter,
which is reused verbatim by bot_dataset.py and is therefore the single most consequential
function in this half of the pipeline.
"""

from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

import crowd_dataset as cd


# --------------------------------------------------------------------------- helpers


def make_store(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path))
    conn.execute(
        "CREATE TABLE observations (instrument TEXT, source TEXT, series_id TEXT, "
        "value REAL, received_time TEXT, valid INTEGER)")
    conn.commit()
    return conn


def insert_obs(conn, instrument, source, series_id, value, received_iso, valid=1):
    conn.execute(
        "INSERT INTO observations (instrument, source, series_id, value, received_time, valid) "
        "VALUES (?, ?, ?, ?, ?, ?)", (instrument, source, series_id, value, received_iso, valid))
    conn.commit()


def write_prices_csv(path: Path, rows: list[tuple[str, float]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        handle.write("date,close\n")
        for date, close in rows:
            handle.write(f"{date},{close}\n")


def daily_prices(n: int, start_price: float = 100.0,
                 pct_step: float = 0.0) -> list[tuple[str, float]]:
    """n strictly-increasing daily dates starting 2024-01-01. pct_step == 0.0 (default) gives
    a genuinely constant close, so per-bar returns and their 20-day stddev are exactly zero."""
    rows = []
    price = start_price
    base = datetime(2024, 1, 1)
    for i in range(n):
        date = (base + timedelta(days=i)).strftime("%Y-%m-%d")
        rows.append((date, price))
        price *= (1.0 + pct_step)
    return rows


# --------------------------------------------------------------------------- feature_names


def test_feature_names_order_and_count():
    names = cd.feature_names()
    assert names[:4] == cd.PRICE_FEATURES
    # 4 price features + 4 columns per series
    assert len(names) == len(cd.PRICE_FEATURES) + 4 * len(cd.SERIES)
    assert names[4:8] == ["retail_pct_long_value", "retail_pct_long_z",
                          "retail_pct_long_age_days", "retail_pct_long_measured"]


# --------------------------------------------------------------------------- to_utc_iso / from_utc_iso / fmt


def test_to_utc_iso_format():
    dt = datetime(2024, 3, 5, 21, 0, 0, 123000, tzinfo=timezone.utc)
    assert cd.to_utc_iso(dt) == "2024-03-05T21:00:00.123Z"


def test_to_utc_iso_converts_non_utc():
    from datetime import timedelta, timezone as tz
    est = tz(timedelta(hours=-5))
    dt = datetime(2024, 3, 5, 16, 0, 0, 0, tzinfo=est)
    assert cd.to_utc_iso(dt) == "2024-03-05T21:00:00.000Z"


def test_from_utc_iso_roundtrip():
    text = "2024-03-05T21:00:00.123Z"
    dt = cd.from_utc_iso(text)
    assert dt.year == 2024 and dt.hour == 21 and dt.tzinfo == timezone.utc
    assert cd.to_utc_iso(dt) == text


def test_fmt_deterministic():
    assert cd.fmt(1.0) == "1"
    assert cd.fmt(0.1 + 0.2) == "0.3"
    assert cd.fmt(123456789.123) == "123456789.1"


# --------------------------------------------------------------------------- fail


def test_fail_raises_systemexit_1(capsys):
    with pytest.raises(SystemExit) as excinfo:
        cd.fail("boom")
    assert excinfo.value.code == 1
    assert "boom" in capsys.readouterr().err


# --------------------------------------------------------------------------- load_prices


def test_load_prices_happy_path(tmp_path):
    p = tmp_path / "prices.csv"
    write_prices_csv(p, [("2024-01-01", 100.0), ("2024-01-02", 101.0)])
    rows = cd.load_prices(p)
    assert rows == [("2024-01-01", 100.0), ("2024-01-02", 101.0)]


def test_load_prices_bad_header(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("foo,bar\n2024-01-01,100\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


def test_load_prices_empty_file(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


def test_load_prices_short_row(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("date,close\n2024-01-01\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


def test_load_prices_unreadable_date(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("date,close\nnot-a-date,100\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


def test_load_prices_unreadable_close(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("date,close\n2024-01-01,abc\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


def test_load_prices_non_positive_close(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("date,close\n2024-01-01,0\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


def test_load_prices_non_increasing_dates(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("date,close\n2024-01-02,100\n2024-01-01,101\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


def test_load_prices_no_rows_after_header(tmp_path):
    p = tmp_path / "prices.csv"
    p.write_text("date,close\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.load_prices(p)


# --------------------------------------------------------------------------- zscore


def test_zscore_below_min_samples_is_none():
    assert cd.zscore(1.0, [1.0, 2.0], min_samples=3) is None


def test_zscore_constant_history_is_none():
    assert cd.zscore(1.0, [5.0, 5.0, 5.0], min_samples=3) is None


def test_zscore_computed():
    # history mean 2, variance ((1-2)^2+(2-2)^2+(3-2)^2)/3 = 2/3
    result = cd.zscore(4.0, [1.0, 2.0, 3.0], min_samples=3)
    assert result == pytest.approx((4.0 - 2.0) / (2.0 / 3.0) ** 0.5)


# --------------------------------------------------------------------------- as_of / history_before


def test_as_of_returns_latest_at_or_before(tmp_path):
    conn = make_store(tmp_path / "store.db")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 10.0, "2024-01-01T00:00:00.000Z")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 20.0, "2024-01-02T00:00:00.000Z")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 30.0, "2024-01-03T00:00:00.000Z")
    result = cd.as_of(conn, "SPX500", "retail", "IG-PCT-LONG", "2024-01-02T12:00:00.000Z")
    assert result == (20.0, "2024-01-02T00:00:00.000Z")
    conn.close()


def test_as_of_none_when_nothing_before(tmp_path):
    conn = make_store(tmp_path / "store.db")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 10.0, "2024-01-05T00:00:00.000Z")
    result = cd.as_of(conn, "SPX500", "retail", "IG-PCT-LONG", "2024-01-01T00:00:00.000Z")
    assert result is None
    conn.close()


def test_as_of_ignores_invalid_rows(tmp_path):
    conn = make_store(tmp_path / "store.db")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 10.0, "2024-01-01T00:00:00.000Z", valid=0)
    result = cd.as_of(conn, "SPX500", "retail", "IG-PCT-LONG", "2024-01-02T00:00:00.000Z")
    assert result is None
    conn.close()


def test_as_of_ignores_other_instrument_source_series(tmp_path):
    conn = make_store(tmp_path / "store.db")
    insert_obs(conn, "OTHER", "retail", "IG-PCT-LONG", 10.0, "2024-01-01T00:00:00.000Z")
    insert_obs(conn, "SPX500", "options", "IG-PCT-LONG", 10.0, "2024-01-01T00:00:00.000Z")
    insert_obs(conn, "SPX500", "retail", "OTHER-SERIES", 10.0, "2024-01-01T00:00:00.000Z")
    result = cd.as_of(conn, "SPX500", "retail", "IG-PCT-LONG", "2024-01-02T00:00:00.000Z")
    assert result is None
    conn.close()


def test_history_before_excludes_at_or_after(tmp_path):
    conn = make_store(tmp_path / "store.db")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 10.0, "2024-01-01T00:00:00.000Z")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 20.0, "2024-01-02T00:00:00.000Z")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 30.0, "2024-01-03T00:00:00.000Z")
    values = cd.history_before(conn, "SPX500", "retail", "IG-PCT-LONG", "2024-01-03T00:00:00.000Z")
    assert values == [10.0, 20.0]
    conn.close()


def test_history_before_empty_when_nothing_earlier(tmp_path):
    conn = make_store(tmp_path / "store.db")
    insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 10.0, "2024-01-05T00:00:00.000Z")
    values = cd.history_before(conn, "SPX500", "retail", "IG-PCT-LONG", "2024-01-01T00:00:00.000Z")
    assert values == []
    conn.close()


# --------------------------------------------------------------------------- price_features


def test_price_features_insufficient_history_is_none():
    prices = daily_prices(3)
    features = cd.price_features(prices, 0)
    assert features["ret_1d_pct"] is None
    assert features["ret_5d_pct"] is None
    assert features["ret_20d_pct"] is None
    assert features["vol_20d_pct"] is None


def test_price_features_ret_1d_computed_ret_5d_still_none():
    prices = daily_prices(3)
    features = cd.price_features(prices, 1)
    assert features["ret_1d_pct"] is not None
    assert features["ret_5d_pct"] is None


def test_price_features_all_present_at_index_20():
    prices = daily_prices(25)
    features = cd.price_features(prices, 20)
    assert features["ret_1d_pct"] is not None
    assert features["ret_5d_pct"] is not None
    assert features["ret_20d_pct"] is not None
    assert features["vol_20d_pct"] is not None
    # linear price series -> constant daily return -> zero volatility
    assert features["vol_20d_pct"] == pytest.approx(0.0, abs=1e-9)


# --------------------------------------------------------------------------- label_for


def test_label_for_long():
    assert cd.label_for(1.0, 0.25) == "LONG"


def test_label_for_short():
    assert cd.label_for(-1.0, 0.25) == "SHORT"


def test_label_for_no_trade_inside_dead_zone():
    assert cd.label_for(0.1, 0.25) == "NO_TRADE"


def test_label_for_boundary_is_no_trade():
    # strictly greater/less required; exactly at the dead zone is NO_TRADE
    assert cd.label_for(0.25, 0.25) == "NO_TRADE"
    assert cd.label_for(-0.25, 0.25) == "NO_TRADE"


# --------------------------------------------------------------------------- cmd_build (integration)


class Args:
    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)


def build_args(store, instrument, prices, out, manifest, **overrides):
    defaults = dict(store=str(store), instrument=instrument, prices=str(prices),
                    out=str(out), manifest=str(manifest), horizon_days=5,
                    dead_zone_pct=0.25, decision_hour_utc=21, z_min_history=3)
    defaults.update(overrides)
    return Args(**defaults)


def test_cmd_build_store_not_found(tmp_path):
    args = build_args(tmp_path / "missing.db", "SPX500", tmp_path / "prices.csv",
                      tmp_path / "out.csv", tmp_path / "manifest.json")
    with pytest.raises(SystemExit):
        cd.cmd_build(args)


def test_cmd_build_insufficient_price_rows(tmp_path):
    store_path = tmp_path / "store.db"
    make_store(store_path).close()
    prices_path = tmp_path / "prices.csv"
    write_prices_csv(prices_path, daily_prices(3))
    args = build_args(store_path, "SPX500", prices_path, tmp_path / "out.csv",
                      tmp_path / "manifest.json", horizon_days=5)
    with pytest.raises(SystemExit):
        cd.cmd_build(args)


def test_cmd_build_writes_dataset_and_manifest(tmp_path, capsys):
    store_path = tmp_path / "store.db"
    conn = make_store(store_path)
    prices = daily_prices(30)
    prices_path = tmp_path / "prices.csv"
    write_prices_csv(prices_path, prices)

    # Insert some observations at various times so some rows are measured, some not.
    decision0 = datetime.strptime(prices[0][0], "%Y-%m-%d").replace(hour=21, tzinfo=timezone.utc)
    for i in range(5):
        received = cd.to_utc_iso(decision0)
        insert_obs(conn, "SPX500", "retail", "IG-PCT-LONG", 50.0 + i,
                  cd.to_utc_iso(decision0.replace(day=max(1, decision0.day))))
    conn.close()

    out_path = tmp_path / "out.csv"
    manifest_path = tmp_path / "manifest.json"
    args = build_args(store_path, "SPX500", prices_path, out_path, manifest_path, horizon_days=5)
    rc = cd.cmd_build(args)
    assert rc == 0
    assert out_path.is_file()
    assert manifest_path.is_file()

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["version"] == cd.MANIFEST_VERSION
    assert manifest["instrument"] == "SPX500"
    assert manifest["features"] == cd.feature_names()
    assert manifest["label_classes"] == cd.LABEL_CLASSES

    lines = out_path.read_text(encoding="utf-8").splitlines()
    header = lines[0].split(",")
    assert header == cd.META_COLUMNS + cd.feature_names()
    # 30 prices - 5 horizon = 25 rows
    assert len(lines) - 1 == 25
    out = capsys.readouterr().out
    assert "wrote 25 rows" in out


def test_cmd_build_smallest_surviving_horizon_still_builds(tmp_path, monkeypatch):
    # horizon_days == len(prices) - 1 is the smallest horizon that survives the
    # `len(prices) <= horizon_days` guard, leaving exactly one row. Confirms the
    # boundary builds successfully — cmd_build's own "no labelable rows" guard
    # (removed 2026-08-16: unreachable, since the loop this checks always runs
    # range(len(prices) - horizon_days) >= 1 times once that earlier guard has
    # passed, and every iteration unconditionally appends a row) no longer exists,
    # so there is nothing left to trigger at this boundary but a normal build.
    store_path = tmp_path / "store.db"
    make_store(store_path).close()
    prices = daily_prices(6)
    prices_path = tmp_path / "prices.csv"
    write_prices_csv(prices_path, prices)
    out_path = tmp_path / "out.csv"
    manifest_path = tmp_path / "manifest.json"
    args = build_args(store_path, "SPX500", prices_path, out_path, manifest_path, horizon_days=5)
    rc = cd.cmd_build(args)
    assert rc == 0
    lines = out_path.read_text(encoding="utf-8").splitlines()
    assert len(lines) - 1 == 1


# --------------------------------------------------------------------------- read_dataset_meta


def test_read_dataset_meta_happy_path(tmp_path):
    p = tmp_path / "dataset.csv"
    p.write_text(
        "decision_time,label_end_time\n"
        "2024-01-01T21:00:00.000Z,2024-01-06T21:00:00.000Z\n"
        "2024-01-02T21:00:00.000Z,2024-01-07T21:00:00.000Z\n",
        encoding="utf-8")
    decisions, ends = cd.read_dataset_meta(p)
    assert decisions == ["2024-01-01T21:00:00.000Z", "2024-01-02T21:00:00.000Z"]
    assert ends == ["2024-01-06T21:00:00.000Z", "2024-01-07T21:00:00.000Z"]


def test_read_dataset_meta_missing_columns(tmp_path):
    p = tmp_path / "dataset.csv"
    p.write_text("foo,bar\n1,2\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.read_dataset_meta(p)


def test_read_dataset_meta_empty_file(tmp_path):
    p = tmp_path / "dataset.csv"
    p.write_text("", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.read_dataset_meta(p)


def test_read_dataset_meta_no_rows(tmp_path):
    p = tmp_path / "dataset.csv"
    p.write_text("decision_time,label_end_time\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.read_dataset_meta(p)


def test_read_dataset_meta_not_time_ordered(tmp_path):
    p = tmp_path / "dataset.csv"
    p.write_text(
        "decision_time,label_end_time\n"
        "2024-01-02T21:00:00.000Z,2024-01-07T21:00:00.000Z\n"
        "2024-01-01T21:00:00.000Z,2024-01-06T21:00:00.000Z\n",
        encoding="utf-8")
    with pytest.raises(SystemExit):
        cd.read_dataset_meta(p)


# --------------------------------------------------------------------------- walk_forward_splits


def iso_days(n: int, start_offset: int = 0):
    """n UTC ISO timestamps, one per day, offset from a fixed epoch by `start_offset` days."""
    base = datetime(2024, 1, 1, 21, 0, 0, tzinfo=timezone.utc) + timedelta(days=start_offset)
    return [cd.to_utc_iso(base + timedelta(days=i)) for i in range(n)]


def test_walk_forward_splits_too_few_rows():
    with pytest.raises(ValueError):
        cd.walk_forward_splits(["x"], ["y"], folds=4, embargo_days=1.0, min_train_fraction=0.5)


def test_walk_forward_splits_no_validation_rows():
    decisions = iso_days(2)
    ends = iso_days(2, start_offset=6)
    with pytest.raises(ValueError):
        cd.walk_forward_splits(decisions, ends, folds=4, embargo_days=1.0,
                               min_train_fraction=1.0)


def test_walk_forward_splits_basic_shape():
    n = 40
    decisions = iso_days(n)
    ends = iso_days(n, start_offset=6)  # each row's label ends 5 days later
    result = cd.walk_forward_splits(decisions, ends, folds=4, embargo_days=1.0,
                                    min_train_fraction=0.5)
    assert result["row_count"] == n
    assert result["embargo_days"] == 1.0
    assert result["min_train_fraction"] == 0.5
    assert len(result["folds"]) <= 4
    # folds are contiguous, non-overlapping, and cover the validation tail
    seen = set()
    for fold in result["folds"]:
        for i in fold["val_rows"]:
            assert i not in seen
            seen.add(i)
    assert seen  # at least one row validated
    assert max(seen) == n - 1


def test_walk_forward_splits_purges_straddling_rows():
    # Rows whose label window (end + embargo) reaches into the validation block must be
    # excluded from that fold's training set.
    n = 20
    decisions = iso_days(n)
    # Make every row's label end far in the future (20 days after decision) so it always
    # straddles into any later validation block within our small n.
    ends = iso_days(n, start_offset=100)
    result = cd.walk_forward_splits(decisions, ends, folds=2, embargo_days=1.0,
                                    min_train_fraction=0.5)
    for fold in result["folds"]:
        # With such a large label horizon, virtually all earlier rows should be purged.
        assert fold["purged"] >= 0
        assert fold["train_rows"] == [] or all(
            cd.from_utc_iso(ends[i]) + __import__("datetime").timedelta(days=1.0)
            < cd.from_utc_iso(decisions[fold["val_rows"][0]])
            for i in fold["train_rows"])


def test_walk_forward_splits_folds_clamped_to_tail():
    # folds requested larger than available tail rows should be clamped, not error.
    n = 4
    decisions = iso_days(n)
    ends = iso_days(n, start_offset=6)
    result = cd.walk_forward_splits(decisions, ends, folds=100, embargo_days=0.0,
                                    min_train_fraction=0.5)
    assert len(result["folds"]) <= 2  # tail is at most 2 rows here


def test_walk_forward_splits_zero_embargo_allows_adjacent_training():
    n = 10
    decisions = iso_days(n)
    ends = iso_days(n)  # label ends same day as decision -> no embargo needed
    result = cd.walk_forward_splits(decisions, ends, folds=2, embargo_days=0.0,
                                    min_train_fraction=0.5)
    total_train = sum(len(f["train_rows"]) for f in result["folds"])
    assert total_train > 0


def test_walk_forward_splits_skips_empty_start_eq_end_folds():
    # Very small tail with many folds requested -> some boundaries collapse (start >= end),
    # exercising the "continue" branch.
    n = 3
    decisions = iso_days(n)
    ends = iso_days(n)
    result = cd.walk_forward_splits(decisions, ends, folds=10, embargo_days=0.0,
                                    min_train_fraction=0.9)
    # folds count in output may be fewer than requested due to collapsed boundaries
    assert len(result["folds"]) >= 1


# --------------------------------------------------------------------------- cmd_splits


def test_cmd_splits_writes_stdout(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    n = 20
    decisions = iso_days(n)
    ends = iso_days(n, start_offset=6)
    with dataset_path.open("w", encoding="utf-8") as handle:
        handle.write("decision_time,label_end_time\n")
        for d, e in zip(decisions, ends):
            handle.write(f"{d},{e}\n")
    args = Args(dataset=str(dataset_path), folds=2, embargo_days=1.0, min_train_fraction=0.5,
               out=None)
    rc = cd.cmd_splits(args)
    assert rc == 0
    out = capsys.readouterr().out
    parsed = json.loads(out)
    assert parsed["row_count"] == n


def test_cmd_splits_writes_file(tmp_path):
    dataset_path = tmp_path / "dataset.csv"
    n = 20
    decisions = iso_days(n)
    ends = iso_days(n, start_offset=6)
    with dataset_path.open("w", encoding="utf-8") as handle:
        handle.write("decision_time,label_end_time\n")
        for d, e in zip(decisions, ends):
            handle.write(f"{d},{e}\n")
    out_path = tmp_path / "splits.json"
    args = Args(dataset=str(dataset_path), folds=2, embargo_days=1.0, min_train_fraction=0.5,
               out=str(out_path))
    rc = cd.cmd_splits(args)
    assert rc == 0
    assert out_path.is_file()
    parsed = json.loads(out_path.read_text(encoding="utf-8"))
    assert parsed["row_count"] == n


def test_cmd_splits_propagates_value_error_as_systemexit(tmp_path):
    dataset_path = tmp_path / "dataset.csv"
    dataset_path.write_text("decision_time,label_end_time\n2024-01-01T21:00:00.000Z,"
                            "2024-01-06T21:00:00.000Z\n", encoding="utf-8")
    args = Args(dataset=str(dataset_path), folds=4, embargo_days=1.0, min_train_fraction=0.5,
               out=None)
    with pytest.raises(SystemExit):
        cd.cmd_splits(args)


# --------------------------------------------------------------------------- cmd_fetch_prices


def test_cmd_fetch_prices_writes_csv(tmp_path, monkeypatch):
    class FakeReply:
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def read(self):
            return b""

    payload = {
        "chart": {"result": [{
            "timestamp": [1704067200, 1704153600, 1704240000],  # 2024-01-01..03 UTC midnight
            "indicators": {"quote": [{"close": [100.0, None, 102.0]}]},
        }]}
    }

    def fake_urlopen(request, timeout=30):
        return FakeReplyWithJson(payload)

    class FakeReplyWithJson:
        def __init__(self, data):
            self._data = data

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def read(self):
            return json.dumps(self._data).encode("utf-8")

    monkeypatch.setattr(cd.urllib.request, "urlopen",
                        lambda request, timeout=30: FakeReplyWithJson(payload))
    out_path = tmp_path / "prices.csv"
    args = Args(symbol="^GSPC", years=1, out=str(out_path))
    rc = cd.cmd_fetch_prices(args)
    assert rc == 0
    lines = out_path.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "date,close"
    # the None close is dropped -> 2 rows
    assert len(lines) - 1 == 2


def test_cmd_fetch_prices_network_error(tmp_path, monkeypatch):
    def fake_urlopen(request, timeout=30):
        raise OSError("network down")

    monkeypatch.setattr(cd.urllib.request, "urlopen", fake_urlopen)
    args = Args(symbol="^GSPC", years=1, out=str(tmp_path / "prices.csv"))
    with pytest.raises(SystemExit):
        cd.cmd_fetch_prices(args)


def test_cmd_fetch_prices_bad_payload_shape(tmp_path, monkeypatch):
    class FakeReply:
        def __init__(self, data):
            self._data = data

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def read(self):
            return json.dumps(self._data).encode("utf-8")

    monkeypatch.setattr(cd.urllib.request, "urlopen",
                        lambda request, timeout=30: FakeReply({"chart": {"result": []}}))
    args = Args(symbol="^GSPC", years=1, out=str(tmp_path / "prices.csv"))
    with pytest.raises(SystemExit):
        cd.cmd_fetch_prices(args)


def test_cmd_fetch_prices_no_usable_closes(tmp_path, monkeypatch):
    class FakeReply:
        def __init__(self, data):
            self._data = data

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def read(self):
            return json.dumps(self._data).encode("utf-8")

    payload = {"chart": {"result": [{"timestamp": [1704067200],
                                     "indicators": {"quote": [{"close": [None]}]}}]}}
    monkeypatch.setattr(cd.urllib.request, "urlopen",
                        lambda request, timeout=30: FakeReply(payload))
    args = Args(symbol="^GSPC", years=1, out=str(tmp_path / "prices.csv"))
    with pytest.raises(SystemExit):
        cd.cmd_fetch_prices(args)


# --------------------------------------------------------------------------- main() CLI dispatch


def test_main_splits_subcommand(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    n = 20
    decisions = iso_days(n)
    ends = iso_days(n, start_offset=6)
    with dataset_path.open("w", encoding="utf-8") as handle:
        handle.write("decision_time,label_end_time\n")
        for d, e in zip(decisions, ends):
            handle.write(f"{d},{e}\n")
    rc = cd.main(["splits", "--dataset", str(dataset_path), "--folds", "2"])
    assert rc == 0


def test_main_requires_subcommand():
    with pytest.raises(SystemExit):
        cd.main([])


def test_main_build_subcommand(tmp_path):
    store_path = tmp_path / "store.db"
    make_store(store_path).close()
    prices = daily_prices(10)
    prices_path = tmp_path / "prices.csv"
    write_prices_csv(prices_path, prices)
    out_path = tmp_path / "out.csv"
    manifest_path = tmp_path / "manifest.json"
    rc = cd.main(["build", "--store", str(store_path), "--instrument", "SPX500",
                 "--prices", str(prices_path), "--out", str(out_path),
                 "--manifest", str(manifest_path), "--horizon-days", "2"])
    assert rc == 0
    assert out_path.is_file()
