# Work Product: Detailed Design

**Produced by:** SWE.3. **Owning role:** Software Designer/Developer.
**Location:** `docs/design.md` (one table row = one DES-id = one unit, per
`processes/SWE.2`'s component/unit split).

## Content rules

Each DES-id entry states the six items `processes/SWE.3-detailed-design-and-
unit-construction.md`'s "Required design information per unit" section
lists: responsibility, unit interface, data structures, behavior/algorithm
notes (non-obvious parts only), `satisfies`, implementation file(s). Never
duplicates architecture-level content (that lives in `architecture-design.md`)
or restates what the interface header already says verbatim.

## Quality criteria

One unit per entry; an entry describing more than one unit's worth of
responsibility is split. No entry references code that no longer exists
(checked by `tools/check_process_docs.py`'s file-existence pass, extended to
sample `docs/design.md`'s `Implementation` column — see that tool's own
spec).

## Review requirement

`templates/review-checklist-design.md`'s unit-level checklist
(`processes/SWE.3`'s three numbered criteria).

## KPIs

Ratio of DES-ids passing all three unit-review criteria on first review
(tracked informally via PR review comments; a formal metric is a `PIM.3`
candidate once enough cycles exist to baseline it).

## Traceability

`docs/design.md`'s `satisfies` column ↔ `requirements/requirements.sdoc`'s
`UID` ↔ `tests/tst_*.cpp`'s `@design`/`@relation` tags, joined by
`tools/trace_report.py`.
