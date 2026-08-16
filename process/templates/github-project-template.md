# GitHub Project Template

## Purpose
Use this template for every downstream project based on the TradingApp reference process.

## Required project structure
- `requirements/`
- `design/`
- `verification/`
- `evidence/`
- `process/`
- `.github/`
- `templates/`

## Mandatory project rules
- Every feature has a parent requirement.
- Every requirement links to at least one design element.
- Every requirement has at least one verification record.
- Every safety-relevant item has a human approver.
- Every AI-assisted artifact has an AI assistance record and human review.
- Every release requires a human final approval.

## Minimum GitHub setup
- issue templates for features and requirements
- pull request template
- CODEOWNERS
- branch protection for main
- required reviews for release branches
- CI checks for traceability and evidence presence
