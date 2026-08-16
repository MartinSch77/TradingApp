# PIM.4 — Process Change Lifecycle

## Purpose

Define the explicit lifecycle and transition criteria for changes to the
process framework itself. This prevents informal, undocumented drift in the
rules that govern the project.

## Inputs

- `process/process-model.md`
- `process/processes/*.md`
- `process/work-products/*.md`
- `process/CHANGELOG.md`
- `process/VERSION`
- `downloads/TradingApp-qa-report.md`
- `MAN.5` risk items and `SUP.9` problem records

## Outputs / Work Products

- a process-change record with current lifecycle state
- updated `process/CHANGELOG.md` entry for released versions
- an updated `process/VERSION` if the baseline changes

## Tasks

1. **Draft**
   - Capture the issue, rationale, and expected scope of the change.
   - Identify the affected process, work products, templates, or strategies.
2. **Proposed**
   - Write a change request with the reason, impact, and needed review path.
   - Record the targeted baseline and any risk or governance trigger.
3. **Under Review**
   - Execute the required review steps and assess impact on traceability and
     maintainability.
   - Confirm it does not undermine the independence of QA or human approval.
4. **Approved**
   - Resolve all blocking findings.
   - Confirm the change is bounded, auditable, and ready for release.
5. **Released**
   - Merge the change into the active process baseline.
   - Update version history and any affected traceability records.
6. **Superseded / Retired**
   - If replaced or removed, record the reason and preserve the historical
     record for audit and traceability.

## Roles

Process Owner (Accountable), Process Architect (Responsible), Quality Manager
(Review), Project Manager (Approval authority for the final change decision).

## Base Practices (ASPICE 4.0 reference)

This document operationalizes the lifecycle principles behind change control,
process improvement, and configuration management: every baseline change must be
observable, controlled, and reviewable, even when the changed item is a process
document rather than software.

## Verification / QA Hooks

QA verifies that a process change reaches each lifecycle state only after the
required review and evidence are recorded. A change that is merged without a
state record or without a traceability validation is treated as a process
deviation and recorded as a risk.
