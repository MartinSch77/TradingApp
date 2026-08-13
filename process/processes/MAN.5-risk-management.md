# MAN.5 — Risk Management

## Purpose

Identify, analyze, treat, and monitor project risk — including risk
originating from process deviations found by QA (`process-model.md` §4),
which is filed here **before** anyone judges whether it produced an actual
defect this time.

## Inputs

- QA findings (`SUP.1` Task 5), incident reports (`SUP.9`), architectural or
  schedule concerns from any role.

## Outputs / Work Products

- `work-products/risk-register.md`-conformant register: one row per risk with
  ID, source, description, likelihood, impact, owner, status, treatment.

## Tasks

1. **Identify.** Any role may raise a risk; QA is obligated to (SUP.1 Task 5)
   for every process/work-product deviation it finds.
2. **Analyze and score.** Likelihood × impact, using the scale defined in
   `strategies/risk-management-strategy.md`. A QA-sourced risk is scored on
   the SAME scale as an engineering-sourced one — process risk is not
   automatically low-severity.
3. **Treat.** Project Management (MAN.3) assigns an owner and a
   mitigate/accept/transfer/avoid decision, recorded in the register.
4. **Monitor.** Re-score open risks each release cycle; a risk that has sat
   untreated past its review date is itself reported by QA as a process
   deviation of MAN.5 (self-referential by design — see `process-model.md`
   §5's "testable" requirement extended to risk hygiene).

## Roles

Project Manager (Accountable), risk owner per entry (Responsible) — see
`roles.md`.

## Base Practices (ASPICE 4.0 reference)

MAN.5.BP1–BP2 (establish strategy, identify risks) → Task 1.
MAN.5.BP3 (analyze) → Task 2. MAN.5.BP4–BP5 (define/implement treatment) →
Task 3. MAN.5.BP6 (monitor) → Task 4.

## Verification / QA Hooks

QA confirms the register exists, every QA-sourced finding has a corresponding
row, and no open risk is past its review date without an owner response.
