# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/ml/export_finbert.py — the FinBERT ONNX exporter.

The heavy optimum/transformers/torch stack is deliberately NOT a project dependency (the
module's own docstring says so), so these tests mock it out via sys.modules rather than
requiring it to be installed. Covers: the "exporter stack not installed" skip path (real,
since optimum/transformers are absent in the ml venv), the happy path with a faked exporter,
and the "exporter produced no model.onnx" failure branch.
"""

from __future__ import annotations

import json
import sys
import types

import pytest

import export_finbert as ef


class Args:
    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)


def run_main(monkeypatch, argv):
    monkeypatch.setattr(sys, "argv", ["export_finbert.py"] + argv)
    return ef.main()


# --------------------------------------------------------------------------- skip path (real)


def test_main_skips_when_exporter_stack_missing(tmp_path, monkeypatch, capsys):
    # The module's own docstring says optimum/transformers are deliberately NOT a project
    # dependency, but this particular ml venv happens to have them installed (measured), so
    # merely evicting them from sys.modules would let Python re-import from disk and actually
    # download+export a real model over the network. Force the ImportError instead by
    # intercepting builtins.__import__ for exactly the two module families export_finbert
    # imports inside its try/except.
    import builtins
    real_import = builtins.__import__

    def blocking_import(name, *args, **kwargs):
        if name == "optimum.exporters.onnx" or name.startswith("transformers"):
            raise ImportError(f"simulated missing dependency: {name}")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", blocking_import)
    monkeypatch.setattr(sys, "argv", ["export_finbert.py", "--out", str(tmp_path / "out")])
    rc = ef.main()
    assert rc == ef.EXIT_SKIPPED
    err = capsys.readouterr().err
    assert "skipped" in err
    assert "pip install" in err


# --------------------------------------------------------------------------- happy path (mocked)


def _install_fake_optimum_transformers(monkeypatch, produce_model=True):
    """Installs fake `optimum.exporters.onnx` and `transformers` modules into sys.modules so
    export_finbert's imports succeed without the real (multi-hundred-MB) stack."""
    calls = {}

    def fake_main_export(model_name_or_path, output, task):
        calls["model_name_or_path"] = model_name_or_path
        calls["output"] = output
        calls["task"] = task
        if produce_model:
            from pathlib import Path
            (Path(output) / "model.onnx").write_bytes(b"fake-onnx-bytes")

    optimum_pkg = types.ModuleType("optimum")
    optimum_exporters_pkg = types.ModuleType("optimum.exporters")
    optimum_onnx_mod = types.ModuleType("optimum.exporters.onnx")
    optimum_onnx_mod.main_export = fake_main_export

    class FakeTokenizer:
        @staticmethod
        def from_pretrained(model_id):
            return FakeTokenizer()

        def get_vocab(self):
            return {"[PAD]": 0, "[CLS]": 1, "hello": 2}

    class FakeConfig:
        def __init__(self):
            self.id2label = {0: "negative", 1: "neutral", 2: "positive"}

        @staticmethod
        def from_pretrained(model_id):
            return FakeConfig()

    transformers_mod = types.ModuleType("transformers")
    transformers_mod.AutoTokenizer = FakeTokenizer
    transformers_mod.AutoConfig = FakeConfig

    monkeypatch.setitem(sys.modules, "optimum", optimum_pkg)
    monkeypatch.setitem(sys.modules, "optimum.exporters", optimum_exporters_pkg)
    monkeypatch.setitem(sys.modules, "optimum.exporters.onnx", optimum_onnx_mod)
    monkeypatch.setitem(sys.modules, "transformers", transformers_mod)
    return calls


def test_main_happy_path_writes_all_four_files(tmp_path, monkeypatch, capsys):
    calls = _install_fake_optimum_transformers(monkeypatch, produce_model=True)
    out_dir = tmp_path / "finbert"
    rc = run_main(monkeypatch, ["--out", str(out_dir)])
    assert rc == 0
    assert (out_dir / "model.onnx").read_bytes() == b"fake-onnx-bytes"
    vocab_lines = (out_dir / "vocab.txt").read_text(encoding="utf-8").splitlines()
    assert vocab_lines == ["[PAD]", "[CLS]", "hello"]
    labels_lines = (out_dir / "labels.txt").read_text(encoding="utf-8").splitlines()
    assert labels_lines == ["negative", "neutral", "positive"]
    info = json.loads((out_dir / "export-info.json").read_text(encoding="utf-8"))
    assert info["model"] == "ProsusAI/finbert"
    assert info["labels"] == ["negative", "neutral", "positive"]
    assert "huggingface.co" in info["licence"]
    assert calls["task"] == "text-classification"
    out = capsys.readouterr().out
    assert "wrote model.onnx, vocab.txt (3 tokens)" in out


def test_main_custom_model_id_used_throughout(tmp_path, monkeypatch):
    calls = _install_fake_optimum_transformers(monkeypatch, produce_model=True)
    out_dir = tmp_path / "finbert"
    rc = run_main(monkeypatch, ["--model", "some-org/custom-model", "--out", str(out_dir)])
    assert rc == 0
    assert calls["model_name_or_path"] == "some-org/custom-model"
    info = json.loads((out_dir / "export-info.json").read_text(encoding="utf-8"))
    assert info["model"] == "some-org/custom-model"
    assert "some-org/custom-model" in info["licence"]


def test_main_exporter_produces_no_model_onnx(tmp_path, monkeypatch, capsys):
    _install_fake_optimum_transformers(monkeypatch, produce_model=False)
    out_dir = tmp_path / "finbert"
    rc = run_main(monkeypatch, ["--out", str(out_dir)])
    assert rc == 1
    err = capsys.readouterr().err
    assert "produced no model.onnx" in err


def test_main_out_dir_created_if_missing(tmp_path, monkeypatch):
    _install_fake_optimum_transformers(monkeypatch, produce_model=True)
    out_dir = tmp_path / "nested" / "finbert"
    assert not out_dir.exists()
    rc = run_main(monkeypatch, ["--out", str(out_dir)])
    assert rc == 0
    assert out_dir.is_dir()
