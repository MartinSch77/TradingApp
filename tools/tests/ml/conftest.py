# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Makes tools/ml/*.py importable by module name, same convention as
tools/tests/conftest.py one level up — kept separate because this half of
the suite runs under the ML venv's own pytest (tools/ml/requirements.txt),
never the pipx one, since these modules need numpy/sklearn/xgboost/onnxruntime."""

import sys
from pathlib import Path

ML_DIR = Path(__file__).resolve().parent.parent.parent / "ml"
if str(ML_DIR) not in sys.path:
    sys.path.insert(0, str(ML_DIR))
