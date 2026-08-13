# SWE.4 — Unit Verification

## Purpose

Verify each software unit against its detailed design and requirements at
the smallest testable scope, and via static analysis.

## Inputs

- `docs/design.md` DES entries, `src/` implementation.

## Outputs / Work Products

- `tests/tst_*.cpp` (tagged `@tstid`/`@design`/`@relation(REQ-…,
  scope=function)`), `test-results/*.xml` (JUnit), `analysis-results/*`
  (the seven-analyzer output). See `work-products/unit-test-specification.md`,
  `work-products/test-report.md`, `work-products/static-analysis-report.md`.

## Tasks

1. Write a unit test for every requirement whose `VERIFICATION` field
   includes `T`, tagged so `tools/trace_report.py` can join it back to the
   requirement and design element.
2. Run the full suite (`tools/run_tests.sh`) and the seven-analyzer stage
   (`tools/static_analysis.sh`) per `strategies/verification-strategy.md`'s
   coverage/acceptance criteria before a change is considered unit-verified.
3. Investigate and either fix or explicitly, justifiedly suppress every
   finding — this project's non-negotiable: "every disabled check... carries
   a written reason AND the measured hit count" (`CLAUDE.md`).
4. Record results as `test-results/*.xml`; a result older than the source it
   verifies is treated as NO evidence, not stale-but-acceptable evidence
   (`CLAUDE.md`'s staleness rule, shared with `SUP.8`/release gating).

## Roles

Verification Engineer (Unit) (Responsible/Accountable) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

SWE.4.BP1 (develop test cases) → Task 1. SWE.4.BP2–BP3 (test, record results)
→ Tasks 2/4. SWE.4.BP5 (bidirectional traceability) →
`tools/trace_report.py`'s TS↔DES↔REQ join.

## Verification / QA Hooks

QA confirms `test-results/*.xml` for every unit test target is newer than
the source it verifies (`tools/gates_to_junit.py`'s own sources-only
staleness rule) and that `analysis-results/` totals match what
`strategies/verification-strategy.md` sets as the acceptance bar.
