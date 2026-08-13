# Work Product: Test Report

**Produced by:** SWE.4, SWE.5, SWE.6. **Owning role:** Verification/
Qualification Engineer. **Location:** `test-results/*.xml` (JUnit),
`downloads/TradingApp-quality-report.pdf`'s test section.

## Content rules

JUnit XML with `tests`/`failures`/`errors` attributes per suite — the
authoritative pass/fail source (`CLAUDE.md`: "verify the test result from the
XML, not the console: console output can be truncated").

## Quality criteria

**Current**: newer than every source file it verifies
(`tools/gates_to_junit.py`/`publish_release.sh`'s shared sources-only
staleness rule — the SAME yardstick both tools use, deliberately, per
`CLAUDE.md`'s own documented fix for the two-tools-disagreeing defect).
**Complete**: 0 failures, 0 errors for the report to count as passing
evidence.

## Review requirement

None beyond the mechanical staleness/pass check above — a test report is
evidence, not an authored document to review for content quality.

## KPIs

Failure count (target 0), staleness (target: always current at release
gate — this is the ONE work product whose absence/staleness makes QA's
report a hard fail rather than informational, per `process-model.md`).

## Traceability

Suite name ↔ `TS-xxx` ids inside it ↔ `docs/test_spec.md`.
