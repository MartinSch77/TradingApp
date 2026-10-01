#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Fetch the pinned PlantUML release used for the Doxygen diagrams (the jar is
# not committed; see docs/tools.md for the tool inventory).
set -euo pipefail
VERSION="1.2026.0"
DEST="$(cd "$(dirname "$0")" && pwd)/third-party/plantuml.jar"
mkdir -p "$(dirname "$DEST")"
curl -sL -o "$DEST" \
    "https://github.com/plantuml/plantuml/releases/download/v${VERSION}/plantuml-${VERSION}.jar"
# PlantUML answers -version with exit status 16 (no diagram was generated), which
# set -e/pipefail would turn into a failed fetch — the download above is the result.
java -jar "$DEST" -version 2>/dev/null | head -1 || true
