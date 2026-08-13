# Work Product: Unit/Integration Test Specification

**Produced by:** SWE.4, SWE.5. **Owning role:** Verification/Integration
Engineer. **Location:** `tests/tst_*.cpp`, indexed in `docs/test_spec.md`.

## Content rules

Every test function carries `//! @tstid TS-… @design DES-…` plus the plain
`// @relation(REQ-…, scope=function)` StrictDoc marker (both required —
`CLAUDE.md`'s own non-negotiable). `docs/test_spec.md` states, in prose, what
each `TS-xxx` id actually pins down — not merely restates the function name.

## Quality criteria

A test proves the specific thing its `docs/test_spec.md` sentence claims —
QA/review samples this by reading the assertion against the sentence, not
just confirming the test passes (a passing test that asserts something other
than its stated claim is a defect in this work product).

## Review requirement

`templates/review-checklist-test.md`.

## KPIs

`tools/trace_report.py`'s "implemented vs specified" count matching exactly
(a test implemented but never added to `docs/test_spec.md`, or vice versa, is
a hard gap the tool already catches).

## Traceability

`TS-xxx` ↔ `DES-xxx` ↔ `REQ-xxx`, joined by `tools/trace_report.py`.
