# Work Product: Process Improvement Record

**Produced by:** PIM.3. **Owning role:** Process Improvement Lead.
**Location:** `process/CHANGELOG.md` (the approved/implemented record),
tracked as a GitHub Issue (`.github/ISSUE_TEMPLATE/process-improvement.yml`)
through its lifecycle.

## Content rules

The triggering QA finding or effectiveness trend, the process file(s)
proposed to change, the change itself (via `SUP.10`), and — the item that
makes this different from an ordinary change request — the indicator that
must move for this to count as `verified` (`strategies/process-improvement-
strategy.md`).

## Quality criteria

Never closed without checking the targeted indicator actually moved on the
NEXT QA report; a document change with no indicator movement is reopened.

## Review requirement

Same as `change-request.md` (it IS a change request against `process/`),
plus the Process Owner's release approval (`process-model.md` §8).

## KPIs

Ratio of improvements that measurably moved their target indicator; time
from trigger to `verified`.

## Traceability

QA finding/trend ↔ this record ↔ the `process/CHANGELOG.md` entry ↔ the next
QA report's re-check of the same indicator.
