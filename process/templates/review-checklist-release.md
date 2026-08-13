# Review Checklist: Release

Read `templates/ai-reviewer-instructions.md` first. Scope: one release
candidate commit, before tagging.

1. `tools/publish_release.sh --dry-run` reports green.
2. `test-results/*.xml` shows 0 failures and is newer than every tracked
   source file (`git ls-files 'src/*' 'tests/*' 'CMakeLists.txt'
   'tests/CMakeLists.txt'`).
3. Every static analyzer totals 0 (or documented, justified suppressions
   only).
4. `tools/lizard_metrics.py`'s ratchet is clean.
5. `tools/trace_report.py` reports 0 hard gaps.
6. `downloads/TradingApp-quality-report.pdf` is newer than the test results.
7. `downloads/TradingApp-qa-report.md` shows no missing/stale test-evidence
   hard fail (the ONE QA finding that blocks release per
   `process-model.md`).
8. Every open Critical/High risk in the risk register tied to this release
   is either closed or has an explicit Project Management accept decision.
9. `CHANGELOG.md` (and `process/CHANGELOG.md` if this release includes a
   process change) is updated.
10. The tag has not already been used (`git tag --list`).
