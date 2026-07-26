#!/usr/bin/env bash
# Build the HTML documentation: refresh the traceability matrix (so the docs
# always ship the current trace state), fetch PlantUML if missing, run doxygen.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

[ -f tools/third-party/plantuml.jar ] || tools/fetch_plantuml.sh
tools/make_requirements.sh              # StrictDoc export + regenerated requirements.md
python3 tools/trace_report.py || true   # gaps are reported inside the matrix
doxygen Doxyfile
# Belt-and-braces: render the collected PlantUML blocks explicitly (doxygen
# writes them to one .pu file; the named @startuml blocks become <name>.svg).
if [ -f docs/html/inline_umlgraph_svghtml.pu ]; then
    java -Djava.awt.headless=true -jar tools/third-party/plantuml.jar \
        -tsvg -o "$ROOT/docs/html" docs/html/inline_umlgraph_svghtml.pu
fi
echo "docs: $ROOT/docs/html/index.html"
