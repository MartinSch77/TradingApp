# SWE.2 — Architectural Design

## Purpose

Define and maintain the software architecture: the layering, module
boundaries, and cross-cutting technical decisions (e.g. why Qt Charts and Qt
Graphs cannot share a process, `CLAUDE.md`'s own documented finding) that
every lower-level design must respect.

## Inputs

- `requirements/requirements.sdoc`, non-functional requirements in particular
  (REQ-N-xxx), existing `docs/architecture.md`.

## Outputs / Work Products

- `docs/architecture.md` (PlantUML + prose), the domain/services/ui layering
  rule enforced by the linker AND by `tests/tst_architecture.cpp`
  (REQ-N-002) — see `work-products/architecture-design.md`. This is a
  SEPARATE baseline from SWE.3's detailed design (Section "Component vs.
  unit" below) — the two are reviewed and versioned independently, because an
  architecture change (a new layer, a new module boundary) is a materially
  different-weight decision than a unit's internal design changing.

## Component vs. unit — where SWE.2 stops and SWE.3 starts

A **software component** is an architectural-level grouping with its own
build target and a stated set of responsibilities and dependencies —
concretely, in this repository: `trading_domain`, `trading_services`, the
`ui` sources compiled into `TradingApp`/`TradingCockpit`/`TradingBot`, and
`tools/ml/` (the offline training pipeline, a component with no runtime
dependency on the rest — `CLAUDE.md`'s own explicit rule). SWE.2 owns:

- **Component identity and responsibility** — what `trading_domain` may and
  may not depend on (`target_link_libraries(trading_domain PUBLIC Qt6::Core)`
  is the enforceable expression of that responsibility).
- **Component interfaces** — the public headers a component exposes to the
  layer above it (e.g. `domain/PaperTrader.h`'s public API is
  `trading_services`'/`ui`'s contract with the domain layer; nothing in
  `PaperTrader.cpp`'s anonymous namespace is part of that contract).
- **Runtime flow between components** — sequence/activity-level PlantUML
  diagrams in `docs/architecture.md` showing how a scan cycle moves through
  `services` into `domain` and back into `ui`.

A **software unit** is a class, a tightly-coupled small group of classes
(e.g. `SwingPullbackStrategyV1` + its `SwingPullbackConfig`/
`SwingPositionState` structs), or a pure functional module (e.g.
`PositionMath.h/.cpp`'s free functions) — the level SWE.3 owns. The
distinguishing test: **a unit is what one `docs/design.md` DES-id names**;
a component is what one `CMakeLists.txt` `add_library`/`qt_add_executable`
target names. `work-products/architecture-design.md` documents components;
`work-products/detailed-design.md` documents units — one entry never
appears in both.

## Tasks

1. Derive the architecture from non-functional requirements (REQ-N-002's
   "domain is Qt Core only," REQ-N-005's confirm-gate placement, etc.).
2. Record every architecture-significant decision with its rationale — this
   project's convention is to record the rationale IN the design/CLAUDE.md
   text itself rather than a separate ADR log, so the reasoning travels with
   the code it constrains.
3. Review architecture changes (checklist:
   `templates/review-checklist-design.md`, architecture section) before they
   are allowed to constrain SWE.3 work.
4. Confirm the layering is enforced mechanically, not just by convention:
   `tst_architecture.cpp`'s source scan PLUS the linker's own
   `target_link_libraries` boundary (`CLAUDE.md`'s "Layering is
   linker-enforced" rule) — two independent enforcement mechanisms is
   deliberate redundancy, not duplication.

## Roles

Software Architect (Responsible/Accountable) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

SWE.2.BP1–BP2 (develop architecture, allocate requirements) → Task 1.
SWE.2.BP4 (define interfaces) → the layer boundary rule. SWE.2.BP6
(consistency/traceability) → `docs/design.md`'s `satisfies` links back to
architecture-driven non-functional requirements.

## Verification / QA Hooks

QA confirms `tst_architecture.cpp` exists, passes, and that
`docs/architecture.md` was updated in the same change set as any commit that
altered a layer boundary (a design decision changing without its record
updating is a process deviation, not merely a documentation lag).
