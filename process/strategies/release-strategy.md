# Release Strategy

Referenced by `processes/REL-product-release.md`.

## Cadence

Release-when-ready, not calendar-driven: a release is cut when
`MAN.3`'s Project Manager judges the accumulated changes worth publishing AND
the gate in `REL-product-release.md` is green. No fixed sprint-end release
obligation exists in this project.

## Gate (restated from REL-product-release.md for completeness)

`tools/publish_release.sh --dry-run` green, AND
`downloads/TradingApp-qa-report.md` showing no missing/stale test-evidence
hard fail. Both must be satisfied from the SAME commit that will be tagged —
a gate passed on an earlier commit does not carry forward.

## Rollback

A released baseline is never un-published; a defect found post-release is
handled by `SUP.9` (problem) and, if it needs a code change, `SUP.10`
(change) followed by a NEW release under a NEW tag. `SUP.8`'s "never mutate
a baseline" rule applies to releases without exception.

## Communication

Release notes are generated from `CHANGELOG.md` plus the GitHub Release's own
auto-generated commit list; the qualification bundle
(`work-products/quality-report.md`) is attached for anyone needing the
underlying evidence rather than just the summary.
