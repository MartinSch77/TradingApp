#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Build the TINY text-classifier fixture tst_finbert drives (REQ-F-044).

A real FinBERT is a 400+ MB download nobody's test suite should need; this writes a
BERT-SHAPED stand-in — the same input/output contract (input_ids / attention_mask /
token_type_ids int64 [1, n] -> logits float [1, 3]) over a seven-token vocabulary, where
"good" pushes the positive logit and "bad" the negative one. Deterministic, ~1 kB, and enough
to prove the tokenizer, the runner and the collector end to end. Needs only the onnx package
(the ml venv has it). Usage: make_finbert_fixture.py <out-dir>
"""

import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

VOCAB = ["[PAD]", "[UNK]", "[CLS]", "[SEP]", "good", "bad", "flat"]
LABELS = ["positive", "negative", "neutral"]


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)

    # One row of logit-contributions per vocabulary token: "good" -> +2 positive,
    # "bad" -> +2 negative, everything else contributes nothing.
    table = np.zeros((len(VOCAB), len(LABELS)), dtype=np.float32)
    table[VOCAB.index("good")] = [2.0, 0.0, 0.0]
    table[VOCAB.index("bad")] = [0.0, 2.0, 0.0]

    ids = helper.make_tensor_value_info("input_ids", TensorProto.INT64, [1, "n"])
    mask = helper.make_tensor_value_info("attention_mask", TensorProto.INT64, [1, "n"])
    types = helper.make_tensor_value_info("token_type_ids", TensorProto.INT64, [1, "n"])
    logits = helper.make_tensor_value_info("logits", TensorProto.FLOAT, [1, len(LABELS)])
    graph = helper.make_graph(
        nodes=[
            helper.make_node("Gather", ["table", "input_ids"], ["per_token"], axis=0),
            helper.make_node("ReduceSum", ["per_token", "sum_axis"], ["logits"],
                             keepdims=0),
        ],
        name="tiny_finbert",
        inputs=[ids, mask, types],
        outputs=[logits],
        initializer=[
            numpy_helper.from_array(table, name="table"),
            numpy_helper.from_array(np.array([1], dtype=np.int64), name="sum_axis"),
        ],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 18)])
    onnx.checker.check_model(model)
    (out / "model.onnx").write_bytes(model.SerializeToString())
    (out / "vocab.txt").write_text("\n".join(VOCAB) + "\n", encoding="utf-8")
    (out / "labels.txt").write_text("\n".join(LABELS) + "\n", encoding="utf-8")
    print(f"fixture written to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
