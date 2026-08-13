# Work Product: Change Request

**Produced by:** SUP.10. **Owning role:** Change Control Board.
**Location:** GitHub Issue (`.github/ISSUE_TEMPLATE/change-request.yml`),
mirrored onto its implementing PR.

## Content rules

Per `strategies/change-management-strategy.md`'s mandatory fields:
requirements affected, architecture/design affected, tests affected, safety/
real-money path affected (yes/no — gates the extra-approval rule),
configuration items affected, risks introduced or changed. State field uses
the shared `proposed → analyzed → approved → implemented → verified →
closed` scheme.

## Quality criteria

Never reaches `approved` with a blank "safety or real-money path affected"
field — an unanswered question there is treated as "yes" (the conservative
default) until explicitly assessed otherwise.

## Review requirement

`templates/review-checklist-process-compliance.md`'s change-control section;
two independent approvals if the safety/money-path field is "yes"
(`strategies/change-management-strategy.md`).

## KPIs

Cycle time proposed→closed; count reaching `implemented` without a recorded
`approved` state (target 0 — a process deviation if it happens).

## Traceability

Issue ↔ PR ↔ commits ↔ requirements/design/tests named in its impact fields.
