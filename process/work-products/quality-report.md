# Work Product: Quality Report (engineering verification bundle)

**Produced by:** SWE.6 / `REL-product-release.md`. **Owning role:**
Qualification/System Test Engineer, Release Engineer.
**Location:** `downloads/TradingApp-quality-report.pdf`
(`tools/make_report.py`), `downloads/TradingApp-axivion-report.pdf`
(`tools/axivion_report.sh`).

## Content rules

One colour PDF covering the FULL `build_all.sh` run's evidence: tests,
traceability, static analysis, coverage, sanitizers, the Axivion dashboard
result. Licence-bound stages (Squish/Coco/Test Center) are listed as MISSING
LICENCES when genuinely absent — never silently omitted, per `CLAUDE.md`'s
own documented fix for the false-negative version of this defect.

## Quality criteria

Newer than the test results it summarizes (`REL`'s own gate: "the quality
PDF newer than the test results"). This is Verification's own summary
document — **not** to be confused with `qa-report.md` below, which is a
DIFFERENT work product asking a different question (process conformance, not
product correctness) per `process-model.md` §2.

## Review requirement

None beyond the staleness/completeness mechanical check.

## KPIs

Freshness (age relative to the sources it reports on); count of stages
reported MISSING LICENCE vs. actually run.

## Traceability

Aggregates `test-report.md` and `static-analysis-report.md`; does not
introduce new traceable facts of its own.
