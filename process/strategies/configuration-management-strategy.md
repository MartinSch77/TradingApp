# Configuration Management Strategy

Referenced by `processes/SUP.8-configuration-management.md`.

## Identification scheme

- Requirements: `REQ-F-xxx` / `REQ-N-xxx`, never reused or renumbered once
  published (superseding a requirement sets its `STATUS` field to
  `superseded` and names the successor — see
  `work-products/requirements-specification.md` — rather than deleting it).
- Design: `DES-xxx`. Tests: `TS-xxx`.
- Releases: application `vX.Y.Z` (semantic versioning, `CMakeLists.txt`'s
  `VERSION`), this process framework `process-vX.Y.Z` (`process/VERSION`) —
  two independent baselines, never conflated.
- Commits: the git SHA is the atomic configuration item identity; a PR
  merge commit is the unit change control operates on (`SUP.10`).

## Baseline points

A baseline is cut at: every merge to `main` (an implicit, continuously
advancing baseline — DevOps principle #2), and every release tag (an
EXPLICIT, named baseline attached with binaries and the qualification
bundle). Only the latter is referenced from outside the repository (a GitHub
Release, a customer-facing artefact).

## Change control

No baselined (tagged) work product is modified in place. A needed correction
against a released baseline goes through `SUP.10`, lands as a new commit, and
is released under a NEW tag — this project's `tools/publish_release.sh`
"never amend, always a new tag" discipline extended explicitly to process
scope.

## Status accounting

`git log --oneline`, `git tag --list`, and `process/traceability-matrix.md`
together answer "what is the current baseline of work product X" without a
separate CM database — consistent with `process-model.md` §5's "reusable,
low complexity" quality bar applied to the CM mechanism itself.
