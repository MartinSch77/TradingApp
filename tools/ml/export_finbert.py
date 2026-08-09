#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Export a financial text-sentiment model to ONNX for the in-app scorer (REQ-F-044).

Downloads a Hugging Face BERT sequence classifier (default: ProsusAI/finbert), exports it to
ONNX via optimum, and writes the THREE files the C++ FinBertSentiment runner reads:

    <out>/model.onnx    the graph (input_ids / attention_mask / token_type_ids -> logits)
    <out>/vocab.txt     the model's OWN WordPiece vocabulary
    <out>/labels.txt    the class of each output column, from the model's own config

    tools/ml/export_finbert.py --out ~/.config/TradingApp/eToro\\ Trader/finbert

The app picks the directory up via TRADINGAPP_FINBERT_DIR or `finbert/` in its config dir.

Offline, optional, and licence-aware: the model is downloaded under ITS OWN licence terms
(named below before anything is fetched), the extra dependencies (optimum + torch, several
hundred MB) are deliberately NOT in requirements.txt, and this script exits 3 ("skipped")
with the install command when they are absent. Python remains a development-time tool.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

EXIT_SKIPPED = 3


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", default="ProsusAI/finbert",
                        help="Hugging Face model id (a BERT sequence classifier)")
    parser.add_argument("--out", required=True, help="directory to write model.onnx into")
    args = parser.parse_args()

    print(f"Exporting {args.model} — downloaded under the MODEL'S OWN licence terms; see "
          f"https://huggingface.co/{args.model} before redistributing anything.")
    try:
        from optimum.exporters.onnx import main_export
        from transformers import AutoConfig, AutoTokenizer
    except ImportError as error:
        # optimum 2.x moved the ONNX exporter into its own optimum-onnx package.
        print(f"export_finbert: skipped: the exporter stack is not installed ({error}).\n"
              f"  Install into the ml venv (several hundred MB, CPU wheels):\n"
              f"  ~/.local/tradingapp-ml/bin/pip install optimum optimum-onnx torch "
              f"--extra-index-url https://download.pytorch.org/whl/cpu", file=sys.stderr)
        return EXIT_SKIPPED

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="finbert-export-") as scratch:
        main_export(model_name_or_path=args.model, output=scratch, task="text-classification")
        exported = Path(scratch) / "model.onnx"
        if not exported.is_file():
            print("export_finbert: the exporter produced no model.onnx", file=sys.stderr)
            return 1
        shutil.copyfile(exported, out / "model.onnx")

    tokenizer = AutoTokenizer.from_pretrained(args.model)
    vocab = tokenizer.get_vocab()  # token -> id
    lines = [""] * len(vocab)
    for token, index in vocab.items():
        lines[index] = token
    (out / "vocab.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # The labels file is AUTHORITATIVE for what the probability columns mean — written from
    # the model's own config, never assumed by the consumer.
    config = AutoConfig.from_pretrained(args.model)
    labels = [config.id2label[i] for i in range(len(config.id2label))]
    (out / "labels.txt").write_text("\n".join(labels) + "\n", encoding="utf-8")
    (out / "export-info.json").write_text(
        json.dumps({"model": args.model, "labels": labels,
                    "licence": f"see https://huggingface.co/{args.model}"}, indent=2) + "\n",
        encoding="utf-8")
    print(f"wrote model.onnx, vocab.txt ({len(vocab)} tokens), labels.txt "
          f"({'/'.join(labels)}) to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
