#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Build the bot-decision training dataset from PredictionLedger's own JSONL (item 9 of the
2026-08-12 strategy redesign; REQ-F-037/-033).

A companion to crowd_dataset.py, not a rewrite: this REUSES walk_forward_splits from that
module verbatim (imported, never reimplemented) — the purged, time-ordered, embargoed split
is the same function for both models, exactly as the redesign asks ("diese Pipeline sollte
für das eigentliche Botmodell wiederverwendet werden").

    tools/ml/bot_dataset.py build --ledger prediction-ledger.jsonl --instrument SPX500 \\
        --out dataset.csv --manifest manifest.json
    tools/ml/bot_dataset.py splits --dataset dataset.csv --folds 4 --embargo-days 1

Rules that matter more than any model fitted downstream:

  * EVERY recorded decision labels, not just the ones taken. PredictionLedger already keeps
    the stay-outs for exactly this reason: a record of executed trades measures the gate in
    front of the signal, never the signal itself. Training only from botsim-experience.jsonl
    inherits that SAME selection bias one level up — it would only ever see what the entry
    gate had already let through.
  * THE LABEL IS PATH-DEPENDENT, never an endpoint question — domain/PathOutcome.h's own
    algorithm (walkPath/resolvePathLabel), REIMPLEMENTED here in Python because the offline
    half of this pipeline is deliberately Python-only (crowd_dataset.py's own convention: the
    app never imports, shells out to, or depends on it). A later row's PRICE only — the
    ledger has no OHLC — becomes a degenerate one-point bar (open=high=low=close=that price),
    the SAME simplification domain/PathOutcome.cpp's own resolvePathLabelForPrediction bridge
    makes and states explicitly: a real intrabar excursion the ledger never sampled is
    invisible to this label, in both languages, by construction — not a Python-only gap.
  * NO LOOKAHEAD. A decision's label is resolved only from STRICTLY LATER rows for the SAME
    symbol; rows for another instrument, or at/before the decision time, never enter it.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import crowd_dataset  # noqa: E402 — walk_forward_splits/fmt/to_utc_iso are reused, not redefined

MANIFEST_VERSION = 1
LABEL_CLASSES = ["SHORT", "NO_TRADE", "LONG"]

# Regime::regimeWord's own five words (src/domain/PredictionLedger.cpp) — one-hot encoded so
# the trainer sees a plain numeric feature, never a string it would have to guess an order for.
REGIME_WORDS = ["unknown", "trend", "range", "highVolatility", "eventWindow"]

FEATURE_NAMES = (["strength", "measured", "unknowns", "prior_move_dir", "vwap_side", "dir"]
                 + [f"regime_{w.lower()}" for w in REGIME_WORDS])

META_COLUMNS = ["instrument", "decision_time", "label_end_time", "label"]


def fail(message: str) -> "None":
    print(f"bot_dataset: {message}", file=sys.stderr)
    raise SystemExit(1)


# --------------------------------------------------------------------------- ledger


def load_ledger(path: Path) -> list[dict]:
    """Every readable JSONL row. A truncated final line (a kill during a write) costs that
    line and nothing else — the SAME tolerance PredictionLedger::loadPredictions has for its
    own file, so a dataset built here never disagrees with the app about what the ledger
    contains."""
    rows: list[dict] = []
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not all(key in obj for key in ("at", "symbol", "price")):
                continue
            rows.append(obj)
    return rows


# --------------------------------------------------------------------------- path label


def walk_path(prices_after: list[float], entry_price: float, is_long: bool, cfg: dict) -> bool:
    """Mirrors domain/PathOutcome.cpp's walkPath for a degenerate one-point-per-row path.
    Same-candle ambiguity does not arise here (one point cannot be both extremes at once the
    way a real bar can) — the conservative stop-first rule is preserved by checking the stop
    BEFORE the target on every point, exactly as the C++ walkPath does."""
    stop_fraction = cfg["stop_fraction"]
    if entry_price <= 0.0 or stop_fraction <= 0.0:
        return False
    stop_distance = entry_price * stop_fraction * cfg["stop_r"]
    target_distance = (entry_price * stop_fraction * cfg["target_r"]
                       + entry_price * cfg["cost_buffer_fraction"])
    stop_price = entry_price - stop_distance if is_long else entry_price + stop_distance
    target_price = entry_price + target_distance if is_long else entry_price - target_distance
    for price in prices_after[:cfg["max_bars"]]:
        stop_hit = (price <= stop_price) if is_long else (price >= stop_price)
        if stop_hit:
            return False
        target_hit = (price >= target_price) if is_long else (price <= target_price)
        if target_hit:
            return True
    return False


def resolve_label(prices_after: list[float], entry_price: float, cfg: dict) -> str:
    """Mirrors domain/PathOutcome.cpp's resolvePathLabel: LONG if a hypothetical long's own
    target resolved first, SHORT the mirror, else NO_TRADE."""
    if walk_path(prices_after, entry_price, True, cfg):
        return "LONG"
    if walk_path(prices_after, entry_price, False, cfg):
        return "SHORT"
    return "NO_TRADE"


# --------------------------------------------------------------------------- build


def cmd_build(args: argparse.Namespace) -> int:
    ledger_path = Path(args.ledger)
    if not ledger_path.is_file():
        fail(f"ledger not found: {ledger_path}")
    rows = load_ledger(ledger_path)

    same_symbol = sorted((r for r in rows if r["symbol"] == args.instrument),
                         key=lambda r: r["at"])
    if not same_symbol:
        fail(f"no rows for instrument {args.instrument!r} in {ledger_path}")

    cfg = {"stop_fraction": args.stop_fraction, "target_r": args.target_r,
           "stop_r": args.stop_r, "cost_buffer_fraction": args.cost_buffer_fraction,
           "max_bars": args.max_bars}

    rows_out: list[list[str]] = []
    for i, decision in enumerate(same_symbol):
        # STRICTLY later rows for the same symbol — no lookahead by construction, since
        # same_symbol is already filtered/sorted and only indices after `i` are consulted.
        later = same_symbol[i + 1:]
        if not later:
            continue   # nothing yet to resolve against — dropped, never guessed at
        window = later[:args.max_bars]
        prices_after = [float(r["price"]) for r in window]
        entry_price = float(decision["price"])
        label = resolve_label(prices_after, entry_price, cfg)

        regime_word = decision.get("regime", "unknown")
        values = {
            "strength": float(decision.get("strength", 0.0)),
            "measured": float(decision.get("measured", 0)),
            "unknowns": float(decision.get("unknowns", 0)),
            "prior_move_dir": float(decision.get("priorMoveDir", 0)),
            "vwap_side": float(decision.get("vwapSide", 0)),
            "dir": float(decision.get("dir", 0)),
        }
        for word in REGIME_WORDS:
            values[f"regime_{word.lower()}"] = 1.0 if regime_word == word else 0.0

        row = [args.instrument, decision["at"], window[-1]["at"], label]
        row += [crowd_dataset.fmt(values[name]) for name in FEATURE_NAMES]
        rows_out.append(row)

    if not rows_out:
        fail("no labelable rows — every decision was too close to the end of the ledger")

    out = Path(args.out)
    with out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(META_COLUMNS + FEATURE_NAMES)
        writer.writerows(rows_out)

    manifest = {
        "version": MANIFEST_VERSION,
        "instrument": args.instrument,
        "features": FEATURE_NAMES,
        "label_classes": LABEL_CLASSES,
        "meta_columns": META_COLUMNS,
        "path_outcome_config": cfg,
    }
    Path(args.manifest).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8")

    labels = [r[META_COLUMNS.index("label")] for r in rows_out]
    counts = {c: labels.count(c) for c in LABEL_CLASSES}
    print(f"wrote {len(rows_out)} rows to {out} "
          f"(LONG {counts['LONG']}, NO_TRADE {counts['NO_TRADE']}, SHORT {counts['SHORT']}); "
          f"manifest {args.manifest}")
    return 0


# --------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="ledger -> path-labelled dataset + manifest")
    build.add_argument("--ledger", required=True, help="prediction-ledger.jsonl")
    build.add_argument("--instrument", required=True, help="e.g. SPX500")
    build.add_argument("--out", required=True, help="dataset CSV to write")
    build.add_argument("--manifest", required=True, help="feature manifest JSON to write")
    build.add_argument("--stop-fraction", type=float, default=0.01,
                       help="R as a fraction of entry price (default 0.01 = 1%%)")
    build.add_argument("--target-r", type=float, default=1.0)
    build.add_argument("--stop-r", type=float, default=1.0)
    build.add_argument("--cost-buffer-fraction", type=float, default=0.0015,
                       help="round-trip cost added to the target distance (default 0.15%%)")
    build.add_argument("--max-bars", type=int, default=20,
                       help="how many later ledger rows to walk before giving up (NO_TRADE)")
    build.set_defaults(func=cmd_build)

    splits = sub.add_parser("splits", help="purged walk-forward folds (delegates to "
                                            "crowd_dataset.walk_forward_splits)")
    splits.add_argument("--dataset", required=True)
    splits.add_argument("--folds", type=int, default=4)
    splits.add_argument("--embargo-days", type=float, default=1.0)
    splits.add_argument("--min-train-fraction", type=float, default=0.5)
    splits.add_argument("--out", help="write JSON here instead of stdout")
    splits.set_defaults(func=crowd_dataset.cmd_splits)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
