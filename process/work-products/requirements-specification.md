# Work Product: Requirements Specification

**Produced by:** SWE.1. **Owning role:** Requirements Engineer.
**Location:** `requirements/requirements.sdoc` (generates `docs/requirements.md`
via `tools/make_requirements.sh` — never hand-edited).

## Content rules

Every requirement (StrictDoc `[REQUIREMENT]` element) states, per the
atomized schema (see `process/requirements-schema-upgrade.md` for the
migration record):

- `UID` — stable, never reused (`REQ-F-xxx` functional / `REQ-N-xxx`
  non-functional).
- `TITLE`, `STATEMENT` — ONE principal obligation each (an "and" joining two
  independently verifiable obligations is a signal to split).
- `SOURCE` — the stakeholder or originating decision (a person, an incident,
  a measured finding — e.g. "measured: 6 closes, median hold 5.2 min...").
- `RATIONALE` — why this requirement exists, distinct from what it demands.
- `PRIORITY` — must / should / could (MoSCoW), set by Project Management.
- `STATUS` — `proposed | analyzed | approved | implemented | verified |
  superseded`.
- `VERIFICATION` — `T` (test) / `A` (analysis) / `I` (inspection), may combine.
- `ACCEPTANCE_CRITERIA` — the observable condition that makes this
  requirement's `STATUS` eligible to become `verified`.
- `ISSUE` — the GitHub Issue/PR that introduced or last materially changed it
  (change history without a duplicate log).

## Quality criteria

Atomic (one principal obligation), independently verifiable, no forward
reference to a design decision (a requirement states WHAT, never HOW).

## Review requirement

`templates/review-checklist-requirements.md`, before `STATUS` may move past
`analyzed`.

## KPIs

% of requirements with `STATUS: verified` at release time; count of
multi-obligation requirements remaining (target: 0 — tracked as a
`PIM.3` improvement item until reached).

## Traceability

`tools/trace_report.py` joins `UID` to `docs/design.md`'s `satisfies` column
and to `tests/tst_*.cpp`'s `@relation(REQ-…, scope=function)` tags.
