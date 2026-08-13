# Problem Resolution Strategy

Referenced by `processes/SUP.9-problem-resolution-management.md`.

## Severity scale

| Severity | Definition | Response target |
|---|---|---|
| Critical | Money-relevant logic wrong, or a safety property (REQ-N-005 double-press, REQ-F-029's "no order path") violated | Fix before any further release; may block `MAN.3` schedule |
| High | Requirement violated outside the critical set, or a hard gap in traceability | Fix before next release |
| Medium | A non-blocking defect (a cosmetic UI issue, an analyzer suppression that turns out to be too broad) | Scheduled, tracked |
| Low | Documentation/comment inaccuracy, a style-only finding | Scheduled at convenience |

## Intake and tool

GitHub Issues is the instance store; `.github/ISSUE_TEMPLATE/problem-report.yml`
(see `processes/SUP.9`'s work product) is the required intake shape — a
problem report missing severity or reproduction steps is returned to the
reporter, not triaged as-is.

## Labels and states

`proposed → analyzed → approved → implemented → verified → closed` (shared
scheme across `SUP.9` and `SUP.10`, per the project owner's explicit
instruction) — realized as GitHub labels `state:proposed`, `state:analyzed`,
etc., plus `severity:critical|high|medium|low`. A problem report is never
closed without reaching `state:verified` first — closing from
`state:implemented` directly skips the evidence-of-fix step and is itself a
QA-detectable process deviation.

## Cross-filing to risk

Any Critical or High severity problem found in an already-released baseline
is cross-filed into `MAN.5`'s risk register the same day, independent of
whether a fix is already in progress — the risk register tracks EXPOSURE
while it exists, not just the fix's progress.
