# Work Product: Problem Report

**Produced by:** SUP.9. **Owning role:** Problem/Incident Manager.
**Location:** GitHub Issue (`.github/ISSUE_TEMPLATE/problem-report.yml`).

## Content rules

Severity (`strategies/problem-resolution-strategy.md`'s scale), reproduction
steps or the automated evidence that detected it, root cause once analyzed,
the fix commit/PR, and the verifying evidence link at closure. State field
uses the shared six-state scheme; **never closed from `implemented` directly
— must pass through `verified`.**

## Quality criteria

A closure without a verifying-evidence link is INVALID — QA's sampling
(`strategies/quality-assurance-strategy.md`) specifically targets this.

## Review requirement

None beyond the closure-evidence check (a problem report is evidence, not an
authored narrative to review for quality of prose).

## KPIs

Mean time to resolution by severity; escape rate (Critical/High problems
found post-release vs. pre-release, per `strategies/problem-resolution-
strategy.md`'s severity scale).

## Traceability

Issue ↔ fix commit/PR ↔ the test that now catches the regression (a fix with
no regression test is itself flagged in review).
