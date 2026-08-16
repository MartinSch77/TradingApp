# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/ml/train_crowd_model.py — the crowd-model baseline trainer/exporter.

Same structure as test_train_bot_model.py (the two scripts are deliberately parallel): pure
helpers (crowd_score_class, medians, impute, read_dataset) tested directly, the pre-import
"refuse rather than pretend" guards, and one full fit+export+ONNX-parity run on a small
synthetic dataset.
"""

from __future__ import annotations

import builtins
import csv
import json
from pathlib import Path

import pytest

import train_crowd_model as tcm


# --------------------------------------------------------------------------- read_dataset


def write_dataset(path: Path, header: list[str], rows: list[list[str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)


def test_read_dataset_happy_path(tmp_path):
    header = ["decision_time", "label_end_time", "label", "retail_pct_long_z"]
    rows = [
        ["2024-01-01T00:00:00.000Z", "2024-01-06T00:00:00.000Z", "LONG", "1.5"],
        ["2024-01-02T00:00:00.000Z", "2024-01-07T00:00:00.000Z", "SHORT", ""],
    ]
    p = tmp_path / "dataset.csv"
    write_dataset(p, header, rows)
    manifest = {"features": ["retail_pct_long_z"]}
    rows_out, labels, decisions, ends = tcm.read_dataset(p, manifest)
    assert rows_out == [[1.5], [None]]
    assert labels == [tcm.CLASS_TO_INT["LONG"], tcm.CLASS_TO_INT["SHORT"]]
    assert decisions[0] == "2024-01-01T00:00:00.000Z"
    assert ends[1] == "2024-01-07T00:00:00.000Z"


def test_read_dataset_empty_file(tmp_path):
    p = tmp_path / "dataset.csv"
    p.write_text("", encoding="utf-8")
    with pytest.raises(SystemExit):
        tcm.read_dataset(p, {"features": ["retail_pct_long_z"]})


def test_read_dataset_missing_columns(tmp_path):
    header = ["decision_time", "label_end_time", "label"]
    rows = [["2024-01-01T00:00:00.000Z", "2024-01-06T00:00:00.000Z", "LONG"]]
    p = tmp_path / "dataset.csv"
    write_dataset(p, header, rows)
    with pytest.raises(SystemExit):
        tcm.read_dataset(p, {"features": ["retail_pct_long_z"]})


def test_read_dataset_unknown_label(tmp_path):
    header = ["decision_time", "label_end_time", "label", "retail_pct_long_z"]
    rows = [["2024-01-01T00:00:00.000Z", "2024-01-06T00:00:00.000Z", "SIDEWAYS", "1.0"]]
    p = tmp_path / "dataset.csv"
    write_dataset(p, header, rows)
    with pytest.raises(SystemExit):
        tcm.read_dataset(p, {"features": ["retail_pct_long_z"]})


# --------------------------------------------------------------------------- crowd_score_class


FEATURES = ["retail_pct_long_z", "put_call_z", "cot_asset_mgr_net_z", "social_net_sentiment_z"]


def row_with(**values) -> list[float | None]:
    return [values.get(name) for name in FEATURES]


def test_crowd_score_class_no_measured_family_abstains():
    row = row_with()  # all None
    assert tcm.crowd_score_class(row, FEATURES, 0.2) == tcm.CLASS_TO_INT["NO_TRADE"]


def test_crowd_score_class_long():
    # retail contrarian (-1 sign): a very negative retail z scores bullish.
    row = row_with(retail_pct_long_z=-3.0)
    assert tcm.crowd_score_class(row, FEATURES, 0.2) == tcm.CLASS_TO_INT["LONG"]


def test_crowd_score_class_short():
    row = row_with(retail_pct_long_z=3.0)
    assert tcm.crowd_score_class(row, FEATURES, 0.2) == tcm.CLASS_TO_INT["SHORT"]


def test_crowd_score_class_inside_threshold_is_no_trade():
    row = row_with(retail_pct_long_z=0.01)
    assert tcm.crowd_score_class(row, FEATURES, 0.2) == tcm.CLASS_TO_INT["NO_TRADE"]


def test_crowd_score_class_partial_measurement_renormalizes():
    # Only cot_asset_mgr_net_z measured (weight 0.20, sign +1): a positive value alone should
    # be enough to cross the threshold since the weight is renormalized to 1.0.
    row = row_with(cot_asset_mgr_net_z=5.0)
    assert tcm.crowd_score_class(row, FEATURES, 0.2) == tcm.CLASS_TO_INT["LONG"]


# --------------------------------------------------------------------------- medians / impute


def test_medians_odd_count():
    assert tcm.medians([[1.0], [3.0], [2.0]], 1) == [2.0]


def test_medians_even_count():
    assert tcm.medians([[1.0], [2.0], [3.0], [4.0]], 1) == [2.5]


def test_medians_all_none_defaults_zero():
    assert tcm.medians([[None], [None]], 1) == [0.0]


def test_impute_fills_none():
    assert tcm.impute([[1.0, None], [None, 5.0]], [10.0, 20.0]) == [[1.0, 20.0], [10.0, 5.0]]


# --------------------------------------------------------------------------- skip/fail helpers


def test_skip_raises_exit_3(capsys):
    with pytest.raises(SystemExit) as excinfo:
        tcm.skip("nope")
    assert excinfo.value.code == tcm.EXIT_SKIPPED
    assert "skipped" in capsys.readouterr().err


def test_fail_raises_exit_1(capsys):
    with pytest.raises(SystemExit) as excinfo:
        tcm.fail("bad")
    assert excinfo.value.code == 1
    assert "bad" in capsys.readouterr().err


# --------------------------------------------------------------------------- main(): pre-import guards


def make_manifest(path: Path, features: list[str], **extra) -> None:
    manifest = {"version": 1, "instrument": "SPX500", "horizon_days": 5, "dead_zone_pct": 0.25,
               "features": features}
    manifest.update(extra)
    path.write_text(json.dumps(manifest), encoding="utf-8")


def make_crowd_dataset(path: Path, n: int, features: list[str], label_fn, feature_fn) -> None:
    from datetime import datetime, timedelta, timezone
    base = datetime(2024, 1, 1, 21, 0, 0, tzinfo=timezone.utc)
    header = ["decision_time", "label_end_time", "label"] + features
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        for i in range(n):
            decision = base + timedelta(days=i)
            end = decision + timedelta(days=5)
            row = [decision.isoformat().replace("+00:00", ".000Z"),
                  end.isoformat().replace("+00:00", ".000Z"),
                  label_fn(i)] + [str(v) for v in feature_fn(i)]
            writer.writerow(row)


def test_main_skips_below_min_samples(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_crowd_dataset(dataset_path, 5, FEATURES,
                       label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                       feature_fn=lambda i: [-3.0 if i % 2 else 3.0, 0.0, 0.0, 0.0])
    with pytest.raises(SystemExit) as excinfo:
        tcm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "200"])
    assert excinfo.value.code == tcm.EXIT_SKIPPED
    assert "min-samples" in capsys.readouterr().err


def test_main_skips_one_class(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_crowd_dataset(dataset_path, 10, FEATURES,
                       label_fn=lambda i: "NO_TRADE",
                       feature_fn=lambda i: [0.0, 0.0, 0.0, 0.0])
    with pytest.raises(SystemExit) as excinfo:
        tcm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "5"])
    assert excinfo.value.code == tcm.EXIT_SKIPPED
    assert "one-class" in capsys.readouterr().err


def test_main_skips_when_ml_env_missing(tmp_path, monkeypatch, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_crowd_dataset(dataset_path, 10, FEATURES,
                       label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                       feature_fn=lambda i: [-3.0 if i % 2 else 3.0, 0.0, 0.0, 0.0])

    real_import = builtins.__import__
    blocked = {"onnxruntime", "xgboost", "onnxmltools", "skl2onnx"}

    def blocking_import(name, *args, **kwargs):
        top = name.split(".")[0]
        if top in blocked:
            raise ImportError(f"simulated missing dependency: {name}")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", blocking_import)
    with pytest.raises(SystemExit) as excinfo:
        tcm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "5"])
    assert excinfo.value.code == tcm.EXIT_SKIPPED
    assert "ML environment" in capsys.readouterr().err


def test_main_skips_when_no_fold_evaluable(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_crowd_dataset(dataset_path, 20, FEATURES,
                       label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                       feature_fn=lambda i: [-3.0 if i % 2 else 3.0, 0.0, 0.0, 0.0])
    with pytest.raises(SystemExit) as excinfo:
        tcm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "5",
                 "--min-fold-train", "100000", "--folds", "2"])
    assert excinfo.value.code == tcm.EXIT_SKIPPED
    assert "no walk-forward fold" in capsys.readouterr().err


# --------------------------------------------------------------------------- main(): full fit


def test_main_full_run_writes_exports_and_report(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    out_dir = tmp_path / "out"
    make_manifest(manifest_path, FEATURES)

    def label_fn(i):
        if i < 15:
            return "NO_TRADE"
        return "LONG" if i % 2 == 0 else "SHORT"

    def feature_fn(i):
        if i < 15:
            return [0.0, 0.0, 0.0, 0.0]
        return [-3.0 if i % 2 == 0 else 3.0, 0.0, 0.0, 0.0]

    make_crowd_dataset(dataset_path, 30, FEATURES, label_fn, feature_fn)
    rc = tcm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                  "--out-dir", str(out_dir), "--min-samples", "10", "--folds", "2",
                  "--embargo-days", "0", "--min-fold-train", "3", "--max-iter", "50",
                  "--xgb-estimators", "5"])
    assert rc == 0
    assert (out_dir / "crowd-logreg.onnx").is_file()
    assert (out_dir / "crowd-xgb.onnx").is_file()
    report = json.loads((out_dir / "training-report.json").read_text(encoding="utf-8"))
    assert report["dataset"]["rows"] == 30
    assert report["evaluated_folds"] >= 1
    assert "logistic_regression" in report["means_over_evaluated_folds"]
    assert "baseline_crowd_score_sign" in report["means_over_evaluated_folds"]
    assert any(f.get("skipped") for f in report["folds"])
    out = capsys.readouterr().out
    assert "rows" in out
    assert "wrote" in out


def test_main_parity_failure_writes_nothing(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    out_dir = tmp_path / "out"
    make_manifest(manifest_path, FEATURES)
    make_crowd_dataset(dataset_path, 20, FEATURES,
                       label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                       feature_fn=lambda i: [-3.0 if i % 2 else 3.0, 0.0, 0.0, 0.0])
    with pytest.raises(SystemExit) as excinfo:
        tcm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(out_dir), "--min-samples", "5", "--folds", "2",
                 "--embargo-days", "0", "--min-fold-train", "3", "--max-iter", "50",
                 "--xgb-estimators", "5", "--parity-tolerance", "-1.0"])
    assert excinfo.value.code == 1
    err = capsys.readouterr().err
    assert "disagrees with the trained model" in err
    assert not (out_dir / "crowd-logreg.onnx").exists()
    assert not (out_dir / "crowd-xgb.onnx").exists()
