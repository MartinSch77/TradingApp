# Risk Register

Real, running register (`work-products/risk-register.md`'s spec). Scored per
`strategies/risk-management-strategy.md` (likelihood × impact, 1–25).

| ID | Source | Description | Likelihood | Impact | Score | Category | Owner | Status | Last reviewed | Treatment |
|---|---|---|---|---|---|---|---|---|---|---|
| RISK-001 | Engineering investigation (`tools/squish_run.sh`'s own in-script record) | Coco GUI-coverage instrumentation does not confirm delivery to the AUT via any of four attempted `COVERAGESCANNER_ARGS` delivery mechanisms (plain export, `--envvar`, appending to `envvars` before/after `--config addAUT`, `--envvars <tempfile>`). Root cause not confirmed — a false "TRADINGAPP_FORCE_SIMULATION arrived" signal was found to be a false positive for delivery, not proof of it. | 3 | 2 | 6 | Technical | Unassigned | open | 2026-08-13 | Accept and monitor; needs Squish support or `--verbose` server tracing this session did not have time for |
| RISK-002 | Requirements review (this session) | REQ-F-034 and REQ-F-035 each bundle roughly ten independent obligations under one requirement id, weakening the meaning of "traced to a test" for either (~15 and ~23 existing test references respectively cite the bundled id rather than a specific obligation). | 3 | 3 | 9 | Process | Requirements Engineer | mitigating | 2026-08-13 | Scoped atomization plan recorded in `process/requirements-schema-upgrade.md`; tracked via a `process-improvement` GitHub issue rather than split without individual test re-verification |
| RISK-003 | QA self-audit (`tools/qa_report.py`, first run) | `CHANGELOG.md` (application-level) does not exist at the repository root, though several work-product specs (`work-products/project-plan.md`, `configuration-baseline.md`) assume its presence as planning/release evidence. | 2 | 2 | 4 | Process | Project Manager | open | 2026-08-13 | Either create it (aggregating git tag history) or correct the work-product specs to name the actual source of truth (git tags/GitHub Releases) — a `process-improvement` candidate |

No Critical (20–25) or currently-untreated High (13–19) risk is open as of
this register's creation.
