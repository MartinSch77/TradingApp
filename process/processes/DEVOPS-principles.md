# DevOps Principles (cross-cutting)

Not an ASPICE base process; a set of practices this project already follows
that every process above relies on, made explicit so QA can check for their
presence rather than assume them.

## Purpose

Keep every process's evidence continuously reproducible, versioned, and fast
to regenerate — the technical precondition that makes SUP.1's "confirm
evidence exists and is current" meaningful rather than a once-a-quarter
paper chase.

## Principles applied in this repository

1. **Everything as code.** Requirements (`requirements/requirements.sdoc`),
   design (`docs/design.md`), infrastructure (`setup.sh`/`.ps1`), pipeline
   (`build_all.sh`, `.github/workflows/*.yml`), and now process
   (`process/*.md`) are all version-controlled text — none is a slide deck or
   a wiki page that can drift from what actually runs.
2. **Continuous Integration.** `.github/workflows/ci.yml` runs build+test on
   every push across Linux x86-64/ARM64, Windows, and macOS — a regression is
   caught before it reaches `main`, not at release time.
3. **Continuous Delivery of evidence, not just code.** `build_all.sh`
   regenerates the FULL evidence set (tests, traceability, analysis,
   coverage, sanitizers, the Axivion dashboard, the quality PDF) from a clean
   tree on demand — evidence is a build artefact, not a hand-maintained
   document that can quietly go stale.
4. **Infrastructure/environment as code.** `setup.sh`/`setup.ps1` provision a
   naked machine to the exact toolchain this project needs — a new
   contributor or a CI runner reaches the same state deterministically.
5. **Fast feedback over gatekeeping.** Findings are fixed or explicitly,
   justifiedly suppressed at the point they are found (`CLAUDE.md`'s
   analyzer-config discipline) rather than batched into a separate
   "hardening" phase.
6. **Reproducible builds where feasible.** `tools/check_reproducibility.sh`
   compares two independent AppImage builds byte-for-byte (informational,
   Linux-only) — a DevOps-adjacent supply-chain practice, not an ASPICE base
   practice, included because CRA-style expectations increasingly ask for it.

## Roles

DevOps Engineer (Responsible for pipeline/environment definitions), Software
Architect (Accountable for the practices actually being followed) — see
`roles.md`.

## Verification / QA Hooks

QA confirms the CI workflows actually run on every push (not disabled), that
`setup.sh`/`.ps1` stay in lockstep with each other (`CLAUDE.md`'s own stated
rule), and samples one `build_all.sh` run per cycle to confirm it reproduces
the evidence set from a clean tree without manual intervention.
