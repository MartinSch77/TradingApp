# Project Management Strategy

Referenced by `processes/MAN.3-project-management.md`.

## Planning artefact

This project expresses its plan as `docs/roadmap.md` (forward-looking,
cost-ordered) plus the realized history in `CHANGELOG.md` and git tags —
deliberately not a separate Gantt/plan document that would drift from the
roadmap the moment either changes. `work-products/project-plan.md` states why
this composition satisfies MAN.3's planning work product without duplication.

## Tailoring log

| Process | Tailoring from the untailored ASPICE base practice set | Justification |
|---|---|---|
| SWE.5 (Integration test) | No separate integration-test EXECUTABLE per se; integration-scope tests live in the same `tests/` suite as unit tests, distinguished by the "integration tests (services layer)" comment block in `tests/CMakeLists.txt` | Project size does not justify a second test binary/harness; the distinction is enforced by convention + this framework's review criteria (`strategies/verification-strategy.md`), not tooling |
| SUP.9/SUP.10 tooling | GitHub Issues/PRs/labels instead of a dedicated ALM tool | The project already lives on GitHub; introducing a second system would fragment evidence DevOps principle #1 (everything as code) argues against |
| MLE.1–4 | Applied only to Category A (TradingApp-trained) models; B/C get versioning only | Full MLE lifecycle on a model this project does not train would document controls it cannot exercise |

Any new tailoring decision is added here with the same two-column shape
before it takes effect — QA checks this log exists and is current whenever
it audits `MAN.3`.

## Milestones

Realized as GitHub Milestones tied to `docs/roadmap.md` items; a milestone
closes only when every issue assigned to it is in `state:closed`
(`strategies/problem-resolution-strategy.md`/`change-management-strategy.md`'s
shared state scheme).
