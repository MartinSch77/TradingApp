# Risk Register

Real, running register (`work-products/risk-register.md`'s spec). Scored per
`strategies/risk-management-strategy.md` (likelihood × impact, 1–25).

| ID | Source | Description | Likelihood | Impact | Score | Category | Owner | Status | Last reviewed | Treatment |
|---|---|---|---|---|---|---|---|---|---|---|
| RISK-001 | Engineering investigation (`tools/squish_run.sh`'s own in-script record) | Coco GUI coverage still produces no `.csexe`. UPDATED 2026-08-13: env-var delivery to the AUT is now CONFIRMED working (verified live via `/proc/<pid>/environ` against the real instrumented binary) — that was never the cause. Root cause narrowed to Coco's CoverageScanner writing its report at `exit()` by default while Squish terminates the AUT instead; `--cs-dump-on-signal=SIGTERM` was instrumented and tested with a manual `kill -TERM`, which neither dumped nor terminated the process — something in Squish's hooked runtime intercepts SIGTERM. | 2 | 2 | 4 | Technical | Unassigned | open | 2026-08-13 | Rebuild with `--cs-dump-on-signal=SIGUSR1` and repeat the manual-signal test (documented next step in `tools/squish_run.sh`); escalate to Squish/froglogic support if that also fails — likelihood/impact lowered from the original 3/2 now that delivery is a closed question and the remaining unknown is narrower |
| RISK-002 | Requirements review (this session) | REQ-F-034 and REQ-F-035 each bundle roughly ten independent obligations under one requirement id, weakening the meaning of "traced to a test" for either (~15 and ~23 existing test references respectively cite the bundled id rather than a specific obligation). | 3 | 3 | 9 | Process | Requirements Engineer | mitigating | 2026-08-13 | Scoped atomization plan recorded in `process/requirements-schema-upgrade.md`; tracked via a `process-improvement` GitHub issue rather than split without individual test re-verification |
| RISK-003 | QA self-audit (`tools/qa_report.py`, first run) | `CHANGELOG.md` (application-level) does not exist at the repository root, though several work-product specs (`work-products/project-plan.md`, `configuration-baseline.md`) assume its presence as planning/release evidence. | 2 | 2 | 4 | Process | Project Manager | open | 2026-08-13 | Either create it (aggregating git tag history) or correct the work-product specs to name the actual source of truth (git tags/GitHub Releases) — a `process-improvement` candidate |

No Critical (20–25) or currently-untreated High (13–19) risk is open as of
this register's creation.
