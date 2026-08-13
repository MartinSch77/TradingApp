# PIM.3 — Process Improvement

## Purpose

Act on QA's conformance findings and effectiveness/efficiency observations
(`process-model.md` §6) to tailor or improve the process — the mechanism that
closes the loop QA itself is deliberately barred from closing (independence,
`SUP.1`).

## Inputs

- `downloads/TradingApp-qa-report.md` (both its CONFIRMED/NOT FOUND
  conformance findings and its effectiveness indicators), the risk register
  (`MAN.5`), closed problem trends (`SUP.9`).
- Cadence and trigger thresholds: `strategies/process-improvement-strategy.md`.

## Outputs / Work Products

- `work-products/process-improvement-record.md`-conformant record: one entry
  per improvement, its trigger (which QA finding or trend), the change
  proposed, and — once approved via `SUP.10` — the resulting
  `process/CHANGELOG.md` entry.

## Tasks

1. Review the QA report each cycle for recurring NOT FOUND/PARTIAL findings
   (a one-off miss is a `MAN.5` risk; a *pattern* is a process-design
   problem).
2. Review effectiveness/efficiency indicators (cycle time, ratchet trend,
   review turnaround) for degradation even where conformance is clean — a
   followed process that is getting slower or less effective still needs
   attention.
3. Propose a process change through `SUP.10-change-request-management.md`;
   PIM.3 never edits `processes/*.md` directly on its own authority (that
   would collapse the Process Architect/Process Owner separation,
   `process-model.md` §8).
4. Track whether an implemented change actually improved the indicator it
   targeted, at the next cycle.

## Roles

Process Improvement Lead (Responsible), Quality Manager (Accountable) — see
`roles.md`.

## Base Practices (ASPICE 4.0 reference)

Maps to the Process Improvement Process Attribute (PA 3.2 / ISO/IEC 33020
capability dimension) rather than a single ASPICE base-practice group: this
project treats it as a first-class process because a framework designed to
be "testable" and "maintainable" (`process-model.md` §5) needs an explicit
mechanism for acting on what its own tests find.

## Verification / QA Hooks

QA confirms every recurring finding (same NOT FOUND on 2+ consecutive
reports) has a corresponding PIM.3 record — a repeated finding with no
improvement action is itself escalated as a `MAN.5` risk.
