---
name: Problem report
about: A defect or incident, tracked to closure per SUP.9
labels: type:problem, state:proposed
---

<!-- See process/processes/SUP.9-problem-resolution-management.md and
     process/strategies/problem-resolution-strategy.md. This is the process
     framework's own problem-tracking instance — the ordinary "Bug report"
     template still works for a quick report; this one exists for the full
     severity/closure-evidence lifecycle. -->

**Severity** <!-- Critical / High / Medium / Low — see problem-resolution-strategy.md's scale -->

**Description**

**Reproduction (or the automated evidence that detected it)**
1.
2.

**Root cause** <!-- filled in once analyzed -->

**Fix** <!-- commit/PR link -->

**Verifying evidence** <!-- REQUIRED before this can move to state:verified —
     a passing test, a re-run analysis at 0 for this finding -->

**State**
- [ ] proposed
- [ ] analyzed
- [ ] approved
- [ ] implemented
- [ ] verified <!-- never skip straight here from implemented -->
- [ ] closed
