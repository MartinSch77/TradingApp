# Work Product: Configuration Baseline

**Produced by:** SUP.8. **Owning role:** Configuration Manager.
**Location:** git tags (`vX.Y.Z`, `process-vX.Y.Z`), GitHub Releases.

## Content rules

A baseline names an exact commit SHA, the artefacts attached to it (binaries,
docs, qualification bundle), and is never mutated after creation
(`strategies/configuration-management-strategy.md`).

## Quality criteria

Every artefact attached is reachable from and built at the exact tagged
commit — `tools/publish_release.sh`'s own "artefacts named for another
version are skipped and named" rule is this criterion enforced in code.

## Review requirement

`REL-product-release.md`'s gate IS the review for this work product — a
baseline is not created without passing it.

## KPIs

Count of baselines created vs. rolled back (a rollback here means a NEW
corrective baseline, never an edit — see Quality criteria); time from gate-
green to tag.

## Traceability

Baseline ↔ every work product's state at that commit (design, tests, QA
report, risk register) — the single point where "what did the project look
like on this date" is answerable exactly.
