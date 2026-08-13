# MAN.3 — Project Management

## Purpose

Plan, track, and steer the project, and — the authority this framework
depends on — **select and tailor the process itself** for the project's
actual context (`process-model.md` §8). QA confirms the selected process is
followed; it does not select it.

## Inputs

- `requirements/requirements.sdoc` (scope), `docs/roadmap.md` (forward plan),
  the risk register (`MAN.5`), `downloads/TradingApp-qa-report.md` (process
  health).

## Outputs / Work Products

- `work-products/project-plan.md`-conformant plan (this repository expresses
  it as `docs/roadmap.md` plus the milestone/version history in
  `CHANGELOG.md` and git tags — see that work product spec for why a
  separate planning document was not duplicated).
- Tailoring decisions: which processes in `process-model.md`'s landscape
  apply at full rigor vs. a stated reduced form, recorded in
  `strategies/project-management-strategy.md`'s "Tailoring log."

## Tasks

1. Maintain the roadmap/plan and keep it consistent with
   `requirements/requirements.sdoc`'s actual scope.
2. Select and tailor the process set from `process-model.md`'s landscape;
   record and justify any deviation from the untailored ASPICE base practice
   set in `strategies/project-management-strategy.md`.
3. Triage risks escalated from `MAN.5` (including QA-sourced process-deviation
   risks) and assign an owner/response.
4. Review `downloads/TradingApp-qa-report.md` each cycle and act on
   NOT FOUND/PARTIAL findings that bear on schedule or scope.
5. Authorize a release to proceed to `REL-product-release.md`'s gate.

## Roles

Project Manager (Responsible/Accountable) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

MAN.3.BP1–BP2 (define scope, estimate) → roadmap maintenance (Task 1).
MAN.3.BP3 (define lifecycle) → process tailoring (Task 2).
MAN.3.BP6 (schedule/track) → git tag / `CHANGELOG.md` history as the tracked
record. MAN.3.BP8 (risk) → interface to MAN.5 (Task 3).

## Verification / QA Hooks

QA (`SUP.1`) confirms a tailoring decision exists and is justified whenever a
process in the landscape is run at reduced rigor; QA does not judge whether
the tailoring choice was *wise* — that is Project Management's and Process
Improvement's call (`process-model.md` §6).
