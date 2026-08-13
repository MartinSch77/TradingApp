# Review Checklist: Design

Read `templates/ai-reviewer-instructions.md` first.

## Architecture section (scope: `docs/architecture.md`, a component-boundary
change)

1. The component's dependency list in the document matches
   `CMakeLists.txt`'s actual `target_link_libraries`.
2. A new/changed component interface is stated as the PUBLIC header(s) it
   exposes, not an internal implementation detail.
3. `tests/tst_architecture.cpp` (or the linker itself) mechanically enforces
   any new boundary claimed here.
4. Runtime-flow diagrams (if changed) still match the code path they claim
   to describe.

## Unit-level section (scope: one `docs/design.md` DES-id)

1. Responsibility stated in one sentence.
2. Unit interface stated or the header named as the interface verbatim.
3. Data structures the unit introduces/consumes are named, with any
   non-obvious invariant stated.
4. Behavior/algorithm notes cover ONLY non-obvious parts (a boundary
   condition, a documented equivalent mutant, a deliberate simplification) —
   not a restatement of the code.
5. `satisfies` names the requirement id(s).
6. Implementation file(s) named and current.
7. The unit's interface does not leak a forbidden dependency across its
   component boundary (cross-check against the architecture section above).
8. Lizard ratchet: this unit's complexity is within budget or the
   over-threshold entry is a dated, deliberate baseline record.

## ML section (scope: a design entry for a model-producing unit — extends
the unit-level list above)

1. Model category (A/B/C, `processes/MLE.1-4-machine-learning-engineering.md`) stated.
2. For Category A: dataset provenance, split method (time-ordered), and
   baseline comparison are named.
3. The design confirms the model NEVER solely decides a trade/order (the
   never-solely-decides safeguard is visible in the design, not merely
   assumed).
