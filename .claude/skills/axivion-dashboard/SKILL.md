---
name: axivion-dashboard
description: Run the Axivion analysis for TradingApp and verify the result on the local dashboard (versions, providers, finding deltas) via the REST API. Use when the user wants the dashboard updated, asks why providers/findings are missing, or wants finding counts per tool.
---

# Axivion run + dashboard verification (TradingApp)

## Running the analysis
- Exactly ONE run at a time: `./axivion/start_analysis.sh` holds a lock
  (`${TMPDIR:-/tmp}/.axivion-TradingApp.lock`); a second run aborts with
  "already active".
  Never run `clean_all.sh` or another `build_all.sh` while it analyses —
  concurrent runs delete each other's `build_axivion/` IR before upload.
- Full run ≈ 30–60 min. Start it in the background; while it runs, do not
  create or modify repository files (the fossil shadow phase snapshots the
  tree at the end).
- The run imports every external log present in `analysis-results/`:
  providers cppcheck, clang-tidy, clazy, gcc-analyzer, msvc-analyze,
  codespell, sonarqube, asan-ubsan, tsan, valgrind, asan, ubsan
  (configured in `axivion/external_import.py` — a Python config
  layer registered in `axivion_config.json`; matchers CANNOT be expressed in
  the JSON files, the Suite validator requires real teecap.Match objects).
  Run `tools/static_analysis.sh build` and/or `tools/sanitize.sh` first if
  the dashboard should reflect current results. Empty logs = clean = the
  provider shows no open findings (that is correct, not a bug).

## Verifying on the dashboard (REST, read-only)
Credentials for the local dashboard come from `axivion/start_analysis.sh`.

```bash
# versions (index/date):
curl -s -u admin:password 'http://localhost:9090/axivion/api/projects/TradingApp' \
  | python3 -c "import json,sys; print([(v['index'],v['name']) for v in json.load(sys.stdin)['versions']])"
# SV delta between two versions (state added/removed, provider, rule, path):
curl -s -u admin:password \
  'http://localhost:9090/axivion/api/projects/TradingApp/issues?kind=SV&start=<N>&end=<M>&state=changed'
```

Notes: the axdashboard MCP server spawns its own empty dashboard instance on a
random port — it cannot see the :9090 uploads; use the REST API for checks.
A few Axivion CWE-200 findings flap between otherwise identical runs
(semantic-analysis query timeout) — not caused by your change.
