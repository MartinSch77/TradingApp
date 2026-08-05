#!/usr/bin/env python3
"""Train the bot's outcome network from its own experience log (REQ-F-033).

The paper-trading bot appends one JSON line per CLOSED trade: the features that
were true when it opened, and what the trade actually made after costs. This
fits a small feed-forward network to that record and writes a model file the app
reads back (src/domain/BotNet.*).

    tools/train_bot_net.py                       # default paths, under the app config dir
    tools/train_bot_net.py --log … --out …       # explicit
    tools/train_bot_net.py --hidden 8 --epochs 400

Deliberately stdlib-only: no numpy, no torch, nothing to install on any of the
three platforms this project supports. A few thousand examples over eleven
features is small enough that plain Python trains in seconds, and a dependency
that only the training step needs would still have to be installed everywhere.

Three choices are about honesty rather than accuracy, and they matter more than
the model does:

  * The validation split is the LAST 20% of the record IN TIME, never a random
    sample. Trading data is a time series: a random split lets the model see the
    future of the very market conditions it is scored on, and the AUC it reports
    would be a fiction.
  * The reported AUC is computed on that held-out tail only. It is the number the
    app uses to decide whether the model may refuse a trade at all, so measuring
    it on the training set would hand the bot a licence it never earned.
  * A record that is tiny, or all-wins, or all-losses trains nothing: the script
    says so and writes no model, rather than emitting weights that would score
    every setup identically.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from datetime import datetime, timezone
from pathlib import Path

# Kept in step with trading::entryFeatureNames() — the log lines carry their own
# names, so a mismatch is detected here rather than silently mis-read.
FEATURES = [
    "confidence",
    "volPct",
    "stopPct",
    "targetPct",
    "spreadPct",
    "edgeOverCost",
    "leverage",
    "dir",
    "hourUtc",
    "dayOfWeek",
    "aiBacked",
]

VAL_FRACTION = 0.2
MIN_SAMPLES = 40  # below this there is nothing to fit; the app has its own, higher bar


def default_log() -> Path:
    """The experience log the app writes, on this platform."""
    if sys.platform.startswith("win"):
        base = Path.home() / "AppData" / "Local" / "TradingApp" / "eToro Trader"
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Preferences" / "TradingApp" / "eToro Trader"
    else:
        base = Path.home() / ".config" / "TradingApp" / "eToro Trader"
    return base / "botsim-experience.jsonl"


def allowed_roots(extra: list[Path] | None = None) -> list[Path]:
    """Where this tool may read and write.

    The experience log lives beside the app's own configuration and a developer may
    keep a copy in the working tree; nothing else is any business of a training
    script. Validating the resolved path against an explicit allowlist — rather than
    opening whatever argv contains — is what keeps a mistyped or hostile `--log` from
    reaching the rest of the file system. `--allow-root` widens the list on purpose
    and in the open, which is how a scratch directory gets used without turning the
    check into a formality.
    """
    roots = [default_log().parent.resolve(), Path.cwd().resolve()]
    roots += [p.expanduser().resolve() for p in (extra or [])]
    return roots


def checked_path(path: Path, *, must_exist: bool, extra_roots: list[Path] | None = None) -> Path:
    """`path` resolved and confined to allowed_roots(), or SystemExit."""
    resolved = path.expanduser().resolve()
    roots = allowed_roots(extra_roots)
    if not any(resolved == root or root in resolved.parents for root in roots):
        raise SystemExit(
            f"refusing to touch {resolved}: outside {', '.join(str(r) for r in roots)}")
    if must_exist and not resolved.is_file():
        raise SystemExit(f"{resolved} is not a readable file")
    if not must_exist and not resolved.parent.is_dir():
        raise SystemExit(f"{resolved.parent} does not exist")
    return resolved


def load(path: Path) -> tuple[list[list[float]], list[float], list[str]]:
    """Rows, labels and problems found. One malformed line never kills the run."""
    rows: list[list[float]] = []
    labels: list[float] = []
    problems: list[str] = []
    with path.open("r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as exc:
                problems.append(f"line {lineno}: {exc}")
                continue
            feats = rec.get("features") or {}
            missing = [name for name in FEATURES if name not in feats]
            if missing:
                problems.append(f"line {lineno}: missing {', '.join(missing)}")
                continue
            if "netPnl" not in rec:
                problems.append(f"line {lineno}: no outcome")
                continue
            rows.append([float(feats[name]) for name in FEATURES])
            # The label is what the account actually kept: a trade that "worked"
            # but did not cover its costs is a loss, and must be trained as one.
            labels.append(1.0 if float(rec["netPnl"]) > 0.0 else 0.0)
    return rows, labels, problems


def standardise(rows: list[list[float]]) -> tuple[list[float], list[float]]:
    n = len(rows)
    cols = len(FEATURES)
    mean = [0.0] * cols
    for row in rows:
        for i in range(cols):
            mean[i] += row[i]
    mean = [m / n for m in mean]
    var = [0.0] * cols
    for row in rows:
        for i in range(cols):
            var[i] += (row[i] - mean[i]) ** 2
    # A constant column has no information; a standard deviation of 1 keeps it at
    # zero after centring instead of dividing by nothing.
    sd = [math.sqrt(v / n) if v / n > 1e-12 else 1.0 for v in var]
    return mean, sd


def apply_norm(rows, mean, sd):
    return [[(v - mean[i]) / sd[i] for i, v in enumerate(row)] for row in rows]


def auc(scores: list[float], labels: list[float]) -> float:
    """Area under the ROC curve, by rank. 0.5 = a coin flip."""
    pos = [s for s, y in zip(scores, labels) if y > 0.5]
    neg = [s for s, y in zip(scores, labels) if y <= 0.5]
    if not pos or not neg:
        return 0.5
    order = sorted(range(len(scores)), key=lambda i: scores[i])
    ranks = [0.0] * len(scores)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and scores[order[j + 1]] == scores[order[i]]:
            j += 1
        shared = (i + j) / 2.0 + 1.0  # average rank for ties
        for k in range(i, j + 1):
            ranks[order[k]] = shared
        i = j + 1
    rank_sum = sum(r for r, y in zip(ranks, labels) if y > 0.5)
    return (rank_sum - len(pos) * (len(pos) + 1) / 2.0) / (len(pos) * len(neg))


class Net:
    """One hidden tanh layer, one sigmoid output, trained by plain SGD."""

    def __init__(self, inputs: int, hidden: int, rng: random.Random):
        limit = math.sqrt(6.0 / (inputs + hidden))
        self.w1 = [[rng.uniform(-limit, limit) for _ in range(inputs)] for _ in range(hidden)]
        self.b1 = [0.0] * hidden
        self.w2 = [rng.uniform(-limit, limit) for _ in range(hidden)]
        self.b2 = 0.0

    def forward(self, x: list[float]) -> tuple[list[float], float]:
        h = [math.tanh(sum(w * v for w, v in zip(row, x)) + b) for row, b in zip(self.w1, self.b1)]
        z = sum(w * hv for w, hv in zip(self.w2, h)) + self.b2
        z = max(-40.0, min(40.0, z))
        return h, 1.0 / (1.0 + math.exp(-z))

    def predict(self, x: list[float]) -> float:
        return self.forward(x)[1]

    def step(self, x: list[float], y: float, lr: float, l2: float) -> float:
        h, p = self.forward(x)
        # Cross-entropy: dL/dz is simply (p - y) for a sigmoid output.
        d_out = p - y
        for j, hv in enumerate(self.w2):
            grad_h = d_out * hv * (1.0 - h[j] * h[j])
            for i, xv in enumerate(x):
                self.w1[j][i] -= lr * (grad_h * xv + l2 * self.w1[j][i])
            self.b1[j] -= lr * grad_h
        for j in range(len(self.w2)):
            self.w2[j] -= lr * (d_out * h[j] + l2 * self.w2[j])
        self.b2 -= lr * d_out
        eps = 1e-12
        return -(y * math.log(p + eps) + (1.0 - y) * math.log(1.0 - p + eps))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", type=Path, default=default_log(),
                    help="experience JSONL written by the bot (default: the app's own)")
    ap.add_argument("--out", type=Path, default=None,
                    help="model file to write (default: botnet.json beside the log)")
    ap.add_argument("--hidden", type=int, default=6, help="hidden units (default 6)")
    ap.add_argument("--epochs", type=int, default=300)
    ap.add_argument("--lr", type=float, default=0.05)
    ap.add_argument("--l2", type=float, default=1e-4, help="weight decay")
    ap.add_argument("--seed", type=int, default=20260805, help="fixed: runs are reproducible")
    ap.add_argument("--min-samples", type=int, default=MIN_SAMPLES)
    ap.add_argument("--allow-root", type=Path, action="append", default=[],
                    metavar="DIR",
                    help="additionally permit reading/writing under DIR (repeatable); by "
                         "default only the app's config directory and the working tree are")
    args = ap.parse_args()

    # Both paths come from the command line, so both are resolved and confined before
    # anything is opened — see checked_path().
    if not args.log.expanduser().exists():
        print(f"no experience log at {args.log}")
        print("Run the bot simulation first — it appends one line per closed trade.")
        return 3  # 'skipped', the same convention the build stages use
    log_path = checked_path(args.log, must_exist=True, extra_roots=args.allow_root)
    out_path = checked_path(args.out or (log_path.parent / "botnet.json"), must_exist=False,
                            extra_roots=args.allow_root)

    rows, labels, problems = load(log_path)
    for problem in problems[:5]:
        print(f"skipped {problem}")
    if len(problems) > 5:
        print(f"…and {len(problems) - 5} more unusable lines")
    print(f"{len(rows)} usable examples from {log_path}")
    if len(rows) < args.min_samples:
        print(f"too few to train on (need {args.min_samples}); letting the bot keep collecting")
        return 3
    wins = sum(1 for y in labels if y > 0.5)
    if wins == 0 or wins == len(labels):
        print(f"every trade in the record has the same outcome ({wins} wins of {len(labels)}) — "
              "there is nothing to separate yet")
        return 3

    # Time-ordered split: the model is scored on trades that happened AFTER the
    # ones it learned from, which is the only split that answers the question the
    # app asks of it.
    cut = max(1, int(len(rows) * (1.0 - VAL_FRACTION)))
    train_x_raw, val_x_raw = rows[:cut], rows[cut:]
    train_y, val_y = labels[:cut], labels[cut:]
    if not val_x_raw:
        print("not enough history for a held-out tail")
        return 3

    mean, sd = standardise(train_x_raw)
    train_x = apply_norm(train_x_raw, mean, sd)
    val_x = apply_norm(val_x_raw, mean, sd)

    rng = random.Random(args.seed)
    net = Net(len(FEATURES), args.hidden, rng)
    order = list(range(len(train_x)))
    for epoch in range(args.epochs):
        rng.shuffle(order)
        loss = 0.0
        for idx in order:
            loss += net.step(train_x[idx], train_y[idx], args.lr, args.l2)
        if (epoch + 1) % max(1, args.epochs // 5) == 0:
            print(f"  epoch {epoch + 1:4d}  train loss {loss / len(order):.4f}")

    val_scores = [net.predict(x) for x in val_x]
    val_auc = auc(val_scores, val_y)
    hits = sum(1 for s, y in zip(val_scores, val_y) if (s >= 0.5) == (y > 0.5))
    val_acc = hits / len(val_y)
    base = sum(val_y) / len(val_y)
    print(f"held-out tail: {len(val_y)} trades, AUC {val_auc:.3f}, accuracy {val_acc * 100:.1f}% "
          f"(always-yes would score {max(base, 1 - base) * 100:.1f}%)")
    if val_auc < 0.5:
        print("the model is worse than a coin flip on unseen trades — writing it anyway, "
              "but the app will not let it refuse anything")

    model = {
        "features": FEATURES,
        "mean": mean,
        "stddev": sd,
        "w1": net.w1,
        "b1": net.b1,
        "w2": net.w2,
        "b2": net.b2,
        "samples": len(train_x),
        "valAuc": val_auc,
        "valAccuracy": val_acc,
        "trainedAt": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
        "hidden": args.hidden,
        "epochs": args.epochs,
    }
    out_path.write_text(json.dumps(model, indent=1), encoding="utf-8")
    print(f"wrote {out_path}")
    print("The app picks it up on the next start; set TRADINGAPP_BOT_NET=advise or gate.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
