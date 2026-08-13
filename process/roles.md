# Roles

One role can be held by one person across several hats in a project this
size — the table states **authority and independence requirements**, not
headcount. The one hard rule (see `process-model.md` Section 3): **the person
acting as Quality Assurance for a given process execution must not be the
same person who executed that process step.** In an AI-assisted context this
is satisfied by using a *different* model/session with no access to the
engineering conversation for the QA pass (Section 7 of `process-model.md`),
or a human reviewer uninvolved in the change.

## V-model and engineering roles

| Role | Responsible for | Primary processes |
|---|---|---|
| **Requirements Engineer** | Authoring/maintaining `requirements/requirements.sdoc`; assigning `VERIFICATION` method per requirement | SWE.1 |
| **Software Architect** | `docs/architecture.md`, the domain/services/ui layering rule, Qt Charts/Graphs separation and other architecture-level decisions | SWE.2 |
| **Software Designer / Developer** | `docs/design.md` DES entries, `src/` implementation | SWE.3 |
| **Verification Engineer (Unit)** | `tests/tst_*.cpp`, static analysis configuration, coverage/mutation/fuzz harnesses | SWE.4 |
| **Integration Engineer** | Cross-module/layer integration, the integration test level (`strategies/verification-strategy.md` §Integration) | SWE.5 |
| **Qualification/System Test Engineer** | System-level acceptance evidence: Squish GUI suite, Test Center upload, `docs/traceability.html`'s requirement-level view | SWE.6 |
| **Release Engineer** | `tools/publish_release.{sh,ps1}`, packaging scripts, the qualification bundle | REL-product-release |

## Supporting-process roles

| Role | Responsible for | Primary processes |
|---|---|---|
| **Project Manager** | Process selection/tailoring authority (`process-model.md` §8), the project plan, schedule and resourcing, risk triage ownership | MAN.3, MAN.5 |
| **Quality Manager (QM)** | Owns the quality management SYSTEM: ensures Process Management and Process Design (below) are actually carried out, appoints/rotates the QA role, receives `downloads/TradingApp-qa-report.md` | Umbrella over SUP.1, PIM.3 |
| **Quality Assurance (QA)** | Independent confirmation the process was followed (never engineering correctness); produces the QA report; escalates unresolved deviations as risks | **SUP.1** (see `processes/SUP.1-quality-assurance.md`) |
| **Configuration Manager** | Baselines, tags, and the identity of every controlled work product; the `git`/CMake `VERSION` discipline itself | SUP.8 |
| **Problem/Incident Manager** | Intake, triage and closure tracking of defects/incidents (GitHub Issues is the tool; the process is what SUP.9 defines regardless of tool) | SUP.9 |
| **Change Control Board (CCB)** — may be one senior role in a small team | Approves/rejects change requests against baselined work products | SUP.10 |
| **Process Architect** | Designs/drafts process and strategy documents (`process-model.md` §8) — a senior/chief engineering expert role, deliberately distinct from who approves them | Authors `processes/*.md`, `strategies/*.md` |
| **Process Owner** | Approves and releases a process version (this framework's own `CHANGELOG.md`/`VERSION`); typically the same person as Project Manager or Quality Manager in a small team, but the AUTHORITY is distinct from the Process Architect's | Releases `process/` versions |
| **Process Improvement Lead** | Acts on QA's effectiveness/efficiency observations (`process-model.md` §6) to propose process tailoring | PIM.3 |
| **DevOps Engineer** | CI/CD pipeline definitions (`.github/workflows/*.yml`), environment provisioning (`setup.sh`/`.ps1`), the "everything as code, everything reproducible" discipline underlying every process above | DEVOPS-principles (cross-cutting) |

## RACI summary (who does what per process)

R = Responsible (does the work), A = Accountable (answers for the outcome),
C = Consulted, I = Informed.

| Process | R | A | C | I |
|---|---|---|---|---|
| MAN.3 Project Management | Project Manager | Project Manager | Software Architect, QM | All roles |
| MAN.5 Risk Management | Project Manager | Project Manager | QA (source of process-deviation risks), all engineering roles | All roles |
| **SUP.1 Quality Assurance** | **QA** | **Quality Manager** | Process Architect (spec questions only, never findings) | Project Manager, all audited roles |
| SUP.8 Configuration Management | Configuration Manager | Configuration Manager | Release Engineer | All roles |
| SUP.9 Problem Resolution | Problem/Incident Manager | Problem/Incident Manager | Verification/Qualification Engineers | Project Manager |
| SUP.10 Change Request Management | Change Control Board | Change Control Board | Process Architect (process-doc changes), Software Architect (design-impacting changes) | All roles |
| SWE.1–SWE.6 | Named engineering role per row above | Software Architect | Requirements Engineer ↔ every downstream role | QA |
| PIM.3 Process Improvement | Process Improvement Lead | Quality Manager | QA (effectiveness data), Project Manager | All roles |
| REL Product Release | Release Engineer | Project Manager | QA (must confirm no missing-evidence hard fail — `process-model.md` "informational except" rule) | All roles |
| DevOps Principles | DevOps Engineer | Software Architect | All engineering roles | All roles |
