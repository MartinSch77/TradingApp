# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/ml/train_bot_model.py — the bot-decision baseline trainer/exporter.

Covers the pure helpers (composite_sign_class, medians, impute, read_dataset) directly, the
"refuse rather than pretend" guards (too few samples, one class, ML env missing) which all
run BEFORE the heavy imports, and one full end-to-end fit+export+ONNX-parity-check run on a
small synthetic dataset kept fast via low --xgb-estimators/--max-iter/--min-samples.
"""

from __future__ import annotations

import builtins
import csv
import json
from pathlib import Path

import pytest

import train_bot_model as tbm


# --------------------------------------------------------------------------- read_dataset


def write_dataset(path: Path, header: list[str], rows: list[list[str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)


def test_read_dataset_happy_path(tmp_path):
    header = ["decision_time", "label_end_time", "label", "dir", "strength"]
    rows = [
        ["2024-01-01T00:00:00.000Z", "2024-01-02T00:00:00.000Z", "LONG", "1", "60"],
        ["2024-01-02T00:00:00.000Z", "2024-01-03T00:00:00.000Z", "SHORT", "-1", ""],
    ]
    p = tmp_path / "dataset.csv"
    write_dataset(p, header, rows)
    manifest = {"features": ["dir", "strength"]}
    rows_out, labels, decisions, ends = tbm.read_dataset(p, manifest)
    assert rows_out == [[1.0, 60.0], [-1.0, None]]
    assert labels == [tbm.CLASS_TO_INT["LONG"], tbm.CLASS_TO_INT["SHORT"]]
    assert decisions == ["2024-01-01T00:00:00.000Z", "2024-01-02T00:00:00.000Z"]
    assert ends == ["2024-01-02T00:00:00.000Z", "2024-01-03T00:00:00.000Z"]


def test_read_dataset_empty_file(tmp_path):
    p = tmp_path / "dataset.csv"
    p.write_text("", encoding="utf-8")
    with pytest.raises(SystemExit):
        tbm.read_dataset(p, {"features": ["dir"]})


def test_read_dataset_missing_columns(tmp_path):
    header = ["decision_time", "label_end_time", "label", "dir"]
    rows = [["2024-01-01T00:00:00.000Z", "2024-01-02T00:00:00.000Z", "LONG", "1"]]
    p = tmp_path / "dataset.csv"
    write_dataset(p, header, rows)
    with pytest.raises(SystemExit):
        tbm.read_dataset(p, {"features": ["dir", "strength"]})  # "strength" absent


def test_read_dataset_unknown_label(tmp_path):
    header = ["decision_time", "label_end_time", "label", "dir"]
    rows = [["2024-01-01T00:00:00.000Z", "2024-01-02T00:00:00.000Z", "SIDEWAYS", "1"]]
    p = tmp_path / "dataset.csv"
    write_dataset(p, header, rows)
    with pytest.raises(SystemExit):
        tbm.read_dataset(p, {"features": ["dir"]})


# --------------------------------------------------------------------------- composite_sign_class


FEATURES = ["dir", "strength"]


def test_composite_sign_class_dir_none():
    assert tbm.composite_sign_class([None, 60.0], FEATURES, 40.0) == tbm.CLASS_TO_INT["NO_TRADE"]


def test_composite_sign_class_strength_none():
    assert tbm.composite_sign_class([1.0, None], FEATURES, 40.0) == tbm.CLASS_TO_INT["NO_TRADE"]


def test_composite_sign_class_below_threshold():
    assert tbm.composite_sign_class([1.0, 10.0], FEATURES, 40.0) == tbm.CLASS_TO_INT["NO_TRADE"]


def test_composite_sign_class_long():
    assert tbm.composite_sign_class([1.0, 60.0], FEATURES, 40.0) == tbm.CLASS_TO_INT["LONG"]


def test_composite_sign_class_short():
    assert tbm.composite_sign_class([-1.0, 60.0], FEATURES, 40.0) == tbm.CLASS_TO_INT["SHORT"]


def test_composite_sign_class_zero_dir():
    assert tbm.composite_sign_class([0.0, 60.0], FEATURES, 40.0) == tbm.CLASS_TO_INT["NO_TRADE"]


# --------------------------------------------------------------------------- medians / impute


def test_medians_odd_count():
    rows = [[1.0], [3.0], [2.0]]
    assert tbm.medians(rows, 1) == [2.0]


def test_medians_even_count():
    rows = [[1.0], [2.0], [3.0], [4.0]]
    assert tbm.medians(rows, 1) == [2.5]


def test_medians_all_none_column_defaults_zero():
    rows = [[None], [None]]
    assert tbm.medians(rows, 1) == [0.0]


def test_medians_ignores_none_values():
    rows = [[1.0], [None], [3.0]]
    assert tbm.medians(rows, 1) == [2.0]


def test_impute_fills_none_with_column_fill():
    rows = [[1.0, None], [None, 5.0]]
    fill = [10.0, 20.0]
    assert tbm.impute(rows, fill) == [[1.0, 20.0], [10.0, 5.0]]


# --------------------------------------------------------------------------- skip/fail helpers


def test_skip_raises_exit_3(capsys):
    with pytest.raises(SystemExit) as excinfo:
        tbm.skip("nope")
    assert excinfo.value.code == tbm.EXIT_SKIPPED
    assert "skipped" in capsys.readouterr().err


def test_fail_raises_exit_1(capsys):
    with pytest.raises(SystemExit) as excinfo:
        tbm.fail("bad")
    assert excinfo.value.code == 1
    assert "bad" in capsys.readouterr().err


# --------------------------------------------------------------------------- main(): pre-import guards


def make_manifest(path: Path, features: list[str], **extra) -> None:
    manifest = {"version": 1, "instrument": "SPX500", "features": features}
    manifest.update(extra)
    path.write_text(json.dumps(manifest), encoding="utf-8")


def make_bot_dataset(path: Path, n: int, features: list[str],
                    label_fn, feature_fn, embargo_free_end=True) -> None:
    """n rows, one per day, decision_time strictly increasing; label_end_time one day later
    (or same day if embargo_free_end) so embargo math stays simple."""
    from datetime import datetime, timedelta, timezone
    base = datetime(2024, 1, 1, 21, 0, 0, tzinfo=timezone.utc)
    header = ["decision_time", "label_end_time", "label"] + features
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        for i in range(n):
            decision = base + timedelta(days=i)
            end = decision if embargo_free_end else decision + timedelta(days=1)
            row = [decision.isoformat().replace("+00:00", ".000Z").replace("T", "T"),
                  end.isoformat().replace("+00:00", ".000Z"),
                  label_fn(i)] + [str(v) for v in feature_fn(i)]
            writer.writerow(row)


def test_main_skips_below_min_samples(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_bot_dataset(dataset_path, 5, FEATURES,
                     label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                     feature_fn=lambda i: [1.0 if i % 2 else -1.0, 60.0])
    with pytest.raises(SystemExit) as excinfo:
        tbm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "200"])
    assert excinfo.value.code == tbm.EXIT_SKIPPED
    assert "min-samples" in capsys.readouterr().err


def test_main_skips_one_class(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_bot_dataset(dataset_path, 10, FEATURES,
                     label_fn=lambda i: "NO_TRADE",
                     feature_fn=lambda i: [0.0, 10.0])
    with pytest.raises(SystemExit) as excinfo:
        tbm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "5"])
    assert excinfo.value.code == tbm.EXIT_SKIPPED
    assert "one-class" in capsys.readouterr().err


def test_main_skips_when_ml_env_missing(tmp_path, monkeypatch, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_bot_dataset(dataset_path, 10, FEATURES,
                     label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                     feature_fn=lambda i: [1.0 if i % 2 else -1.0, 60.0])

    real_import = builtins.__import__
    blocked = {"onnxruntime", "xgboost", "onnxmltools", "skl2onnx"}

    def blocking_import(name, *args, **kwargs):
        top = name.split(".")[0]
        if top in blocked:
            raise ImportError(f"simulated missing dependency: {name}")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", blocking_import)
    with pytest.raises(SystemExit) as excinfo:
        tbm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "5"])
    assert excinfo.value.code == tbm.EXIT_SKIPPED
    assert "ML environment" in capsys.readouterr().err


def test_main_skips_when_no_fold_evaluable(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    make_manifest(manifest_path, FEATURES)
    make_bot_dataset(dataset_path, 20, FEATURES,
                     label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                     feature_fn=lambda i: [1.0 if i % 2 else -1.0, 60.0])
    # An impossibly high min-fold-train forces every fold's training block to be reported
    # "unusable", so evaluated stays 0 and the whole run is skipped.
    with pytest.raises(SystemExit) as excinfo:
        tbm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(tmp_path / "out"), "--min-samples", "5",
                 "--min-fold-train", "100000", "--folds", "2"])
    assert excinfo.value.code == tbm.EXIT_SKIPPED
    assert "no walk-forward fold" in capsys.readouterr().err


# --------------------------------------------------------------------------- main(): full fit


def test_main_full_run_writes_exports_and_report(tmp_path, capsys):
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    out_dir = tmp_path / "out"
    make_manifest(manifest_path, FEATURES)
    # 30 rows: first 15 all NO_TRADE (will make an early fold's training block one-class ->
    # "skipped" branch), remaining 15 alternate LONG/SHORT with dir/strength agreeing with the
    # label so the composite-sign baseline is exercised meaningfully too.
    def label_fn(i):
        if i < 15:
            return "NO_TRADE"
        return "LONG" if i % 2 == 0 else "SHORT"

    def feature_fn(i):
        if i < 15:
            return [0.0, 10.0]
        return [1.0 if i % 2 == 0 else -1.0, 60.0]

    make_bot_dataset(dataset_path, 30, FEATURES, label_fn, feature_fn)
    rc = tbm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                  "--out-dir", str(out_dir), "--min-samples", "10", "--folds", "2",
                  "--embargo-days", "0", "--min-fold-train", "3", "--max-iter", "50",
                  "--xgb-estimators", "5"])
    assert rc == 0
    assert (out_dir / "bot-logreg.onnx").is_file()
    assert (out_dir / "bot-xgb.onnx").is_file()
    report = json.loads((out_dir / "training-report.json").read_text(encoding="utf-8"))
    assert report["dataset"]["rows"] == 30
    assert report["evaluated_folds"] >= 1
    assert "logistic_regression" in report["means_over_evaluated_folds"]
    assert "baseline_composite_sign" in report["means_over_evaluated_folds"]
    assert any(f.get("skipped") for f in report["folds"])  # the monochrome fold
    out = capsys.readouterr().out
    assert "rows" in out
    assert "wrote" in out


def test_main_parity_failure_writes_nothing(tmp_path, capsys):
    # worst |prob diff| is always >= 0.0; a negative tolerance guarantees the parity check
    # fails deterministically without needing a real ONNX/sklearn disagreement.
    dataset_path = tmp_path / "dataset.csv"
    manifest_path = tmp_path / "manifest.json"
    out_dir = tmp_path / "out"
    make_manifest(manifest_path, FEATURES)
    make_bot_dataset(dataset_path, 20, FEATURES,
                     label_fn=lambda i: "LONG" if i % 2 else "SHORT",
                     feature_fn=lambda i: [1.0 if i % 2 else -1.0, 60.0])
    with pytest.raises(SystemExit) as excinfo:
        tbm.main(["--dataset", str(dataset_path), "--manifest", str(manifest_path),
                 "--out-dir", str(out_dir), "--min-samples", "5", "--folds", "2",
                 "--embargo-days", "0", "--min-fold-train", "3", "--max-iter", "50",
                 "--xgb-estimators", "5", "--parity-tolerance", "-1.0"])
    assert excinfo.value.code == 1
    err = capsys.readouterr().err
    assert "disagrees with the trained model" in err
    assert not (out_dir / "bot-logreg.onnx").exists()
    assert not (out_dir / "bot-xgb.onnx").exists()
