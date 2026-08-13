# Review Checklist: Test

Read `templates/ai-reviewer-instructions.md` first. Scope: one new/changed
test function.

1. Carries `//! @tstid TS-… @design DES-…` and the plain
   `// @relation(REQ-…, scope=function)` marker.
2. `docs/test_spec.md` has a matching row stating, in prose, what this test
   actually pins down.
3. The assertion(s) in the test body actually check the claim
   `docs/test_spec.md` states — not a weaker or unrelated claim.
4. Test is deterministic (no reliance on wall-clock time without an injected
   clock, no reliance on network unless it is an integration test against
   `MockHttpServer`).
5. If this test targets a boundary condition (exactly-zero, exactly-at-cap,
   sign flip): the boundary is tested on BOTH sides, not just the one that
   happens to pass.
6. `tools/run_tests.sh` shows the test passing, and `test-results/*.xml`
   reflects it (0 failures for the suite).
7. If added to close a hard gap: `tools/trace_report.py` re-run confirms the
   gap is gone and no new one was introduced.
