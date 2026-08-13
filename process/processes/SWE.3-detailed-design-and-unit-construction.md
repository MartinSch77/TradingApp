# SWE.3 — Detailed Design and Unit Construction

## Purpose

Design each software unit consistent with the architecture, and construct it
(`src/`) consistent with that design.

## Inputs

- `docs/architecture.md`, the requirement(s) a unit implements.

## Outputs / Work Products

- `docs/design.md` DES-xxx entries (one per designed element, naming its
  implementing file(s) and the requirements it `satisfies`), the
  implementation in `src/`. See `work-products/detailed-design.md` and
  `work-products/source-code.md`.

## Required design information per unit

Per `SWE.2`'s component/unit split, a `docs/design.md` DES-id entry is
complete only when it states, for that ONE unit:

- **Responsibility** — the single sentence a reader needs before the detail
  (what problem this unit solves, e.g. "Position/money arithmetic").
- **Unit interface** — the public functions/methods and their signatures, or
  a reference to the header being the interface verbatim (this project's
  usual choice: `PositionMath.h` IS the interface spec, so the design entry
  names it rather than re-transcribing it — Section 5's "state once" rule
  from `process-model.md`).
- **Data structures** — the structs/classes the unit introduces or consumes
  (e.g. `SwingPositionState`, `StrategyDecision`) and any invariant that is
  not obvious from the type alone (a stop price that only ever tightens,
  never loosens).
- **Behavior/algorithm notes** — ONLY what is non-obvious from reading the
  code (a boundary condition, a documented equivalent mutant, a deliberate
  simplification) — this project's own "don't explain what, explain why"
  comment discipline (`CLAUDE.md`) applied to design entries too.
- **`satisfies`** — the requirement id(s) this unit exists for.
- **Implementation** — the file(s).

A design review (below) checks an entry against exactly this list; an entry
missing one of the first four items is INCOMPLETE, not merely terse.

## Unit design review criteria

`templates/review-checklist-design.md`'s unit-level section is the
executable form of this list. A unit passes design review only if:

1. Every one of the six items above is present or explicitly N/A with a
   reason (e.g. "no non-obvious behavior" is a valid, stated answer — an
   OMITTED section is not).
2. The unit's interface does not leak an implementation detail across the
   component boundary it sits in (e.g. a `domain/` unit's public interface
   never exposes a Qt module beyond Core — `tst_architecture.cpp` is the
   mechanical backstop for this, the review is the human/AI-legible check
   for the same rule).
3. The unit's own McCabe complexity is within the lizard ratchet
   (`tools/lizard_metrics.py`) or the over-threshold entry is a deliberate,
   dated baseline record — never a silent excess.

## Tasks

1. Design each unit and record it as a `docs/design.md` entry BEFORE (or in
   the same change set as) writing the code — `tools/trace_report.py` fails
   the build if a requirement has no design element claiming to satisfy it
   (a "hard gap").
2. Construct the unit per this project's coding rules (`CLAUDE.md`'s
   non-negotiables: `.clang-tidy`, boolean-decision ≤ 6 conditions for MC/DC,
   `Q_OBJECT;` StrictDoc anchor convention, etc.).
3. Review the design and code (checklist: `templates/review-checklist-design.md`,
   `templates/review-checklist-code.md`) before it is handed to SWE.4.
4. Keep `docs/design.md` and the code in sync on every change — a design
   entry describing behavior the code no longer has is a defect in this work
   product, not a harmless staleness.

## Roles

Software Designer/Developer (Responsible/Accountable), Software Architect
(Consulted for anything crossing a layer boundary) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

SWE.3.BP1–BP2 (develop detailed design, define interfaces) → Task 1.
SWE.3.BP3 (define/implement unit) → Task 2. SWE.3.BP5 (bidirectional
traceability design ↔ requirements) → `docs/design.md`'s `satisfies` column.

## Verification / QA Hooks

QA confirms `tools/trace_report.py` reports 0 hard gaps and that the lizard
metrics ratchet (`tools/lizard_metrics.py`) shows no NEW over-threshold
function introduced without a documented, deliberate baseline entry
(`CLAUDE.md`'s "ratchet, not a threshold" rule) — a ratchet violation is a
process-conformance finding here, distinct from SWE.4's correctness finding
on the same code.
