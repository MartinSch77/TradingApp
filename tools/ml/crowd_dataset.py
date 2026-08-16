#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Build the crowd-model training dataset from the app's own observation store (REQ-F-041).

Phase 4 of the crowd subsystem is an OFFLINE pipeline: this tool reads the SQLite store the
C++ application writes (src/services/CrowdStore.cpp, the RAW observation layer of REQ-F-039),
joins it to a daily price series, and emits a labelled CSV plus a versioned feature manifest.
Python is a development-time tool here — the app never imports, shells out to, or depends on it.

    tools/ml/crowd_dataset.py build --store crowd.db --instrument SPX500 \\
        --prices prices.csv --out dataset.csv --manifest manifest.json
    tools/ml/crowd_dataset.py splits --dataset dataset.csv --folds 4 --embargo-days 2
    tools/ml/crowd_dataset.py fetch-prices --symbol ^GSPC --years 10 --out prices.csv

This half of the pipeline is deliberately STDLIB-ONLY (sqlite3, csv, json), so the rules that
make the dataset honest are testable on every machine the project supports — the model-fitting
half (train_crowd_model.py) is the only part that needs the optional `./setup.sh ml` environment.

Three rules are load-bearing, and they matter more than any model fitted downstream:

  * AS-OF BY RECEIVED TIME. A datum enters a row only when its received (publication) time is
    at or before the row's decision time. The store keeps event time and received time apart
    precisely so this join is checkable: a COT report about a Tuesday, released the Friday
    after, must never inform a Wednesday decision. Per-series z-scores are normalized against
    values received strictly BEFORE the datum they score — the same past-only discipline as the
    Phase 2 crowd score (src/domain/RollingZScore.cpp).
  * MISSING STAYS MISSING. An unmeasured feature is an EMPTY cell beside a 0/1 `_measured`
    marker, never a zero — a zero z-score is a real claim ("exactly average"), which an absent
    series never made. Rows whose label horizon runs past the price history are DROPPED, never
    labelled by invention.
  * NO_TRADE IS AN OUTCOME. Labels are LONG / NO_TRADE / SHORT from the forward return over a
    configurable horizon with a DEAD ZONE representing the round-trip cost: a move that would
    not clear its own cost is a NO_TRADE, so the model is taught that staying out is an answer.

The outputs carry no wall-clock timestamps, so the same inputs produce byte-identical files —
a property the test suite pins (tst_crowdml.cpp).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sqlite3
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

MANIFEST_VERSION = 1

# Class order is part of the contract with the trainer and the exported model.
LABEL_CLASSES = ["SHORT", "NO_TRADE", "LONG"]

# The series the dataset reads: the four families the Phase 2 score combines
# (src/services/CrowdScoreBuilder.cpp — retail is read contrarian THERE, the dataset stores the
# RAW z and leaves orientation to the consumer) plus the second COT leg and the VIX level the
# real providers emit. (source string as the store persists it, series id, column prefix.)
SERIES = [
    ("retail", "IG-PCT-LONG", "retail_pct_long"),
    ("options", "PUT-CALL", "put_call"),
    ("volatility", "VIX", "vix"),
    ("institutional", "COT-ASSET-MGR-NET", "cot_asset_mgr_net"),
    ("institutional", "COT-LEV-FUND-NET", "cot_lev_fund_net"),
    ("social", "NET-SENTIMENT", "social_net_sentiment"),
]

PRICE_FEATURES = ["ret_1d_pct", "ret_5d_pct", "ret_20d_pct", "vol_20d_pct"]

META_COLUMNS = ["instrument", "decision_time", "close", "label_end_time",
                "forward_return_pct", "label"]


def feature_names() -> list[str]:
    """The one canonical feature order. APPEND-ONLY: consumers match columns by name, and a
    reordering would silently repoint every model trained before it."""
    names = list(PRICE_FEATURES)
    for _source, _series, prefix in SERIES:
        names += [f"{prefix}_value", f"{prefix}_z", f"{prefix}_age_days", f"{prefix}_measured"]
    return names


def to_utc_iso(dt: datetime) -> str:
    """The exact format CrowdStore writes (Qt::ISODateWithMs, UTC, trailing Z) — fixed width,
    so string comparison in SQL orders the same way the timestamps do."""
    dt = dt.astimezone(timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


def from_utc_iso(text: str) -> datetime:
    return datetime.fromisoformat(text.replace("Z", "+00:00")).astimezone(timezone.utc)


def fmt(value: float) -> str:
    """Deterministic number formatting for the CSV."""
    return format(value, ".10g")


def fail(message: str) -> "None":
    print(f"crowd_dataset: {message}", file=sys.stderr)
    raise SystemExit(1)


# --------------------------------------------------------------------------- prices


def load_prices(path: Path) -> list[tuple[str, float]]:
    """A daily close series as (ISO date, close), strictly increasing dates, closes > 0."""
    rows: list[tuple[str, float]] = []
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None or [h.strip() for h in header[:2]] != ["date", "close"]:
            fail(f"{path}: expected a 'date,close' header")
        for line, row in enumerate(reader, start=2):
            if len(row) < 2:
                fail(f"{path}:{line}: expected 'date,close'")
            date, close_text = row[0].strip(), row[1].strip()
            try:
                datetime.strptime(date, "%Y-%m-%d")
                close = float(close_text)
            except ValueError:
                fail(f"{path}:{line}: unreadable date or close ({row[0]!r}, {row[1]!r})")
            if close <= 0.0:
                fail(f"{path}:{line}: non-positive close {close}")
            if rows and date <= rows[-1][0]:
                fail(f"{path}:{line}: dates must be strictly increasing")
            rows.append((date, close))
    if not rows:
        fail(f"{path}: no price rows")
    return rows


def cmd_fetch_prices(args: argparse.Namespace) -> int:
    """Daily closes from Yahoo's keyless chart API — the same public feed the app already uses
    for its reference series. Network happens ONLY here, on explicit request; no test calls it."""
    symbol = urllib.parse.quote(args.symbol, safe="")
    url = (f"https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
           f"?interval=1d&range={args.years}y")
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (TradingApp ml)"})
    try:
        with urllib.request.urlopen(request, timeout=30) as reply:
            payload = json.load(reply)
    except Exception as error:  # noqa: BLE001 — one named failure, whatever the transport said
        fail(f"fetching {args.symbol}: {error}")
    try:
        result = payload["chart"]["result"][0]
        stamps = result["timestamp"]
        closes = result["indicators"]["quote"][0]["close"]
    except (KeyError, IndexError, TypeError):
        fail(f"{args.symbol}: unexpected chart payload shape")
    rows: dict[str, float] = {}
    for stamp, close in zip(stamps, closes):
        if close is None:  # a null close is a missing print, not a zero — the "." discipline
            continue
        date = datetime.fromtimestamp(stamp, tz=timezone.utc).strftime("%Y-%m-%d")
        rows[date] = float(close)
    if not rows:
        fail(f"{args.symbol}: the chart payload held no usable closes")
    out = Path(args.out)
    with out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["date", "close"])
        for date in sorted(rows):
            writer.writerow([date, fmt(rows[date])])
    print(f"wrote {len(rows)} daily closes for {args.symbol} to {out}")
    return 0


# --------------------------------------------------------------------------- features


def zscore(value: float, history: list[float], min_samples: int) -> float | None:
    """Population z against PRIOR history — the same rule as src/domain/RollingZScore.cpp:
    nothing below `min_samples`, nothing for a spreadless (constant) history."""
    if len(history) < min_samples:
        return None
    mean = sum(history) / len(history)
    variance = sum((v - mean) ** 2 for v in history) / len(history)
    if variance <= 0.0:
        return None
    return (value - mean) / math.sqrt(variance)


def as_of(conn: sqlite3.Connection, instrument: str, source: str, series_id: str,
          decision_iso: str) -> tuple[float, str] | None:
    """The newest VALID observation of a series whose received time is at or before the
    decision time — the one join that keeps look-ahead out of the dataset."""
    row = conn.execute(
        "SELECT value, received_time FROM observations"
        " WHERE instrument = ? AND source = ? AND series_id = ? AND valid = 1"
        "   AND received_time <= ?"
        " ORDER BY received_time DESC LIMIT 1",
        (instrument, source, series_id, decision_iso)).fetchone()
    return (float(row[0]), str(row[1])) if row is not None else None


def history_before(conn: sqlite3.Connection, instrument: str, source: str, series_id: str,
                   before_iso: str) -> list[float]:
    """The values received strictly BEFORE a datum — what its z is normalized against
    (mirrors CrowdStore::seriesValuesBefore, oldest first)."""
    rows = conn.execute(
        "SELECT value FROM observations"
        " WHERE instrument = ? AND source = ? AND series_id = ? AND valid = 1"
        "   AND received_time < ?"
        " ORDER BY received_time ASC",
        (instrument, source, series_id, before_iso)).fetchall()
    return [float(r[0]) for r in rows]


def price_features(prices: list[tuple[str, float]], index: int) -> dict[str, float | None]:
    """Past-only price context: returns over 1/5/20 rows and the population stddev of the last
    20 daily returns. Insufficient history is None (an empty cell), never zero."""
    close = prices[index][1]
    out: dict[str, float | None] = {}
    for name, lag in (("ret_1d_pct", 1), ("ret_5d_pct", 5), ("ret_20d_pct", 20)):
        out[name] = (close / prices[index - lag][1] - 1.0) * 100.0 if index >= lag else None
    if index >= 20:
        rets = [(prices[i][1] / prices[i - 1][1] - 1.0) * 100.0
                for i in range(index - 19, index + 1)]
        mean = sum(rets) / len(rets)
        out["vol_20d_pct"] = math.sqrt(sum((r - mean) ** 2 for r in rets) / len(rets))
    else:
        out["vol_20d_pct"] = None
    return out


def label_for(forward_return_pct: float, dead_zone_pct: float) -> str:
    if forward_return_pct > dead_zone_pct:
        return "LONG"
    if forward_return_pct < -dead_zone_pct:
        return "SHORT"
    return "NO_TRADE"


# --------------------------------------------------------------------------- build


def cmd_build(args: argparse.Namespace) -> int:
    store = Path(args.store)
    if not store.is_file():
        fail(f"store not found: {store}")
    prices = load_prices(Path(args.prices))
    if len(prices) <= args.horizon_days:
        fail(f"only {len(prices)} price rows — nothing survives a {args.horizon_days}-day horizon")

    names = feature_names()
    conn = sqlite3.connect(f"file:{store}?mode=ro", uri=True)
    try:
        rows_out: list[list[str]] = []
        for index in range(len(prices) - args.horizon_days):
            date, close = prices[index]
            end_date, end_close = prices[index + args.horizon_days]
            decision = datetime.strptime(date, "%Y-%m-%d").replace(
                hour=args.decision_hour_utc, tzinfo=timezone.utc)
            decision_iso = to_utc_iso(decision)
            label_end = datetime.strptime(end_date, "%Y-%m-%d").replace(
                hour=args.decision_hour_utc, tzinfo=timezone.utc)
            forward = (end_close / close - 1.0) * 100.0

            values: dict[str, float | None] = price_features(prices, index)
            for source, series_id, prefix in SERIES:
                latest = as_of(conn, args.instrument, source, series_id, decision_iso)
                if latest is None:
                    values[f"{prefix}_value"] = None
                    values[f"{prefix}_z"] = None
                    values[f"{prefix}_age_days"] = None
                    values[f"{prefix}_measured"] = 0.0
                    continue
                value, received_iso = latest
                prior = history_before(conn, args.instrument, source, series_id, received_iso)
                values[f"{prefix}_value"] = value
                values[f"{prefix}_z"] = zscore(value, prior, args.z_min_history)
                values[f"{prefix}_age_days"] = ((decision - from_utc_iso(received_iso))
                                                .total_seconds() / 86400.0)
                values[f"{prefix}_measured"] = 1.0

            row = [args.instrument, decision_iso, fmt(close), to_utc_iso(label_end),
                   fmt(forward), label_for(forward, args.dead_zone_pct)]
            row += ["" if values[n] is None else fmt(values[n]) for n in names]
            rows_out.append(row)
    finally:
        conn.close()
    # rows_out is never empty here: the loop above runs range(len(prices) -
    # args.horizon_days) times, unconditionally appending one row per
    # iteration, and the earlier `len(prices) <= args.horizon_days` guard
    # already refused the only case that range would be empty.

    out = Path(args.out)
    with out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(META_COLUMNS + names)
        writer.writerows(rows_out)

    manifest = {
        "version": MANIFEST_VERSION,
        "instrument": args.instrument,
        "horizon_days": args.horizon_days,
        "dead_zone_pct": args.dead_zone_pct,
        "decision_hour_utc": args.decision_hour_utc,
        "z_min_history": args.z_min_history,
        "label_classes": LABEL_CLASSES,
        "meta_columns": META_COLUMNS,
        "features": names,
        "series": [{"source": s, "series_id": i, "prefix": p} for s, i, p in SERIES],
    }
    Path(args.manifest).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8")
    labels = [r[META_COLUMNS.index("label")] for r in rows_out]
    counts = {c: labels.count(c) for c in LABEL_CLASSES}
    print(f"wrote {len(rows_out)} rows to {out} "
          f"(LONG {counts['LONG']}, NO_TRADE {counts['NO_TRADE']}, SHORT {counts['SHORT']}); "
          f"manifest {args.manifest}")
    return 0


# --------------------------------------------------------------------------- splits


def walk_forward_splits(decision_times: list[str], label_end_times: list[str], folds: int,
                        embargo_days: float, min_train_fraction: float) -> dict:
    """Purged walk-forward folds over TIME-ORDERED rows.

    The tail after `min_train_fraction` is cut into `folds` contiguous validation blocks. For
    each block, training rows are every EARLIER row whose label window — its label end time
    plus the embargo — closes strictly before the block begins. Rows straddling the boundary
    are PURGED: their labels are resolved by prices inside the validation block, and a random
    split (or an unpurged one) would let the model train on its own evaluation future.
    """
    count = len(decision_times)
    if count < 2:
        raise ValueError("too few rows to split")
    first_val = max(int(count * min_train_fraction), 1)
    tail = count - first_val
    if tail < 1:
        raise ValueError("min-train-fraction leaves no validation rows")
    folds = max(1, min(folds, tail))
    boundaries = [first_val + round(k * tail / folds) for k in range(folds + 1)]
    embargo = timedelta(days=embargo_days)

    out_folds = []
    for k in range(folds):
        start, end = boundaries[k], boundaries[k + 1]
        if start >= end:
            continue
        val_start = from_utc_iso(decision_times[start])
        train = [i for i in range(start)
                 if from_utc_iso(label_end_times[i]) + embargo < val_start]
        out_folds.append({
            "index": len(out_folds),
            "train_rows": train,
            "val_rows": list(range(start, end)),
            "val_start": decision_times[start],
            "val_end": decision_times[end - 1],
            "purged": start - len(train),
        })
    return {"embargo_days": embargo_days, "min_train_fraction": min_train_fraction,
            "row_count": count, "folds": out_folds}


def read_dataset_meta(path: Path) -> tuple[list[str], list[str]]:
    """decision_time and label_end_time columns of a built dataset, order preserved."""
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None or "decision_time" not in header or "label_end_time" not in header:
            fail(f"{path}: not a crowd dataset (missing meta columns)")
        d_col, e_col = header.index("decision_time"), header.index("label_end_time")
        decisions, ends = [], []
        previous = ""
        for row in reader:
            if row[d_col] < previous:
                fail(f"{path}: rows are not time-ordered at {row[d_col]}")
            previous = row[d_col]
            decisions.append(row[d_col])
            ends.append(row[e_col])
    if not decisions:
        fail(f"{path}: no rows")
    return decisions, ends


def cmd_splits(args: argparse.Namespace) -> int:
    decisions, ends = read_dataset_meta(Path(args.dataset))
    try:
        result = walk_forward_splits(decisions, ends, args.folds, args.embargo_days,
                                     args.min_train_fraction)
    except ValueError as error:
        fail(str(error))
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


# --------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="store + prices -> labelled dataset + manifest")
    build.add_argument("--store", required=True, help="the app's crowd SQLite store")
    build.add_argument("--instrument", required=True, help="e.g. SPX500")
    build.add_argument("--prices", required=True, help="CSV date,close (daily)")
    build.add_argument("--out", required=True, help="dataset CSV to write")
    build.add_argument("--manifest", required=True, help="feature manifest JSON to write")
    build.add_argument("--horizon-days", type=int, default=5,
                       help="label horizon in price ROWS (trading days; default 5)")
    build.add_argument("--dead-zone-pct", type=float, default=0.25,
                       help="|forward return| that must be cleared to label a direction — "
                            "the round-trip cost stand-in (default 0.25)")
    build.add_argument("--decision-hour-utc", type=int, default=21, choices=range(24),
                       help="decision time of day, UTC (default 21: after the US cash close)")
    build.add_argument("--z-min-history", type=int, default=3,
                       help="prior samples required before a z is trusted (default 3, "
                            "matching the Phase 2 score)")
    build.set_defaults(func=cmd_build)

    splits = sub.add_parser("splits", help="purged walk-forward folds for a built dataset")
    splits.add_argument("--dataset", required=True)
    splits.add_argument("--folds", type=int, default=4)
    splits.add_argument("--embargo-days", type=float, default=2.0)
    splits.add_argument("--min-train-fraction", type=float, default=0.5)
    splits.add_argument("--out", help="write JSON here instead of stdout")
    splits.set_defaults(func=cmd_splits)

    fetch = sub.add_parser("fetch-prices", help="daily closes from Yahoo's keyless chart API")
    fetch.add_argument("--symbol", required=True, help="Yahoo symbol, e.g. ^GSPC or ^NDX")
    fetch.add_argument("--years", type=int, default=10)
    fetch.add_argument("--out", required=True)
    fetch.set_defaults(func=cmd_fetch_prices)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
