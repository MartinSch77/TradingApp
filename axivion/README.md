# Axivion configuration

MISRA C++ 2023 style analysis + architecture checks for TradingApp; run via
`./axivion/start_analysis.sh` (project DB served by the local dashboard).

## External-tool integration (clang-tidy / cppcheck / clazy)

Pipeline that brings the third-party analyzer output onto the dashboard,
using the Suite's official import mechanism (reference manual 6.2.10
*ImportExternalAnalysisOutput* + 6.2.4.4 *ExternalAnalysisFormats*):

1. `tools/static_analysis.sh build` runs **cppcheck** and **clang-tidy**
   (plus **clazy** — Qt coding rules, levels 0–1 — when installed:
   `sudo apt install clazy`) over the compile database and writes one log per
   tool to `analysis-results/`.
2. The configuration layer `axivion/external_import.py` (listed in
   `axivion_config.json`) creates one `ImportExternalAnalysisOutput` +
   `GenericFormat` copy per tool. During the next `axivion_ci` run each copy
   cats its log and re-emits every finding line as a style violation —
   *provider* = tool name, *errno* = the tool's own rule id — so dashboard
   filtering, suppression and delta views work exactly as for native rules.
   A missing log imports nothing (`check_returncode = False`), so analysis
   runs without a prior `static_analysis.sh` are unaffected.

No manual registration step is needed: the layer is plain configuration and
travels with the repository. (An earlier custom-rule scaffold under
`axivion/rules/` was replaced by this official mechanism; see git history.)

Current state (2026-07-25): cppcheck and clang-tidy report **0 findings**
(40 were fixed in commit history); clazy 1.11 is installed and wired in.
The import pipeline was verified end-to-end with synthetic findings — one per
tool — which appeared on the dashboard under their respective providers and
disappeared again after re-running with the real, clean logs.
