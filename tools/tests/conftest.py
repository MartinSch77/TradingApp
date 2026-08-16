# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Makes the repo's stdlib-only Python scripts importable by module name (they
are flat scripts, not a package — no __init__.py, mirroring the convention the
tools already use among themselves, e.g. tools/ml/bot_dataset.py's `import
crowd_dataset`). Covers tools/, axivion/ and packaging/ — every directory with
scripts this suite tests outside tools/ml (which gets its own conftest.py,
run through the ML venv's own pytest instead)."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
for _dir in (ROOT / "tools", ROOT / "axivion", ROOT / "packaging"):
    if str(_dir) not in sys.path:
        sys.path.insert(0, str(_dir))
