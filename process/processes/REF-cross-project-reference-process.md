# REF — Cross-project Reference Process

## Purpose

Define the shared process model that every project must inherit when it uses
this reference repository as its process baseline. This process creates the
common feature-to-requirement-to-design-to-verification-to-approval flow used
across all projects.

## Inputs

- Feature requests, stories, and change proposals.
- Baseline process documents from this repository.
- Project-specific implementation work products.

## Outputs / Work Products

- Feature record using `templates/feature-template.md`
- Requirement record using `templates/requirement-template.md`
- Design element record using `templates/design-element-template.md`
- Verification record using `templates/verification-template.md`
- Release approval record using `templates/release-approval-template.md`
- AI assistance record using `templates/ai-assistance-record-template.md`
- Project template record using `templates/github-project-template.md`
- Traceability report using `work-products/traceability-matrix.md`

## Tasks

1. Capture each new work item as a feature with a stable feature ID using
   `templates/feature-template.md`.
2. Decompose each feature into one or more requirements using
   `templates/requirement-template.md`.
3. Link each requirement to the relevant design elements and implementation via
   `templates/design-element-template.md`.
4. Define a verification method and evidence path for each requirement using
   `templates/verification-template.md`.
5. Record AI assistance and require human review before the item is accepted using
   `templates/ai-assistance-record-template.md`.
6. Require independent QA confirmation for process-critical and safety-relevant
   items.
7. Require a final human release approval for any item that is safety-relevant
   or release-critical using `templates/release-approval-template.md`.
8. Rebuild and validate the traceability matrix before release approval.
9. Use `templates/github-project-template.md` to onboard any downstream project
   reusing this process model.

## Roles

- Feature Owner — accountable for feature definition.
- Requirements Engineer — accountable for requirement quality and traceability.
- Software Architect — accountable for design-element mapping.
- Verification Engineer — accountable for validation evidence.
- Quality Assurance — verifies process compliance and independence.
- Safety Responsible / Qualified Human — confirms safety-related conclusions.
- Final Approver — human decision-maker for release or closure.

## Base Practices (ASPICE 4.0 reference)

This process supports traceability, confirmation, and release governance in the
same spirit as SWE.1, SWE.2, SWE.3, SWE.4, SWE.6, SUP.1, and REL-product-release.

## Verification / QA Hooks

QA confirms that every feature record has a linked requirement, every requirement
has a linked design element and verification record, and every safety-relevant
item has documented human approval. AI-generated content is accepted only if a
human review and final approval record exist.
