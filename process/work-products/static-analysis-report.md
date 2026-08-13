# Work Product: Static Analysis Report

**Produced by:** SWE.4. **Owning role:** Verification Engineer (Unit).
**Location:** `analysis-results/*` (per-tool text/CSV), the Axivion
dashboard (MISRA C++ 2023 + CERT/CWE + architecture), `analysis-results/
tool-versions.json` (tool identity record).

## Content rules

One file/section per tool (cppcheck, clang-tidy, Clang Static Analyzer,
g++ `-fanalyzer`, clazy, lizard, PMD CPD, codespell, qmllint) plus the Axivion
dashboard's own findings; `tool-versions.json` records exactly which tool
version produced the report, so a "0 findings" claim is reproducible.

## Quality criteria

A suppressed/disabled check carries a written reason and a measured hit
count in `.clang-tidy`/`tools/cppcheck-suppressions.txt`
(`CLAUDE.md`'s non-negotiable) — a suppression with no such record is treated
as a defect in THIS work product, independent of whether the underlying code
is actually fine.

## Review requirement

Any NEW suppression added in a change is reviewed against
`templates/review-checklist-code.md`'s suppression-justification item.

## KPIs

Total finding count (target 0 at merge, per `CLAUDE.md`'s release gate);
count of suppressions lacking a written reason (target 0, checked by
`tools/static_analysis.sh` itself failing the build on an undocumented one).

## Traceability

Finding ↔ file/line ↔ (if fixed) the commit that resolved it; (if
suppressed) the suppression's own justification comment.
