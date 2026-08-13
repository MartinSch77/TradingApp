# Work Product: Architecture Design

**Produced by:** SWE.2. **Owning role:** Software Architect.
**Location:** `docs/architecture.md`.

## Content rules

Per `processes/SWE.2-architectural-design.md`'s component/unit split: this
document describes **components** only (`trading_domain`, `trading_services`,
`ui`, `tools/ml/`), their responsibilities, their PUBLIC interfaces to the
layer above, and runtime flow between them (PlantUML sequence/activity
diagrams). It never contains a class-level design decision — that belongs in
`work-products/detailed-design.md`.

## Quality criteria

Every component's dependency list matches its actual `CMakeLists.txt`
`target_link_libraries` — the document and the build must never disagree
about what depends on what.

## Review requirement

`templates/review-checklist-design.md`'s architecture section, before a
component boundary change is allowed to constrain SWE.3 work.

## KPIs

Zero drift incidents (a `tst_architecture.cpp` failure caused by an
undocumented layering change) per release cycle.

## Traceability

Non-functional requirements (REQ-N-xxx) → architecture decisions here → the
linker/`tst_architecture.cpp` enforcement.
