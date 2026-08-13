# REL — Product Release

Not a numbered ASPICE base process on its own; this project treats release as
a first-class process because it is where every other process's evidence
converges into a single gate. It draws on MAN.3 (authorization), SUP.8
(baseline), and SWE.6 (qualification evidence).

## Purpose

Gate and publish a versioned release only when the evidence chain is real and
current — never on a red or stale pipeline (`CLAUDE.md`'s own non-negotiable,
echoed here as a process rule rather than only a tool behavior).

## Inputs

- A green full pipeline (`build_all.sh` — build, test, trace, analysis,
  coverage, sanitize, Axivion, report), `downloads/TradingApp-qa-report.md`
  showing no missing-test-evidence hard fail (`process-model.md`'s stated
  exception to "QA is informational").
- Cadence and rollback rules: `strategies/release-strategy.md`.
- Review gate: `templates/review-checklist-release.md`.

## Outputs / Work Products

- A git tag (`vX.Y.Z`), the GitHub Release with attached binaries, the
  qualification bundle (`work-products/quality-report.md`).

## Tasks

1. Confirm the evidence per `tools/publish_release.sh --dry-run`: tests
   green and newer than sources, every analyzer at zero, the ratchet clean,
   0 hard gaps, the quality PDF newer than the test results.
2. Confirm QA's own report carries no hard-fail exception (missing/stale test
   evidence) — this is the ONE place a QA finding blocks progress rather than
   merely informing (`process-model.md` §"Quality Assurance's report").
3. Tag, push, let `.github/workflows/release.yml` rebuild all four platforms,
   then run `tools/publish_release.sh` to attach docs and the qualification
   bundle.
4. Record the release as a new baseline in `SUP.8`'s terms and close out any
   `MAN.5` risks the release was gated on.

## Roles

Release Engineer (Responsible), Project Manager (Accountable — authorizes the
release per `MAN.3`) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

Draws on MAN.3.BP9 (authorize), SUP.8.BP2-3 (baseline), SWE.6's qualification
evidence requirement.

## Verification / QA Hooks

QA's release-time check IS the hard-fail exception itself: `tools/qa_report.py`
run immediately before `tools/publish_release.sh` and its exit code is part
of the release gate, not merely advisory, specifically for missing/stale test
evidence — every other QA finding stays informational per `process-model.md`.
