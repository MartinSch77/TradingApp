# Requirements Schema Upgrade — record and atomization plan

Real record, not illustrative — tracks a genuine, in-progress SUP.10 change
against `requirements/requirements.sdoc`.

## What landed (this change)

`requirements/requirements.sdoc`'s `[GRAMMAR]` gained six OPTIONAL fields, so
every existing requirement stays valid unchanged:

- `SOURCE` — the stakeholder or measured finding behind the requirement.
- `RATIONALE` — why, distinct from what.
- `PRIORITY` — must / should / could.
- `STATUS` — `proposed | analyzed | approved | implemented | verified |
  superseded`.
- `ACCEPTANCE_CRITERIA` — the observable condition for `STATUS: verified`.
- `ISSUE` — the GitHub Issue/PR of record.

Verified via `strictdoc export` (no parse errors) and
`tools/trace_report.py` (0 hard gaps, unchanged) both before and after.

## What did NOT land yet, and why

REQ-F-034 and REQ-F-035 are each a single `STATEMENT` bundling roughly a
dozen and ten independently verifiable obligations respectively — a textbook
non-atomic requirement. Splitting them correctly requires re-classifying
every existing `@relation(REQ-F-034…)`/`@relation(REQ-F-035…)` tag across
~15 and ~23 test-function references (respectively) plus their
`docs/design.md` `satisfies` entries onto the CORRECT new atomic id — work
that needs careful, individual reading of each test to avoid the opposite
defect: a traceability link that is now WRONG rather than merely coarse.

Rather than rush that reclassification, both requirements were marked
`STATUS: proposed` with a `RATIONALE` stating the bundling explicitly, and
this file records the scoped plan below. This is itself the intended
behavior of `processes/SUP.1-quality-assurance.md`'s independence principle
applied to self-review: an honest "not done yet, tracked" beats a rushed
"done" that a QA audit would later have to un-confirm.

## Atomization plan (tracked as GitHub Issue — see the repository's Issues
for the live instance filed under the `process-improvement` template)

**REQ-F-034** splits into (working ids, finalized at implementation time):
churn control (cooldown + pace limit + minimum hold), reversal
conviction gate, reversal economics gate, session-phase sit-out windows,
reluctant-symbol handling, exit-reason-by-rule reporting, correlation-bucket
leverage cap (checked for overlap with REQ-F-031's own group-risk
requirement before finalizing — likely a duplicate, not a new atomic id),
session-structure reads (checked for overlap with REQ-F-022 before
finalizing — likely a duplicate), local-model-as-source display (checked for
overlap with REQ-F-030), leverage-ladder folding order.

**REQ-F-035** splits into: one requirement per independent read (futures
leadership, futures push, volatility direction, 10-year yield, curve shape,
heavyweight participation, breadth-by-VWAP, volume-delta, opening-range
position — nine), one for the majority-agreement gate, and two DISPLAY-only
requirements (constituent own-view panel, cap-weighted lead indicator) —
those two are arguably REQ-F-038 (cockpit)/REQ-UI territory rather than
REQ-F-035 at all, to be confirmed during the split.

## Acceptance for closing the tracked issue

Both `STATUS` fields move to `superseded`, naming their successor ids;
`tools/trace_report.py` shows 0 new hard gaps; every re-tagged test's
`docs/test_spec.md` sentence is re-verified against its actual assertion
(not just its old sentence carried over) per
`templates/review-checklist-test.md` item 3.
