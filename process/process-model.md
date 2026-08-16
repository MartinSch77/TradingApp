# Process Model — strategy, ASPICE mapping, and what Quality Assurance actually is

A **process** is the instrument a project or organization uses to keep
project risk at an acceptable level — not paperwork for its own sake. A
**strategy** is goals plus a plan for reaching them. This document is
TradingApp's process strategy: what the goals are, which Automotive SPICE®
(ASPICE) 4.0 processes realize them, and — the part this framework was
specifically built to add — how **Quality Assurance (QA)** independently
confirms the process is actually followed.

Everything here is written in English regardless of the language a request to
change it arrives in, per this project's own convention (see the root
`CLAUDE.md`'s "Language" section) applied one level up, to process itself.

## 1. The question this framework answers that the existing toolchain does not

The root `CLAUDE.md` already describes an unusually deep **engineering
verification** toolchain — seven static analyzers, three sanitizers, mutation
testing, fuzzing, requirements-as-code traceability
(`tools/trace_report.py`), a Squish/Coco GUI suite, a Test Center upload. All
of that is real, automated, and answers one question: **is the product
correct?**

It does not answer: **did the project follow a defined, repeatable process to
build it — checked by someone other than the people who did the work?** That
second question is what Quality Assurance answers, and it is a genuinely
different discipline from verification, not a rebranding of it.

> **A note on terminology this project has to be explicit about.** Qt itself
> markets a family of products as "Qt Quality Assurance" (Squish, Coco, Test
> Center) — this repository uses all three. **That naming is Qt's marketing
> term for test-automation and coverage tooling, not "Quality Assurance" in
> the IEEE 730 / ISO/IEC 12207 sense this framework uses.** Squish/Coco/Test
> Center evidence is **verification** evidence (Section 2 explains why) and
> is cited as such throughout `processes/SWE.4-unit-verification.md` and
> `processes/SWE.6-qualification-test.md`. When this framework says "QA," it
> always means the independent process-conformance role defined in Section 3
> — never the Qt product family.

## 2. Quality Assurance is not Verification — the distinction this framework is built around

This is the single most important sentence in this document:

> **Verification asks "is the ENGINEERING WORK PRODUCT correct?" (tests pass,
> static analysis is clean, a design review found no defects). Quality
> Assurance asks "was the DEFINED PROCESS actually followed to produce it?"
> — and QA answers that question independently of the people who did the
> engineering, including organizationally.**

This is not a house convention — it is how [IEEE Std 730-2014 (Software
Quality Assurance Processes)](https://standards.ieee.org/) and
[ISO/IEC/IEEE 12207](https://www.iso.org/standard/63712.html) define the two
disciplines, and it is exactly the split Automotive SPICE draws between its
**SUP.1 (Quality Assurance)** process and the **engineering processes
(SWE.1–SWE.6)** whose outputs SUP.1 audits:

| | Verification (engineering's job) | Quality Assurance (SUP.1) |
|---|---|---|
| Question | Is *this* test suite/design/code correct? | Was the *defined process* followed to produce it? |
| Performed by | The engineers who did the work (or peers reviewing it) — `processes/SWE.4-unit-verification.md`, `SWE.5`, `SWE.6` | An **independent** role — organizationally separate, see Section 3 |
| Evidence | Test results, static-analysis reports, code review comments, coverage figures | Confirmation that the process's own tasks were executed, in the order and with the roles/reviews the process demands |
| Tooling in THIS repo | cppcheck, clang-tidy, Axivion, Coco, Squish, Test Center, `tools/trace_report.py`, sanitizers, `tst_*.cpp` | `tools/qa_report.py`, `templates/review-checklist-process-compliance.md`, this framework's own process specs as the reference |
| A finding here means | The PRODUCT has a defect | The PROCESS was not followed — which is a **project risk**, whether or not the product this time happens to be fine |
| What a "0 findings" result proves | The product passed the checks that exist | Nothing about the product's quality on its own — only that the checks were actually run, by the people the process names, with the evidence the process demands |

The ASPICE Process Assessment Model states SUP.1's purpose plainly: *"to
provide independent confirmation that... work products and processes comply
with predefined provisions"*. Independent is load-bearing — Section 3 defines
what independence means here concretely, since "everyone reviews everyone's
own PRs" is not independence.

**Why this distinction matters even when execution is automated.** A project
can reasonably say "our pipeline can't really go wrong — it's scripted, it's
in CI, a human can't skip a step by accident." That argument defends
*repeatability*, not *conformance*. QA still needs a reference process to
check the automation ITSELF against: does the pipeline actually run the
review the process demands, is the reviewer role actually independent, does a
process change go through the change process before the pipeline changes,
is the evidence the pipeline produces actually the evidence the process asked
for. Automation removes *some* failure modes (a step silently skipped) and
leaves others fully open (the step itself never having been specified
correctly, or having drifted from what was specified). That gap is precisely
what `tools/qa_report.py` (Section 6) checks for, and precisely why "we
automated it" is not, on its own, an answer to an ASPICE assessor.

## 3. Independence — what it means concretely in this project

ASPICE and IEEE 730 both require QA to be independent, "including
organizationally," from the work it audits. In a project of this size that
cannot mean a separate department; it means a concrete, checkable separation
of *authority and information flow*:

1. **QA never writes or fixes the engineering work product it audits.** A QA
   finding is *"no evidence process step X was executed"* or *"work product Y
   does not meet its content rules"* — never a code fix, never a design
   change. Section 5 of `processes/SUP.1-quality-assurance.md` states this as
   a hard rule.
2. **QA reports do not route through the role that owns the process being
   audited.** The QA report (`downloads/TradingApp-qa-report.md`) is a
   standalone artefact addressed to Project Management (`roles.md`), not a
   pull-request comment the process owner can edit away.
3. **QA's reference is the *process specification*, not the current
   implementation's habits.** `tools/qa_report.py` reads `processes/*.md`
   verbatim as its checklist; it is not tuned to whatever the pipeline
   currently happens to do. If the pipeline and the spec disagree, that is
   itself a QA finding (a process deviation — Section 4), not evidence the
   spec should quietly change to match.
4. **An AI acting as QA is independence-compatible on the same logic this
   project already applies to AI-assisted engineering** (see
   `docs/ai-assisted-development.md`): an AI reviewer holds no stake in
   whether the finding looks good, has no authorship credit to protect, and
   — critically — is given ONLY the process specification and the
   repository's observable state, never a narrative from the engineer being
   audited. Section 7 makes this concrete for both Claude and an external
   tool (e.g. ChatGPT) acting as QA or as an ASPICE assessor.

## 4. Process deviations are risks, first

A missing review, a work product produced without its required content, a
process step executed by the wrong role — every one of these is, on
discovery, filed as a **risk** in
`processes/MAN.5-risk-management.md`'s risk register **before** anyone
decides whether it also turned into an actual defect this time. This mirrors
ASPICE's own logic: SUP.1's purpose statement ties process conformance
directly to risk, and a process exists *specifically* to keep project risk at
an acceptable level (this document's opening sentence) — so evidence the
process was not followed is evidence risk may be higher than assumed,
independent of whether today's output happens to be fine. `MAN.5` defines
severity/likelihood scoring and who triages a QA-sourced risk.

## 5. Process quality — the process specification is itself a work product

This framework applies to itself the same engineering discipline the root
`CLAUDE.md` applies to code (see its own "Don't add features... beyond what
the task requires" and ratchet-based metrics culture). A process
specification here must be:

- **Loosely coupled** — each `processes/*.md` file names the work products it
  produces/consumes by reference (a filename in `work-products/`), never by
  re-stating their content rules inline; a work product's rules live in
  exactly one place.
- **Testable** — `tools/check_process_docs.py` mechanically checks that every
  process has the required sections (Purpose, Inputs, Outputs/Work Products,
  Tasks, Roles, Base Practices, Verification/QA Hooks) and that every named
  work product resolves to a real file. A process document that cannot be
  parsed this way is treated as a defect in the process asset, exactly like a
  compile error in source.
- **Traceable** — `process/traceability-matrix.md` is the generated
  process→work-product matrix; `tools/qa_report.py` cross-checks it against
  the repository's actual output (does `docs/design.md` exist and is it
  newer than the requirements it claims to satisfy, etc.).
- **Low complexity, reusable, human-maintainable** — one process per file,
  one work product per file, no file over roughly 150 lines; a strategy
  document is referenced from every process that needs it rather than
  duplicated (e.g. `strategies/verification-strategy.md` is read by
  `SWE.4`, `SWE.5` and `SWE.6` alike). A maintainer (or an AI) can update one
  process without reading the whole framework.

## 6. Effectiveness and efficiency — process monitoring is not just conformance

Confirming a process was *followed* is necessary but not sufficient — a
followed process that does not actually reduce risk or reach project goals is
a process that needs improvement, not more auditing. `tools/qa_report.py`
therefore reports two distinct things per process, never conflated:

- **Conformance** (did the defined steps happen, with the right roles and
  evidence) — QA's own, primary question.
- **Effectiveness/efficiency indicators** (cycle time, defect escape rate,
  ratchet trend, review turnaround) sourced from the SAME evidence QA already
  reads, reported alongside so Project Management and
  `processes/PIM.3-process-improvement.md` can judge whether the process
  itself needs tailoring — a judgement QA states as an observation, never as
  a mandate, since changing the process is Project Management's authority
  (Section 8) and Process Improvement's mechanism.

## 7. AI-assisted engineering AND AI-assisted QA, in a regulated-style setting

TradingApp's `docs/ai-assisted-development.md` already documents how AI
assists **engineering** here. This framework extends the same demonstration
to governance: **the process is designed so an AI (or an external human
auditor) can perform both roles independently** —

- **AI as reviewer/QA**: `templates/*.md` are written as literal, executable
  checklists — a yes/no/evidence-cited item per line, with the exact
  repository path or command that produces the evidence — precisely so an
  LLM (Claude, or, per the project owner's stated intent, ChatGPT as an
  independent second opinion) can execute one without inventing criteria.
  `tools/qa_report.py` is the same idea in code rather than prompt: it reads
  the SAME specification and produces a report with no room for a model's
  own judgement about what "compliant" means.
- **AI as ASPICE assessor**: because every process file follows the SAME
  section structure and cites the ASPICE 4.0 base practices it implements
  (Section 9), an assessor — human or AI — can walk `processes/*.md` against
  the current Process Assessment Model (PAM) rating scale (N/P/L/F per base
  and generic practice) without needing a separate assessment-preparation
  document. This is the concrete mechanism behind the project owner's stated
  goal: *"I might ask ChatGPT to perform an ASPICE assessment on this
  project."*

This dual design is also this project's answer to a broader question: **can
AI-assisted development work inside a regulated-style environment at all** —
the kind ISO 26262 (functional safety), ISO/SAE 21434 (automotive
cybersecurity) or the EU Cyber Resilience Act (CRA) eventually demand.
Section 10 states the honest scope limit on that claim.

### Continuous improvement and process erosion

This process framework is an **initial release baseline**, not an end state.
The process is deliberately designed to be continuously improved, with new
rules, evidence, or tailoring introduced under the same discipline the
project applies to code: explicit review, traceability, and risk-based
acceptance.

A process is considered to be eroding when any of the following occur:

- process steps are followed by habit rather than by written requirement
- evidence is missing or stale even though the process still says it is required
- roles drift without a formal change request
- review checklists are bypassed under time pressure
- quality gates become informal and no longer enforced by automation or review
- the process becomes harder to understand than the work it is meant to govern

Process erosion is treated as a **project risk**, not as a harmless local
variation. `processes/MAN.5-risk-management.md` and the QA workflow in
`processes/SUP.1-quality-assurance.md` are the formal escalation path for this
risk. Any future process change must document why the change is required,
what evidence motivated it, and how the change will be monitored for drift.

This is a standing condition for further development: the process is expected
and required to evolve, but it must evolve with evidence, traceability, and a
maintainability discipline equal to that of the software it governs.

## 8. Lifecycle for process changes

The process itself is treated as a living work product and therefore has its
own explicit lifecycle. A process change is not an informal local edit; it is a
controlled state transition subject to the same traceability and review
expectations as any other product baseline.

The lifecycle states are:

- **Draft** — the proposed change is being written and scoped.
- **Proposed** — a change request exists with rationale, impact analysis, and
  affected process or work-product references.
- **Under Review** — the change is being assessed by the designated reviewers,
  including impact on traceability, auditability, and maintainability.
- **Approved** — all blocking issues are resolved, the change has a clear
  owner, and the review decision is recorded.
- **Released** — the approved change is merged into the current baseline and
  versioned in `process/CHANGELOG.md` and `process/VERSION`.
- **Superseded** — the process version is replaced by a newer one, but the old
  version remains archived for auditability and historical traceability.
- **Retired** — the process or rule is no longer used and has a documented
  reason for removal or exclusion.

The transition criteria are:

1. **Draft → Proposed**
   - A change-request record exists.
   - The reason for the change is stated in terms of risk, defect, or
     process erosion.
   - Affected files are named and traceability impact is estimated.
2. **Proposed → Under Review**
   - The change request is assigned to a reviewer or board.
   - Impact on related processes, work products, and templates is documented.
   - The change does not bypass the normal QA or risk-review path.
3. **Under Review → Approved**
   - No blocking review comments remain unresolved.
   - Traceability checks pass (`tools/check_process_docs.py`).
   - Process ownership and release responsibility are clear.
4. **Approved → Released**
   - The change is merged to the active baseline.
   - The release record is updated in `process/CHANGELOG.md` and the process
     version is incremented in `process/VERSION`.
   - The updated baseline is validated against the repository's process checks.
5. **Released → Superseded / Retired**
   - A replacement or cancellation is documented in the changelog.
   - Historical evidence remains accessible for audit and traceability.

This is the formal control mechanism behind the project's statement that the
process may evolve continuously, but only with evidence, review, and explicit
release discipline. The lifecycle rules are implemented in
`processes/PIM.3-process-improvement.md`,
`processes/SUP.10-change-request-management.md`, and
`processes/PIM.4-process-change-lifecycle.md`.

## 9. Cross-project reference process: reusable standard for every project

This repository is the **reference process implementation** for a cross-project
software development and assurance model. The project intentionally keeps a
shared standard that downstream repositories can inherit via GitHub templates,
submodules, or repository scaffolding.

The reference model is intentionally simple but strict:

1. **Feature backlog**: every new story or idea is captured as a feature item.
2. **Requirement decomposition**: every feature is broken down into one or more
   requirements.
3. **Design allocation**: each requirement is linked to one or more design
   elements or components.
4. **Implementation mapping**: each design element points to the implementing
   code or work product.
5. **Verification evidence**: every requirement has a named verification method
   and evidence reference.
6. **Release approval**: the final release decision remains a human decision, not
   an AI decision.
7. **Independent QA**: quality assurance checks conformance to the process, not
   just the correctness of the engineering output.

The minimum valid chain is therefore:

Feature → Requirement → Design Element → Implementation → Verification → Evidence → Human approval

This is the standard that all projects must follow to be eligible for release
or assessment. Any project that cannot show this chain does not have a
complete traceability story.

For safety-relevant work, the default rule is stricter:

- human review is mandatory
- independent QA confirmation is mandatory
- a qualified person must confirm the safety-relevant conclusion or approval
- AI-generated content is treated as assistive only and must be reviewed before
  use as evidence

## 9. Process selection, tailoring, and authority

Per ASPICE's own MAN.3 practice, **Project Management selects and tails the
process** for the project's actual context — not Quality Assurance (which
only confirms conformance to whatever was selected) and not individual
engineers. `processes/MAN.3-project-management.md` names the Project
Management role as the tailoring authority, and `roles.md` states who plays
that role in this repository.

**Who designs a process, versus who releases it, versus who audits it — three
different roles, deliberately:**

| Activity | Role | Why not the same role |
|---|---|---|
| Designs/drafts a process or strategy | **Process Architect** (a senior/chief engineering expert) — see `roles.md` | Design needs deep engineering context (what SWE.4 must demand of a unit test here specifically); that context lives in engineering expertise, not in management or QA |
| Approves/releases a process version | **Process Owner / Project Management** | Release is an organizational commitment — resourcing, timeline, and cross-team buy-in are management decisions, not technical ones |
| Confirms an active process is followed | **Quality Assurance** | Independence (Section 3) — the same reason QA never designs the thing it audits |
| Judges whether the process needs to change | **Process Improvement (PIM.3)**, fed by QA's effectiveness observations (Section 6) | Improvement decisions weigh cost/benefit project-wide, which is again a management-adjacent, not an audit, function |

## 9. Process landscape — every process in this repository, ASPICE-mapped

| Process | ASPICE 4.0 ref | One-line purpose | Spec |
|---|---|---|---|
| Project Management | MAN.3 | Plans, tracks, and tailors the project's process | `processes/MAN.3-project-management.md` |
| Risk Management | MAN.5 | Identifies, scores, and tracks risk — including process-deviation risk from QA | `processes/MAN.5-risk-management.md` |
| Quality Assurance | **SUP.1** | Independently confirms the process was followed | `processes/SUP.1-quality-assurance.md` |
| Configuration Management | SUP.8 | Baselines, versions, and controls every work product | `processes/SUP.8-configuration-management.md` |
| Problem Resolution Management | SUP.9 | Tracks defects/incidents from report to closure | `processes/SUP.9-problem-resolution-management.md` |
| Change Request Management | SUP.10 | Controls changes to baselined work products | `processes/SUP.10-change-request-management.md` |
| Requirements Elicitation | SWE.1 | Captures and maintains `requirements/requirements.sdoc` | `processes/SWE.1-requirements-elicitation.md` |
| Architectural Design | SWE.2 | Maintains `docs/architecture.md` and the layering rules | `processes/SWE.2-architectural-design.md` |
| Detailed Design & Unit Construction | SWE.3 | Maintains `docs/design.md` and `src/` | `processes/SWE.3-detailed-design-and-unit-construction.md` |
| Unit Verification | SWE.4 | Unit tests + static analysis on every unit | `processes/SWE.4-unit-verification.md` |
| Integration & Integration Test | SWE.5 | Layer/module integration and its own test level | `processes/SWE.5-integration-and-integration-test.md` |
| Qualification Test | SWE.6 | System-level acceptance against requirements | `processes/SWE.6-qualification-test.md` |
| Process Improvement | PIM.3 | Acts on QA/effectiveness findings to tailor the process | `processes/PIM.3-process-improvement.md` |
| Product Release | (project-specific, MAN.3/SUP.8-adjacent) | Gate + publish a versioned release | `processes/REL-product-release.md` |
| DevOps Principles | (cross-cutting) | Continuous integration/delivery practice underlying every process above | `processes/DEVOPS-principles.md` |
| Machine Learning Engineering | MLE.1–MLE.4 (emerging ASPICE-for-AI scope) | Data provenance, leakage-safe splitting, evaluation/approval thresholds, model versioning/withdrawal — for TradingApp-trained models only (Categories A/B/C) | `processes/MLE.1-4-machine-learning-engineering.md` |

Cross-cutting **strategies** (Section 5's "referenced, not duplicated" rule)
live in `strategies/` and are cited by the processes above; **work products**
and their content/quality rules live in `work-products/`.

## 10. Scope and honesty

This framework is a **demonstrator** of how process-as-code, independent
QA, and AI-assisted execution can compose in a regulated-style setting. It is
**not**:

- A certified quality management system, nor a substitute for one.
- A claim that TradingApp is ISO 26262 / ISO 21434 / CRA compliant — those
  standards impose safety/security lifecycle requirements (hazard analysis,
  TARA, vulnerability handling, SBOM obligations) this project does not
  currently implement in full; this framework shows the PROCESS SCAFFOLDING
  those standards would slot into (SUP.1 for their own required independent
  QA function, SUP.9 for their incident-handling obligations, SUP.8/CRA's
  SBOM expectations), not the standards' complete content.
- A replacement for a real ASPICE assessment by an accredited assessor —
  Section 7's "AI as ASPICE assessor" is a self-assessment aid, stated as
  such everywhere it is offered.

Where this framework is silent or thin, `tools/qa_report.py` says so
explicitly (**NO EVIDENCE FOUND**, never a guessed pass) — the same honesty
discipline `CLAUDE.md` already applies project-wide ("a probability is
MEASURED, never asserted").
