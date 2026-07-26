#!/usr/bin/env bash
# Conditional SonarQube scan: runs ONLY when a SonarQube server is actually
# reachable and sonar-scanner is installed — otherwise it reports why and
# exits 0 (the pipeline is not failed by an absent optional tool).
#
# After a successful scan the open issues are pulled from the server's Web API
# and normalized into analysis-results/sonarqube.txt (file|line|severity|rule|
# message), which axivion/external_import.py brings onto the Axivion dashboard
# as provider "sonarqube".
#
# Env: SONAR_HOST_URL (default http://localhost:9000), SONAR_TOKEN (if the
# server requires authentication).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/analysis-results"
HOST="${SONAR_HOST_URL:-http://localhost:9000}"
mkdir -p "$OUT"

if ! command -v sonar-scanner >/dev/null 2>&1; then
    echo "sonar-scanner not installed — SonarQube scan skipped."
    echo "(install: https://docs.sonarsource.com/sonarqube/latest/analyzing-source-code/scanners/sonarscanner/)"
    exit 0
fi
if ! curl -sf --max-time 5 "$HOST/api/system/status" | grep -q '"status":"UP"'; then
    echo "no SonarQube server reachable at $HOST — scan skipped (start one, or set SONAR_HOST_URL)."
    exit 0
fi

echo "== sonar-scanner against $HOST =="
(cd "$ROOT" && sonar-scanner -Dsonar.host.url="$HOST" \
    ${SONAR_TOKEN:+-Dsonar.token=$SONAR_TOKEN}) || exit 1

# Export open issues -> pipe format for the dashboard import.
curl -sf ${SONAR_TOKEN:+-u "$SONAR_TOKEN:"} \
    "$HOST/api/issues/search?componentKeys=TradingApp&resolved=false&ps=500" \
    | python3 - "$OUT/sonarqube.txt" <<'EOF'
import json, sys
data = json.load(sys.stdin)
rows = []
for issue in data.get("issues", []):
    comp = issue.get("component", "")            # e.g. TradingApp:src/x.cpp
    path = comp.split(":", 1)[1] if ":" in comp else comp
    line = issue.get("line", 1)
    sev = issue.get("severity", "MAJOR").lower()
    rule = issue.get("rule", "sonarqube")
    msg = issue.get("message", "").replace("|", "/")
    rows.append(f"{path}|{line}|{sev}|{rule}|{msg}")
with open(sys.argv[1], "w") as f:
    f.write("\n".join(rows) + ("\n" if rows else ""))
print(f"sonarqube: {len(rows)} open issues -> {sys.argv[1]}")
EOF
