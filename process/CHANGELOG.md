# Process Framework Changelog

Versioned independently of the TradingApp application (`process/VERSION`,
tags `process-vX.Y.Z`) — see `process/README.md`.

## 0.1.0 — initial release (2026-08-13)

First version of the process framework: `process-model.md` (the SUP.1/
verification distinction, ASPICE 4.0 landscape, DevOps principles, MLE
model-category scope), `roles.md`, 16 process specifications
(MAN.3, MAN.5, SUP.1, SUP.8, SUP.9, SUP.10, SWE.1–SWE.6, PIM.3, REL,
DEVOPS-principles, MLE.1–4), 9 strategies, 17 work-product specifications,
7 AI-executable review checklist templates, `tools/qa_report.py` (the QA
report generator) and `tools/check_process_docs.py` (the process
framework's own traceability check).

GitHub operationalization landed alongside: the PR template gained
Change Impact / Verification Evidence / Reviews and Approvals sections;
five new issue templates (change request, problem report, project risk,
QA nonconformance, process improvement) realize the
`proposed → analyzed → approved → implemented → verified → closed` state
scheme as GitHub labels.

`requirements/requirements.sdoc`'s grammar gained six optional fields
(`SOURCE`, `RATIONALE`, `PRIORITY`, `STATUS`, `ACCEPTANCE_CRITERIA`,
`ISSUE`); REQ-F-034 and REQ-F-035 were marked `STATUS: proposed` for atomic
decomposition (tracked separately — see `process/requirements-schema-
upgrade.md` and its linked GitHub issue) rather than split without a
careful, individually-reviewed re-mapping of the ~15 and ~23 existing test
references each carries.

Known gaps, stated rather than hidden (`process-model.md` §10): the
requirements atomization above is not yet executed, only scoped; this is
the framework's first release and has not yet accumulated a full audit
cycle's worth of real QA report history.
