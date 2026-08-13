# Work Product: Risk Register

**Produced by:** MAN.5. **Owning role:** Project Manager (accountable),
risk owner per entry (responsible).
**Location:** `process/risk-register.md` (this project's actual register —
a real, running instance, not a template; see that file directly).

## Content rules

One row per risk: ID (`RISK-NNN`), source (which process/role/QA finding
raised it), description, likelihood (1–5), impact (1–5), score, category
(`strategies/risk-management-strategy.md`'s list), owner, status
(open/mitigating/accepted/closed), last-reviewed date, treatment.

## Quality criteria

Every QA-sourced NOT FOUND/PARTIAL finding has a corresponding row within
the same reporting cycle it was found in (`SUP.1` Task 5's obligation, from
the other side). No open risk past its review date without a logged
response.

## Review requirement

Re-scored at least once per release cycle (`strategies/risk-management-
strategy.md`).

## KPIs

Count of open risks by band; count of overdue reviews (target 0); mean time
to first response after a risk is raised.

## Traceability

Risk ↔ its source (a QA finding, a problem report, an architecture decision)
↔ its treatment (a change request, an accepted-and-monitored note).
