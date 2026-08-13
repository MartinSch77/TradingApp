# SUP.9 — Problem Resolution Management

## Purpose

Ensure every reported problem (defect, incident, unexpected finding from
analysis/test/production-like use) is captured, analyzed, tracked to
resolution, and closed with evidence — never silently dropped.

## Inputs

- GitHub Issues (the tool used for intake), static-analysis findings that are
  triaged as real defects rather than suppressed, `SWE.4`–`SWE.6` failures
  that are not immediately fixed inline.

## Outputs / Work Products

- `work-products/problem-report.md`-conformant record per problem: GitHub
  Issue as the instance, with the required fields (Section content rules in
  the work-product spec) — severity, reproduction, root cause, fix
  commit/PR, verification evidence, closure date.

## Tasks

1. **Report.** Anyone (including an automated pipeline stage failure) opens a
   problem record with enough detail to reproduce or the automated evidence
   (log/report) that detected it.
2. **Analyze.** Severity and priority assigned; a problem affecting a
   released baseline is cross-filed into `MAN.5`'s risk register.
3. **Resolve.** Fix implemented and verified per the SAME verification rules
   the original work product was built under (`strategies/verification-strategy.md`)
   — a bug fix does not get a lighter test bar than new code.
4. **Close.** Closed only with a link to the verifying evidence (a passing
   test, a re-run analysis at 0 for that finding); QA samples closed problems
   for this link's presence.

## Roles

Problem/Incident Manager (Accountable), the engineer who fixes it
(Responsible) — see `roles.md`.

## Base Practices (ASPICE 4.0 reference)

SUP.9.BP1 (record) → Task 1. SUP.9.BP2–BP3 (analyze, initiate) → Task 2.
SUP.9.BP4–BP5 (track to closure, communicate status) → Tasks 3–4.

## Verification / QA Hooks

QA samples a percentage of closed issues each cycle (rate in
`strategies/quality-assurance-strategy.md`) and confirms each sampled issue's
closure evidence link actually resolves to a passing, current result.
