# Review Checklist: Code

Read `templates/ai-reviewer-instructions.md` first. Scope: one PR's diff.

1. `tools/static_analysis.sh` reports 0 NEW findings for the changed files
   (pre-existing, already-suppressed findings elsewhere are out of scope).
2. Any NEW suppression (`.clang-tidy`, `tools/cppcheck-suppressions.txt`, an
   inline `// NOLINT`) carries a written reason and a measured hit count.
3. Every boolean decision the diff touches stays within the ≤ 6-condition
   MC/DC budget.
4. Comments explain WHY, never WHAT — a comment restating what the next line
   obviously does is a finding here, not a style nit to wave through.
5. No feature/abstraction added beyond what the linked requirement/change
   request actually needs (`CLAUDE.md`'s "don't design for hypothetical
   future requirements").
6. Layering respected: a `domain/` file adds no Qt-module-beyond-Core
   `#include` (cross-check `tst_architecture.cpp` passes).
7. `docs/design.md` updated in the SAME change if the diff changes a unit's
   documented behavior, interface, or data structures.
8. New/changed tests exist per `review-checklist-test.md` and
   `tools/trace_report.py` shows 0 new hard gaps.
9. If the diff touches money-relevant logic (sizing, risk gates, cost model,
   order validation): a boundary-condition test was added
   (`strategies/verification-strategy.md`'s acceptance criterion 5), and — if
   the file is in the Mull mutation pilot — the kill rate is unchanged or a
   new survivor is documented as a genuine equivalent mutant.
10. `tools/lizard_metrics.py`'s ratchet is unchanged or improved.
