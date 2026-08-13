# Process Improvement Strategy

Referenced by `processes/PIM.3-process-improvement.md`.

## Trigger thresholds

- A NOT FOUND/PARTIAL QA finding repeated on **2 consecutive** QA reports for
  the same process → mandatory PIM.3 review (not optional).
- An effectiveness indicator (`process-model.md` §6) degrading for **3
  consecutive** cycles → mandatory PIM.3 review.
- Any role may request a PIM.3 review at any time regardless of threshold.

## Improvement lifecycle

Uses the SAME `proposed → analyzed → approved → implemented → verified →
closed` state scheme as `SUP.9`/`SUP.10`, since a process improvement IS a
change request against `process/*.md` and goes through `SUP.10` for its
actual approval — PIM.3 originates and justifies the proposal;
`SUP.10`/the Process Owner (`process-model.md` §8) approves it.

## What counts as "verified" for a process improvement

Not merely "the document changed" — the NEXT QA report after the change must
show the targeted indicator improved (finding no longer recurring, or the
effectiveness metric back within its prior range). An improvement whose
document changed but whose indicator did not move is reopened, not closed.
