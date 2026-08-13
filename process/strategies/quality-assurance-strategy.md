# Quality Assurance Strategy

Referenced by `processes/SUP.1-quality-assurance.md`.

## Audit cadence

- **Every release** (`REL-product-release.md`'s gate): full QA report run,
  its missing-test-evidence hard-fail rule enforced.
- **Every calendar month** (or every N merged PRs, whichever comes first,
  recorded in `process/CHANGELOG.md`'s cadence note once a real cadence is
  observed): a full conformance sweep across every process in
  `process-model.md`'s landscape, not just the ones touched that month —
  a process that was quiet is still auditable.
- **On demand**: any role may request an ad hoc QA pass; QA does not need
  permission to look.

## Sampling rule for high-volume work products

For `SUP.9` problem closures (potentially many per cycle): QA samples at
least 20% of closures each cycle, rounded up, minimum 1. A sampled closure
found NOT to have valid evidence escalates to a FULL review of that cycle's
closures, not just a note on the one instance — one bad sample is itself
evidence the population may be unreliable.

## Reporting

`tools/qa_report.py` output is the single canonical report
(`downloads/TradingApp-qa-report.md`) — see `work-products/qa-report.md` for
its required structure. No parallel QA report format exists; a finding not
representable by the tool is filed as a `PIM.3` improvement to the tool
itself.

## Independence enforcement

Per `process-model.md` §3: the person/session running a given QA cycle must
not have been Responsible or Accountable for the process step under audit
that cycle. For an AI-executed QA pass, this means a fresh session with only
`process/`, the repository's observable state, and NO transcript of the
engineering conversation that produced the change being audited.

## What QA never does

- Fix the finding itself.
- Judge product correctness beyond "does the process's own defined
  verification evidence exist and is it current" (that judgement belongs to
  the verification processes, `SWE.4`–`SWE.6`).
- Block a release for any reason other than the one stated exception
  (`process-model.md`'s missing/stale test evidence rule).
