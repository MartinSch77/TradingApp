# SUP.8 — Configuration Management

## Purpose

Establish and maintain the integrity of every controlled work product across
its life: unambiguous identity, controlled change, and a reproducible way to
recover any prior baseline.

## Inputs

- Every work product listed in `process/traceability-matrix.md`.

## Outputs / Work Products

- Git history itself is the configuration-managed record (this project keeps
  no parallel CM tool); `CHANGELOG.md` (application) and
  `process/CHANGELOG.md` (this framework) are the human-readable baseline
  index. `work-products/configuration-baseline.md` states the identity rules
  a baseline must satisfy.

## Tasks

1. **Identify.** Every controlled item has a stable, unique identifier: REQ/
   DES/TS IDs (`tools/trace_report.py`'s own scheme), git commit SHAs, and
   semantic version tags (`vX.Y.Z` for the application, `process-vX.Y.Z` for
   this framework — two independent baselines by design, `process/README.md`).
2. **Baseline.** A release tag (`REL-product-release.md`) is a baseline: every
   artefact `tools/publish_release.sh` attaches is traceable to the exact
   commit the tag points at, and `tools/gates_to_junit.py`/`publish_release.sh`
   both judge staleness against the SAME sources-only file list
   (`git ls-files 'src/*' 'tests/*' ...` — see `CLAUDE.md`'s own non-negotiable
   on this) so two tools cannot silently disagree about what "current" means.
3. **Control change.** No baselined work product changes outside
   `SUP.10-change-request-management.md`'s process — direct commits to a
   tagged release are not a thing this project does.
4. **Status accounting.** `git log`/`git tag --list` plus
   `process/traceability-matrix.md` answer "what is the current baseline of
   work product X" without needing a separate database.

## Roles

Configuration Manager (Accountable) — in practice the same person operating
`git`/`tools/publish_release.sh` under Release Engineer's hat; the roles are
named separately because the RESPONSIBILITIES differ (`roles.md`).

## Base Practices (ASPICE 4.0 reference)

SUP.8.BP1 (identify configuration items) → Task 1. SUP.8.BP2–BP3 (establish
CM system, baseline) → Task 2. SUP.8.BP4 (control changes) → Task 3.
SUP.8.BP5 (status accounting) → Task 4.

## Verification / QA Hooks

QA confirms every artefact attached to a release tag is reachable from that
exact commit (no artefact "named for another version" per `CLAUDE.md`'s own
`publish_release.sh` rule) and that no tagged baseline has been force-pushed
or amended after the fact.
