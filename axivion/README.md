# Axivion configuration

MISRA C++ 2023 style analysis + architecture checks for TradingApp; run via
`./axivion/start_analysis.sh` (project DB served by the local dashboard).

## External-tool integration (clang-tidy / cppcheck / clazy)

Pipeline that brings the third-party analyzer output onto the dashboard:

1. `tools/static_analysis.sh build` runs **cppcheck** and **clang-tidy**
   (plus **clazy** — Qt coding rules, levels 0–1 — when installed:
   `sudo apt install clazy`) over the compile database and merges every
   finding into `analysis-results/external_findings.csv`.
2. The custom rule `axivion/rules/external_findings.py`
   (`ExternalFindings-Import`, rule group *Stylechecks*) re-emits each CSV row
   as a style violation during the next `axivion_ci` run — tool and original
   rule id are carried in the message, so dashboard filtering/suppression and
   delta views work exactly as for native rules.

One-time registration: `axivion_config axivion/axivion_config.json` →
right-click *Analysis* → *Additional rules…* → select `axivion/rules` →
enable *ExternalFindings-Import*. The rule is a no-op when the CSV is absent.
Validate the source-location factory against your Suite version on first run
(the rule tries the known API spellings and documents itself).

Current state (2026-07-25): cppcheck and clang-tidy report **0 findings**
(40 were fixed in commit history); clazy pending installation (needs sudo).
