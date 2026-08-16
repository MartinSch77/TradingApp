# Work Product: Traceability Matrix

**Produced by:** SWE.1–SWE.6 (jointly, via their own work products).
**Owning role:** Software Architect (accountable for the matrix being
truthful; no single engineering role can vouch for the whole chain alone).
**Location:** `docs/traceability.html` (generated,
`tools/trace_report.py`) for REQ↔DES↔TS↔result; `process/
traceability-matrix.md` (this framework's own, hand-maintained) for
process↔work-product.

## Content rules

The REQ↔DES↔TS matrix is GENERATED, never hand-edited — a hand-edited
traceability claim is worse than none, because it can assert a link that
does not exist in the sources. The process↔work-product matrix is
hand-maintained here because it changes rarely (only when a process or work
product spec is added/removed) and is itself checked by
`tools/check_process_docs.py`.

The cross-project reference model requires the following forward traceability
chain for every feature and every requirement:

Feature → Requirement → Design Element → Implementation → Verification → Evidence → Human approval

This is a hard requirement for release readiness and is the minimum evidence
chain a downstream project must maintain in the same repository pattern.

## Quality criteria

0 hard gaps (every REQ with `VERIFICATION` including `T` has an executed
test) at release time; every work product named in a `processes/*.md` file
resolves to a real file in `work-products/`.

For the reference process itself, the chain is checked in the same way as the
product traceability: no feature may be declared complete without a linked
requirement, no requirement without a design element or verification case, and
no safety-relevant item without human final approval.

## Review requirement

None beyond the mechanical check — see Quality criteria.

## KPIs

Hard-gap count (target 0), open-gap count (informational — legitimately
non-test-verified requirements per their own `VERIFICATION` field).

## Traceability

This work product IS the traceability record — it has no further upstream
link beyond the specs it draws from.
