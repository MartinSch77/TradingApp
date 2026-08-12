# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Sphinx configuration — developer handbook over the repository's markdown
# documentation (MyST). Built by tools/make_docs.sh into docs/sphinx-html/
# when sphinx-build is installed (pipx install sphinx; pipx inject sphinx
# myst-parser — ./setup.sh does both). Complements, not replaces, the Doxygen
# API reference (docs/html/) and the StrictDoc requirements export
# (docs/strictdoc/).

project = "TradingApp"
author = "Martin Schuler"
copyright = "2026, Martin Schuler"

extensions = ["myst_parser"]
source_suffix = {".md": "markdown"}
root_doc = "sphinx_index"

exclude_patterns = [
    "html",          # Doxygen output
    "strictdoc",     # StrictDoc output
    "sphinx-html",   # our own output
    "_build",
]

# The markdown pages double as Doxygen pages and carry @page/@ref directives;
# MyST renders them verbatim, which is acceptable for the handbook view.
suppress_warnings = ["myst.xref_missing", "myst.header"]

html_theme = "alabaster"
html_title = "TradingApp developer handbook"
