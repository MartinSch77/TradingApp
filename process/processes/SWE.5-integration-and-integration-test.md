# SWE.5 — Integration and Integration Test

## Purpose

Integrate units into the layered whole (domain ← services ← ui) and verify
the INTEGRATION itself — interactions between units the unit level cannot
see (e.g. `EtoroClient` against an in-process mock HTTP server, config
layering, the simulation engine's cost model composed with the paper-trading
book).

## Inputs

- Unit-verified components from `SWE.4`.

## Outputs / Work Products

- The "integration tests" subset of `tests/` (per `tests/CMakeLists.txt`'s
  own comment: "unit tests over the pure domain layer plus integration tests
  over the services"), e.g. `tst_etoroclient`, `tst_config`,
  `tst_simulationengine`, `tst_jsonhttp`, `tst_marketfeeds`.

## Tasks

1. Integrate per the architecture's layer order; a component is only
   integrated once its own unit verification is evidenced (Task 2 of
   `SWE.4`), never in parallel with unverified units it depends on.
2. Test integration-level behavior specifically: retry/backoff, rate-limit
   handling, cross-module state consistency — never re-testing what a unit
   test already covers (this project's own "don't duplicate a clone" ethos
   applied to test scope, not just code).
3. Define minimum coverage/acceptance criteria for integration in
   `strategies/verification-strategy.md` §Integration — this project's stated
   answer, since ASPICE leaves the specific bar to the project.

## Roles

Integration Engineer (Responsible/Accountable) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

SWE.5.BP1 (specify integration test strategy) → realized by
`strategies/verification-strategy.md`. SWE.5.BP2–BP3 (integrate, verify) →
Tasks 1–2. SWE.5.BP5 (traceability) → the same `tools/trace_report.py` join,
since integration tests carry the same `@tstid`/`@relation` tags as unit
tests in this project's single test suite.

## Verification / QA Hooks

QA confirms the integration test subset actually exercises cross-component
paths (not merely re-running unit-scope assertions under an "integration"
label) by sampling test bodies against `strategies/verification-strategy.md`'s
definition of integration scope.
