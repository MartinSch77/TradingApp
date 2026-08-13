# Risk Management Strategy

Referenced by `processes/MAN.5-risk-management.md`.

## Scoring scale

Likelihood and impact each scored 1 (rare/negligible) to 5 (near-certain/
severe); risk score = likelihood × impact (1–25). A risk originating from a
QA process-deviation finding is scored on this SAME scale — `process-model.md`
§4's explicit rule that process risk is not automatically low-severity.

| Score | Band | Response expectation |
|---|---|---|
| 1–5 | Low | Accept and monitor; re-score next cycle |
| 6–12 | Medium | Assign owner and a mitigation plan within one cycle |
| 13–19 | High | Mitigation plan required before the next release |
| 20–25 | Critical | May block the current release (`MAN.3` authority) |

## Review cadence

Every open risk is re-scored at least once per release cycle
(`REL-product-release.md`'s cadence); a risk untouched past its review date
is itself flagged by QA as a `MAN.5` process deviation (self-referential by
design, `processes/MAN.5-risk-management.md`'s own Task 4).

## Categories

Technical (design/implementation), Process (QA-sourced deviations), Schedule,
Security/Supply-chain (informed by `docs/tools.md`'s OpenSSF Scorecard/
Coverity findings), ML-specific (`MLE.1-4`'s dataset/model risks — e.g. a
Category A model trained on a dataset later found to leak future data).
