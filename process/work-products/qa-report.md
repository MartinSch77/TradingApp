# Work Product: QA Report (process-conformance bundle)

**Produced by:** SUP.1. **Owning role:** Quality Assurance.
**Location:** `downloads/TradingApp-qa-report.md` (`tools/qa_report.py`).

Not to be confused with `quality-report.md` (SWE.6's engineering-verification
PDF) — see `process-model.md` §2's distinction table. This is the ONE report
in the project answering "was the process followed," never "is the product
correct."

## Content rules

One section per process in `process-model.md`'s landscape table, each
stating:

- **Conformance verdict**: `CONFIRMED` / `NOT FOUND` / `PARTIAL`, with the
  exact evidence checked and its result — never a bare verdict with no
  citation.
- **Effectiveness/efficiency indicators** (`process-model.md` §6), where
  available, kept visually/structurally distinct from the conformance
  verdict so the two are never read as one number.
- A closing section listing every OPEN finding from the previous report and
  whether it is now resolved, still open, or escalated to `MAN.5`.

## Quality criteria

Every process in the landscape appears — an omission is itself a defect in
this work product (`processes/SUP.1`'s own Task 4). No verdict is asserted
without a checked, reproducible piece of evidence (a file path, a command,
a git log query) named alongside it.

## Review requirement

None — QA has no reviewer above it in this framework other than `PIM.3`'s
effectiveness lens (`process-model.md` §7's "avoiding infinite regress"
note).

## KPIs

CONFIRMED ratio across processes (informational trend, not a gate); recurring
NOT FOUND count (feeds `PIM.3`'s trigger threshold).

## Traceability

Each verdict cites the process file it audited against and the work product
file(s) whose evidence it checked.
