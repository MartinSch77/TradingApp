# SUP.10 — Change Request Management

## Purpose

Control changes to **baselined** work products (a tagged release, a released
process version) so a change is deliberate, reviewed, and traceable — never a
silent edit to something already published.

## Inputs

- A proposed change to a baselined item: an application hotfix against a
  released tag, or a change to this process framework itself (which is
  released and versioned independently — `process/README.md`).

## Outputs / Work Products

- A pull request is the change-request instance for code; for a process
  change, the PR additionally updates `process/CHANGELOG.md` with the
  rationale. `work-products/change-request.md` states the required content
  (what changed, why, impact assessed, approver).

## Tasks

1. **Submit.** A change request states what baseline it targets and why the
   change cannot wait for the next unbaselined cycle of normal work.
2. **Assess impact.** For a code change: which requirements/design/tests are
   touched (`tools/trace_report.py` re-run confirms nothing silently
   orphaned). For a process change: which processes/work-products cite the
   changed file (`tools/check_process_docs.py`'s traceability check).
3. **Approve/reject.** Change Control Board decision, recorded in the PR.
4. **Implement and re-baseline.** Per `SUP.8`, a new baseline (tag) is cut
   once the change lands — never a mutation of the old tag.

## Roles

Change Control Board (Accountable) — see `roles.md`; for a small team this
may be the Project Manager and Software/Process Architect jointly.

## Base Practices (ASPICE 4.0 reference)

SUP.10.BP1 (identify) → Task 1. SUP.10.BP2 (evaluate impact) → Task 2.
SUP.10.BP3 (approve/track) → Task 3. SUP.10.BP4 (implement) → Task 4.

## Verification / QA Hooks

QA confirms every change to an already-tagged baseline went through a
reviewed PR with an explicit impact assessment recorded, rather than a direct
push.
