# SWE.1 — Requirements Elicitation

## Purpose

Capture and maintain the system/software requirements as the single source
of truth every downstream process (design, code, test) traces to.

## Inputs

- Stakeholder/feature requests, prior requirements, `docs/roadmap.md`'s
  forward plan.

## Outputs / Work Products

- `requirements/requirements.sdoc` (StrictDoc) — see
  `work-products/requirements-specification.md` for content rules
  (REQ-F-xxx / REQ-N-xxx numbering, mandatory `VERIFICATION` field: T = test,
  A = analysis, I = inspection). `docs/requirements.md` is GENERATED from it
  (`tools/make_requirements.sh`) and is never hand-edited.

## Tasks

1. Elicit and state each requirement so it is independently verifiable
   (the `VERIFICATION` field is not decorative — `tools/trace_report.py`
   reads it to decide whether a missing automated test is a real gap).
2. Assign a stable REQ ID; IDs are never reused or renumbered once
   published (a CM rule, `SUP.8`).
3. Review each new/changed requirement (checklist:
   `templates/review-checklist-requirements.md`) before it is considered
   baselined for downstream work to start against.
4. Re-run `tools/trace_report.py` after every change; 0 hard gaps is the
   acceptance bar for this process's own output (not for the whole project —
   see `strategies/verification-strategy.md`'s acceptance criteria).

## Roles

Requirements Engineer (Responsible/Accountable) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

SWE.1.BP1–BP3 (specify, structure, analyze) → Task 1. SWE.1.BP5 (establish
bidirectional traceability) → Task 4, realized by `tools/trace_report.py`.
SWE.1.BP7 (communicate) → the review in Task 3.

## Verification / QA Hooks

QA confirms every requirement added since the last audit carries a
`VERIFICATION` field and was reviewed per Task 3's checklist before any
commit references it from `docs/design.md`.
