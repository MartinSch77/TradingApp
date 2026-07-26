#!/usr/bin/env bash
# Fetch the pinned PlantUML release used for the Doxygen diagrams (the jar is
# not committed; see docs/tools.md for the tool inventory).
set -euo pipefail
VERSION="1.2026.0"
DEST="$(cd "$(dirname "$0")" && pwd)/third-party/plantuml.jar"
mkdir -p "$(dirname "$DEST")"
curl -sL -o "$DEST" \
    "https://github.com/plantuml/plantuml/releases/download/v${VERSION}/plantuml-${VERSION}.jar"
java -jar "$DEST" -version | head -1
