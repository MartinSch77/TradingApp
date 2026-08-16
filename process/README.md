# TradingApp Process Framework — process-as-code

This directory is a **separately versioned process asset** that belongs to the
TradingApp project (it lives in the same repository and is tagged
`process-vX.Y.Z`, independent of the application's own `vX.Y.Z` tags — see
[`CHANGELOG.md`](CHANGELOG.md) and [`VERSION`](VERSION)). It defines *how* this
project builds software, not *what* the software does — that is
`requirements/requirements.sdoc` and `docs/design.md`.

## Why this exists

TradingApp already has an unusually deep **engineering verification**
toolchain: seven static analyzers, three sanitizers, mutation testing,
fuzzing, requirements-as-code traceability, a Squish/Coco GUI test suite, a
Test Center upload — all described in the root `CLAUDE.md` and `docs/`. That
toolchain answers *"is the product correct?"*.

It does **not** answer a different, equally real question: *"did the project
actually follow a defined, repeatable PROCESS to get here — and is there
independent evidence of that, separate from the people who did the work?"*
That second question is what **Quality Assurance (QA)** answers, and this
framework exists to define the process QA checks against, plus QA itself.

**Read [`process-model.md`](process-model.md) first** — it explains the
QA-vs-verification distinction (IEEE 730 / ISO/IEC 12207), maps every process
here to Automotive SPICE® (ASPICE) 4.0, and states what this framework
deliberately is and is not (a demonstrator, not a certified QMS — see its
"Scope and honesty" section).

## Layout

```
process/
  process-model.md          Read this first: QA vs verification, ASPICE map, DevOps
  roles.md                  Role catalog (V-model + engineering + supporting), RACI
  processes/                One file per ASPICE process this project runs
  strategies/                Cross-cutting strategies each process's tasks draw on
  work-products/             One content/quality/KPI spec per work product TYPE
  templates/                 Review checklists — human- or AI-executable
  CHANGELOG.md, VERSION      This framework's own release history
```

## Cross-project reference process

This project is intentionally the **reference implementation** for the shared
process model every other project in the organization is expected to inherit.
This is the **initial release baseline** of that standard: it is intentionally
versioned, documented, and maintainable so it can evolve continuously without
turning into undocumented drift. The process is not "finished" in the sense of
"no further change"; its design explicitly expects ongoing improvement and
requires process-erosion checks as a condition for later development.
The reference pattern is:

- feature backlog items are decomposed into requirements
- each requirement is linked to a design element and a verification case
- each requirement and design element has a responsible role and an evidence record
- safety-related items require a human final approver
- AI assistance is recorded and reviewed, but never allowed to be the final
  authorization step

This is the standard the project keeps for all of its own work, and the same
structure is what a downstream project copies or reuses via GitHub templates,
submodules or repository scaffolding.

## How the pieces connect

```
process-model.md ─┬─> processes/*.md      (WHAT gets done, by WHOM, per ASPICE base practice)
                   ├─> strategies/*.md     (HOW a class of task is carried out project-wide)
                   ├─> work-products/*.md  (WHAT "done" looks like for one artefact type)
                   ├─> templates/*.md      (the checklist a reviewer — human or AI — runs)
                   └─> cross-project reference model (feature -> requirement -> design -> verification -> approval)

processes/*.md  --produces/consumes-->  work-products/*.md   (machine-checked: see below)
roles.md        --assigns responsibility for-->  processes/*.md tasks
```

Every process names the work products it produces and consumes; every work
product names the process step that produces it. That link is not just prose:
`tools/check_process_docs.py` parses every file in `processes/` and
`work-products/` and fails if a named work product has no spec, a spec is
never referenced by any process, or a required section is missing — the same
"traceability is machine-checked or it rots" discipline the rest of this repo
already applies to REQ↔DES↔TS (`tools/trace_report.py`).

## Quality Assurance's report

`tools/qa_report.py` is QA's own tool: it inspects the repository's *current
state* (git history, `test-results/*.xml`, `analysis-results/`,
`docs/traceability.html`, `CHANGELOG.md`, the risk register, …) against what
`processes/*.md` says must exist, and writes
`downloads/TradingApp-qa-report.md` — a **CONFIRMED / NO EVIDENCE FOUND**
line per process, never a guess. Per `process-model.md`'s own scope statement,
it is **informational and does not block a release**, with one exception
project management accepted explicitly: **no current test-report evidence is
a hard failure**, because a release with no proof anything was tested is not
a case QA may stay silent about.

## Versioning

This framework has its own semantic version (`VERSION`) and changelog,
independent of the application. A process change is reviewed and released
like any other work product (see `processes/SUP.10-change-request-management.md`
and `processes/MAN.3-project-management.md`'s tailoring authority) — it is
not a side effect of an application commit.
