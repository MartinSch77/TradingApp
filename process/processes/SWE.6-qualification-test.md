# SWE.6 — Qualification Test

## Purpose

Confirm the integrated system meets its requirements from the outside — the
system-level acceptance evidence a stakeholder actually cares about, distinct
from unit/integration test which are development-internal.

## Inputs

- The integrated application (`SWE.5`'s output), `requirements/requirements.sdoc`.

## Outputs / Work Products

- `docs/traceability.html` (requirement-level view), the Squish GUI suite
  (`squish/`) plus its Coco coverage figure, the Test Center upload
  (`tools/testcenter_upload.sh`), `downloads/TradingApp-quality-report.pdf`.
  See `work-products/traceability-matrix.md`, `work-products/quality-report.md`.

## Tasks

1. Confirm every requirement is traced to an EXECUTED result (not merely a
   written test) — `tools/trace_report.py`'s "0 hard gaps" bar.
2. Run the licensed GUI/coverage/Test-Center chain
   (`tools/make_test_report.sh`) where the licences are present; where they
   are not, the quality report states them MISSING rather than silently
   omitting the row (`CLAUDE.md`'s licence-bound-stage discipline).
3. Produce the final quality report as the qualification evidence package.

## Roles

Qualification/System Test Engineer (Responsible/Accountable) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

SWE.6.BP1 (specify qualification test strategy) → realized by
`strategies/verification-strategy.md` §Acceptance. SWE.6.BP2–BP3 (test,
record) → Tasks 1–2. SWE.6.BP5 (traceability) → `docs/traceability.html`.

## Verification / QA Hooks

QA confirms `docs/traceability.html` reports 0 hard gaps at release time and
that the quality report's licence-bound rows accurately reflect whether the
licence was actually present on the machine that produced the report (a
false "missing licence" row when the licence WAS available is exactly the
defect `CLAUDE.md` records having found and fixed once already —
`tools/make_report.py`'s clone-detection row).
