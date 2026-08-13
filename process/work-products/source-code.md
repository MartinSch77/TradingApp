# Work Product: Source Code

**Produced by:** SWE.3. **Owning role:** Software Designer/Developer.
**Location:** `src/`.

## Content rules

Conforms to `.clang-tidy`, the MC/DC-driven "≤ 6 conditions per boolean
decision" rule, the `Q_OBJECT;` StrictDoc-anchor convention, and the layering
rule (`domain` ← `services` ← `ui`) enforced by both the linker and
`tests/tst_architecture.cpp`. Every non-obvious line carries a WHY comment,
never a WHAT comment (`CLAUDE.md`'s comment discipline).

## Quality criteria

Zero findings across the seven-analyzer static-analysis stage (or every
suppression is written, justified, and hit-count-measured per `CLAUDE.md`'s
non-negotiable); lizard ratchet unchanged or improved; zero PMD CPD clones
≥ 100 tokens.

## Review requirement

`templates/review-checklist-code.md`.

## KPIs

Static-analysis finding count (target 0 at merge time), ratchet trend
(over-threshold function count, target non-increasing).

## Traceability

Each file/function ↔ its `docs/design.md` DES-id ↔ requirement.
