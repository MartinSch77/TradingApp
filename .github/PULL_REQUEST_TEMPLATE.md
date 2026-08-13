## What & why

<!-- What does this change, and why? Reference REQ-… ids where applicable. -->

## Quality checklist

- [ ] `tools/run_tests.sh build` — all tests pass
- [ ] `python3 tools/trace_report.py` — no hard traceability gaps
      (new tests tagged `@tstid` / `@design` / `@relation(REQ-…, scope=function)`)
- [ ] `tools/static_analysis.sh build` — no new findings
- [ ] New/changed requirements edited in `requirements/requirements.sdoc`
      (not in the generated `docs/requirements.md`)
- [ ] No secrets, no layering violations (domain ← services ← ui)

<!-- The two sections below are this project's process framework
     (process/README.md) speaking through GitHub: they are the same fields
     process/strategies/change-management-strategy.md requires for any
     change to a baselined work product, and reused here for every PR so the
     evidence trail is uniform whether or not the target was formally
     baselined yet. See process/process-model.md for why QA and
     verification are asked as separate questions below. -->

## Change impact

- Related issue/change request:
- Requirements affected:
- Architecture/design affected:
- Tests affected:
- Safety or real-money path affected: <!-- yes/no — "yes" requires the
  extra independent approval in process/strategies/change-management-strategy.md -->
- Configuration items affected:
- Risks introduced or changed: <!-- link a process/risk-register.md entry, or "none" -->

## Verification evidence

- Unit verification:
- Component/integration verification:
- Software verification:
- Regression result:
- Static-analysis result:

## Reviews and approvals

- Technical review:
- Requirements/design review:
- QA disposition: <!-- process-model.md §2/§3 — a PROCESS-conformance
  statement, not a repeat of the technical review above -->
- Release relevance: <!-- does this change belong in the next release baseline? -->
