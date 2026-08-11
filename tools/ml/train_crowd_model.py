#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fit and export the crowd baseline models from a built dataset (REQ-F-041).

The OFFLINE half two of the Phase 4 pipeline: crowd_dataset.py builds the labelled dataset;
this tool fits the two baseline models — a logistic regression and a gradient-boosted-trees
classifier (XGBoost) — evaluates them with PURGED WALK-FORWARD validation beside named
baselines on identical rows, and exports each to ONNX for the (later, optional) C++ inference
phase. Python remains a development-time tool: the app never runs this.

    tools/ml/train_crowd_model.py --dataset dataset.csv --manifest manifest.json --out-dir out/

Honesty rules, in the order they bite:

  * REFUSE RATHER THAN PRETEND. A dataset below --min-samples, or with fewer than two label
    classes, or a machine without the optional ML environment (`./setup.sh ml`) exits with the
    project's "skipped" code 3 and writes NOTHING — a model fitted to nothing would still score
    every row, which is worse than no model. These guards run BEFORE the heavy imports, so the
    refusals are testable on machines that never installed them.
  * THE NUMBERS ARE OUT-OF-SAMPLE. Every reported metric comes from purged walk-forward folds
    (crowd_dataset.walk_forward_splits): evaluation rows are strictly later than everything
    trained on, and training rows whose label window reaches into the evaluation block are
    purged. The final exported model is then fitted on the full dataset — the report says so,
    and its numbers stay the walk-forward ones.
  * BASELINES ON IDENTICAL ROWS. Each fold scores the models beside the majority class of its
    own training block, always-NO_TRADE, and the transparent Phase 2 crowd score's sign
    (REQ-F-040 weights, retail contrarian) — so "beats the baseline" is measured, not felt.
  * MISSING IS IMPUTED VISIBLY. Empty cells become the TRAINING fold's median (never the
    validation fold's — that would leak), the per-series `_measured` flags keep the absence
    representable, and the final medians are embedded in the ONNX metadata so any consumer
    imputes exactly as the trainer did.
  * AN EXPORT MUST REPRODUCE ITS SOURCE. Each ONNX graph is run through onnxruntime and
    compared to the fitted model's own probabilities BEFORE it is written; a disagreement is a
    reported failure and no file — never a shipped model that answers differently.
"""

from __future__ import annotations

import argparse
import json
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import crowd_dataset  # noqa: E402 — the split logic must be THE one the dataset tool defines

EXIT_SKIPPED = 3  # the project-wide "stage skipped" code (build_all's convention)

CLASS_TO_INT = {"SHORT": 0, "NO_TRADE": 1, "LONG": 2}
INT_TO_CLASS = {v: k for k, v in CLASS_TO_INT.items()}

# The Phase 2 crowd-score hypothesis weights and bullish orientations
# (src/domain/CrowdScore.h defaults; src/services/CrowdScoreBuilder.cpp orientation).
# Retail is CONTRARIAN and PUT-CALL is bearish-when-high, hence the -1 signs.
SCORE_FAMILIES = [
    ("retail_pct_long_z", 0.35, -1.0),
    ("put_call_z", 0.30, -1.0),
    ("cot_asset_mgr_net_z", 0.20, 1.0),
    ("social_net_sentiment_z", 0.15, 1.0),
]


def skip(message: str) -> "None":
    print(f"train_crowd_model: skipped: {message}", file=sys.stderr)
    raise SystemExit(EXIT_SKIPPED)


def fail(message: str) -> "None":
    print(f"train_crowd_model: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_dataset(dataset_path: Path, manifest: dict):
    """Rows matched to the manifest BY NAME. A feature the CSV does not carry is a hard error —
    positional reading is exactly the silent mispairing the manifest exists to prevent."""
    features = manifest["features"]
    with dataset_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None:
            fail(f"{dataset_path}: empty dataset")
        missing = [n for n in features + ["label", "decision_time", "label_end_time"]
                   if n not in header]
        if missing:
            fail(f"{dataset_path}: columns missing for manifest features: {', '.join(missing)}")
        index = {name: header.index(name) for name in header}
        rows, labels, decisions, ends = [], [], [], []
        for row in reader:
            rows.append([float(row[index[n]]) if row[index[n]] != "" else None
                         for n in features])
            label = row[index["label"]]
            if label not in CLASS_TO_INT:
                fail(f"{dataset_path}: unknown label {label!r}")
            labels.append(CLASS_TO_INT[label])
            decisions.append(row[index["decision_time"]])
            ends.append(row[index["label_end_time"]])
    return rows, labels, decisions, ends


def crowd_score_class(row: list[float | None], features: list[str], threshold: float) -> int:
    """The transparent Phase 2 score's sign as a class — the baseline a trained model must
    demonstrably beat. Unmeasured families are excluded and the weights renormalized (missing
    is never zero); NO measured family abstains (NO_TRADE)."""
    total_weight = 0.0
    score = 0.0
    for column, weight, sign in SCORE_FAMILIES:
        z = row[features.index(column)]
        if z is None:
            continue
        total_weight += weight
        score += weight * sign * z
    if total_weight <= 0.0:
        return CLASS_TO_INT["NO_TRADE"]
    score /= total_weight
    if score > threshold:
        return CLASS_TO_INT["LONG"]
    if score < -threshold:
        return CLASS_TO_INT["SHORT"]
    return CLASS_TO_INT["NO_TRADE"]


def medians(rows: list[list[float | None]], n_features: int) -> list[float]:
    """Per-column median over the MEASURED values only. A column with no measurement imputes
    0.0 — its paired `_measured` flag (always present) is what carries the absence."""
    out = []
    for col in range(n_features):
        values = sorted(r[col] for r in rows if r[col] is not None)
        if not values:
            out.append(0.0)
            continue
        mid = len(values) // 2
        out.append(values[mid] if len(values) % 2 else (values[mid - 1] + values[mid]) / 2.0)
    return out


def impute(rows: list[list[float | None]], fill: list[float]) -> list[list[float]]:
    return [[v if v is not None else fill[c] for c, v in enumerate(r)] for r in rows]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--folds", type=int, default=4)
    parser.add_argument("--embargo-days", type=float, default=2.0)
    parser.add_argument("--min-train-fraction", type=float, default=0.5)
    parser.add_argument("--min-samples", type=int, default=200,
                        help="below this there is nothing to fit (exit 3)")
    parser.add_argument("--min-fold-train", type=int, default=20,
                        help="a fold training block below this is reported, not fitted")
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--max-iter", type=int, default=500)
    parser.add_argument("--xgb-estimators", type=int, default=200)
    parser.add_argument("--baseline-threshold", type=float, default=0.2,
                        help="|crowd score| needed before the baseline calls a direction")
    parser.add_argument("--parity-tolerance", type=float, default=5e-3,
                        help="max |probability difference| the ONNX export may show")
    args = parser.parse_args(argv)

    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    features = manifest["features"]
    rows, labels, decisions, ends = read_dataset(Path(args.dataset), manifest)

    # The honest refusals — BEFORE the heavy imports, so they hold everywhere.
    if len(rows) < args.min_samples:
        skip(f"{len(rows)} rows < --min-samples {args.min_samples}; nothing to fit")
    classes_present = sorted(set(labels))
    if len(classes_present) < 2:
        only = INT_TO_CLASS[classes_present[0]]
        skip(f"every row is labelled {only}; a one-class model separates nothing")

    # Expected in walk-forward evaluation: a model may predict a class the (later, smaller)
    # validation block never realised. sklearn warns per fold; the fold report carries the
    # numbers, so the warning is noise here.
    import warnings
    warnings.filterwarnings("ignore", message="y_pred contains classes not in y_true")

    try:
        import numpy as np
        import onnxruntime
        import sklearn
        import xgboost
        from onnxmltools.convert.xgboost.operator_converters.XGBoost import (
            convert_xgboost as xgboost_onnx_op)
        from skl2onnx import convert_sklearn, update_registered_converter
        from skl2onnx.common.data_types import FloatTensorType
        from skl2onnx.common.shape_calculator import calculate_linear_classifier_output_shapes
        from sklearn.linear_model import LogisticRegression
        from sklearn.metrics import accuracy_score, balanced_accuracy_score, f1_score
        from xgboost import XGBClassifier
    except ImportError as error:
        skip(f"the ML environment is not provisioned ({error}); run ./setup.sh ml")

    splits = crowd_dataset.walk_forward_splits(decisions, ends, args.folds, args.embargo_days,
                                               args.min_train_fraction)

    def metric_set(y_true, y_pred) -> dict:
        return {
            "accuracy": float(accuracy_score(y_true, y_pred)),
            "balanced_accuracy": float(balanced_accuracy_score(y_true, y_pred)),
            "macro_f1": float(f1_score(y_true, y_pred, average="macro", zero_division=0)),
        }

    def fit_logreg(x, y):
        model = LogisticRegression(max_iter=args.max_iter, class_weight="balanced",
                                   random_state=args.seed)
        model.fit(x, y)
        return model, [int(c) for c in model.classes_]

    def fit_xgb(x, y):
        # XGBClassifier insists on consecutive 0..k-1 labels; a fold may lack a class, so the
        # labels are re-encoded for the fit and the model's class table carries the mapping.
        present = sorted(set(y))
        remap = {c: i for i, c in enumerate(present)}
        model = XGBClassifier(n_estimators=args.xgb_estimators, max_depth=3, learning_rate=0.1,
                              subsample=0.9, tree_method="hist", random_state=args.seed,
                              eval_metric="mlogloss", n_jobs=2)
        model.fit(x, [remap[c] for c in y])
        return model, present

    fold_reports = []
    sums: dict[str, dict[str, dict[str, float]]] = {}
    evaluated = 0
    for fold in splits["folds"]:
        train_idx, val_idx = fold["train_rows"], fold["val_rows"]
        report = {"index": fold["index"], "train_rows": len(train_idx),
                  "val_rows": len(val_idx), "purged": fold["purged"],
                  "val_start": fold["val_start"], "val_end": fold["val_end"]}
        y_train = [labels[i] for i in train_idx]
        if len(train_idx) < args.min_fold_train or len(set(y_train)) < 2:
            report["skipped"] = (f"training block unusable ({len(train_idx)} rows, "
                                 f"{len(set(y_train))} classes)")
            fold_reports.append(report)
            continue
        fill = medians([rows[i] for i in train_idx], len(features))
        x_train = np.asarray(impute([rows[i] for i in train_idx], fill), dtype=np.float32)
        x_val = np.asarray(impute([rows[i] for i in val_idx], fill), dtype=np.float32)
        y_val = [labels[i] for i in val_idx]

        results = {}
        logreg, _ = fit_logreg(x_train, y_train)
        results["logistic_regression"] = metric_set(y_val, logreg.predict(x_val))
        xgb, xgb_classes = fit_xgb(x_train, y_train)
        xgb_pred = [xgb_classes[int(p)] for p in xgb.predict(x_val)]
        results["xgboost"] = metric_set(y_val, xgb_pred)

        majority = max(set(y_train), key=y_train.count)
        results["baseline_majority_class"] = metric_set(y_val, [majority] * len(y_val))
        results["baseline_always_no_trade"] = metric_set(
            y_val, [CLASS_TO_INT["NO_TRADE"]] * len(y_val))
        results["baseline_crowd_score_sign"] = metric_set(
            y_val, [crowd_score_class(rows[i], features, args.baseline_threshold)
                    for i in val_idx])

        report["results"] = results
        fold_reports.append(report)
        evaluated += 1
        for name, metrics in results.items():
            for metric, value in metrics.items():
                sums.setdefault(name, {}).setdefault(metric, 0.0)
                sums[name][metric] += value

    if evaluated == 0:
        skip("no walk-forward fold was evaluable; the record is too short to measure anything")
    means = {name: {metric: value / evaluated for metric, value in metrics.items()}
             for name, metrics in sums.items()}

    # The exported models: fitted on the FULL dataset (the report keeps the honest
    # walk-forward numbers), verified against onnxruntime BEFORE anything reaches disk.
    update_registered_converter(XGBClassifier, "XGBoostXGBClassifier",
                                calculate_linear_classifier_output_shapes, xgboost_onnx_op,
                                options={"nocl": [True, False], "zipmap": [True, False]})
    fill_all = medians(rows, len(features))
    x_all = np.asarray(impute(rows, fill_all), dtype=np.float32)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    final_logreg, logreg_classes = fit_logreg(x_all, labels)
    final_xgb, final_xgb_classes = fit_xgb(x_all, labels)
    exports = {}
    verified: list[tuple[str, bytes]] = []
    for name, model, class_order in (("crowd-logreg.onnx", final_logreg, logreg_classes),
                                     ("crowd-xgb.onnx", final_xgb, final_xgb_classes)):
        # ai.onnx.ml is pinned to 3: the xgboost converter otherwise emits a domain version
        # newer than skl2onnx itself accepts (measured with xgboost 3.4 / skl2onnx 1.19).
        onx = convert_sklearn(model, initial_types=[("input", FloatTensorType([None,
                                                                               len(features)]))],
                              options={id(model): {"zipmap": False}},
                              target_opset={"ai.onnx.ml": 3})
        metadata = {
            "feature_names": json.dumps(features),
            "imputation_medians": json.dumps(fill_all),
            "classes": json.dumps([INT_TO_CLASS[c] for c in class_order]),
            "manifest_version": str(manifest["version"]),
            "instrument": str(manifest["instrument"]),
            "horizon_days": str(manifest["horizon_days"]),
            "dead_zone_pct": str(manifest["dead_zone_pct"]),
            "training_rows": str(len(rows)),
        }
        for key, value in metadata.items():
            entry = onx.metadata_props.add()
            entry.key, entry.value = key, value
        payload = onx.SerializeToString()
        session = onnxruntime.InferenceSession(payload, providers=["CPUExecutionProvider"])
        onnx_probs = session.run(None, {"input": x_all})[1]
        own_probs = model.predict_proba(x_all)
        worst = float(np.max(np.abs(np.asarray(onnx_probs) - np.asarray(own_probs))))
        if worst > args.parity_tolerance:
            fail(f"{name}: the ONNX export disagrees with the trained model "
                 f"(max |dp| {worst:.6f} > {args.parity_tolerance}); nothing written")
        verified.append((name, payload))
        exports[name] = {"classes": [INT_TO_CLASS[c] for c in class_order],
                         "parity_max_abs_diff": worst}
    # Both exports verified BEFORE either reaches disk — a partial pair on failure would look
    # like a completed run to whoever picks the directory up later.
    for name, payload in verified:
        (out_dir / name).write_bytes(payload)

    report = {
        "config": {k: getattr(args, k) for k in ("folds", "embargo_days", "min_train_fraction",
                                                 "min_samples", "seed", "max_iter",
                                                 "xgb_estimators", "baseline_threshold")},
        "dataset": {"path": str(args.dataset), "rows": len(rows),
                    "label_counts": {INT_TO_CLASS[c]: labels.count(c)
                                     for c in sorted(set(labels))},
                    "manifest_version": manifest["version"],
                    "instrument": manifest["instrument"]},
        "validation": "purged walk-forward; final models fitted on the full dataset AFTER "
                      "these numbers were measured",
        "folds": fold_reports,
        "means_over_evaluated_folds": means,
        "evaluated_folds": evaluated,
        "exports": exports,
        "versions": {"python": sys.version.split()[0], "numpy": np.__version__,
                     "scikit_learn": sklearn.__version__, "xgboost": xgboost.__version__,
                     "onnxruntime": onnxruntime.__version__},
    }
    (out_dir / "training-report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(f"{len(rows)} rows, {evaluated} evaluated fold(s); "
          f"means over evaluated folds (balanced accuracy):")
    for name in sorted(means):
        print(f"  {name:28s} {means[name]['balanced_accuracy']:.3f}")
    print(f"wrote {', '.join(sorted(exports))} and training-report.json to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
