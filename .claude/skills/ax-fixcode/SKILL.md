---
name: ax-fixcode
description: >-
  One command to (1) fix all Axivion style violations (SVs) in the currently
  marked / selected code lines AND (2) refactor that code to remove unneeded
  complexity - extract duplicated logic into a shared function, flatten nesting,
  drop redundant branches - in ANY Axivion-analyzed project (C or C++; any
  configured standard: MISRA C / MISRA C++ / CERT C / CERT C++ / AUTOSAR / CWE /
  CUDA). Driven by the axdashboard + axdocumentation MCP servers. Use whenever
  the user runs /ax-fixcode, or asks to fix the Axivion SVs / findings in the
  marked or highlighted lines, clean up / simplify / de-duplicate the selected
  code, or reduce complexity in a specific line range or the current IDE
  selection. Scopes to what the user marked - a line range, or the whole file
  when the entire file is selected - and falls back to the whole project (or a
  named folder) when nothing is selected. Applies behavior-preserving fixes and
  refactors, then re-verifies against a fresh analysis. Project-agnostic -
  discovers the project's language, standard, conventions and re-run flow instead
  of assuming them.
allowed-tools: >-
  Read, Edit, Write, Grep, Glob, Bash, TodoWrite,
  mcp__axdashboard__get_connection_info, mcp__axdashboard__get_current_project_name,
  mcp__axdashboard__get_analyzed_projects, mcp__axdashboard__select_database,
  mcp__axdashboard__list_available_databases, mcp__axdashboard__list_database_standards,
  mcp__axdashboard__get_findings, mcp__axdashboard__get_findings_overview,
  mcp__axdashboard__get_top_violated_rules, mcp__axdashboard__get_top_violating_files,
  mcp__axdashboard__get_statistics, mcp__axdashboard__get_statistics_filtered,
  mcp__axdashboard__get_project_rules, mcp__axdashboard__get_suppression_guidance,
  mcp__axdashboard__identify_rule_standard,
  mcp__axdocumentation__search_axivion_documentation
---

# Ax Fix Code - fix SVs + simplify the marked lines

`/ax-fixcode` does **two jobs** on the code the user has marked:

1. **Fix** every Axivion **style violation (SV)** in the marked lines.
2. **Refactor** that code to remove **unneeded complexity** - move similar /
   duplicated parts into a shared function, flatten nesting, delete redundant
   branches, name repeated literals.

Both jobs are **strictly behavior-preserving**, and scoped to what the user
marked - a line range, a whole file, or (when nothing is selected) the whole
project or a named folder (see Step 0).

This skill is **project-agnostic**: it works for any Axivion-analyzed C or C++
project and any configured standard. **Discover** the project's language,
standard(s), conventions and re-run flow (Step 1) - never hardcode or assume
them. If the project ships its own Axivion skill or fix-pattern notes (e.g. a
project-specific `axivion-review`), consult it for verified, project-tuned
patterns.

## Ground rules (from the MCP server instructions - do not break these)

- **Axivion MCP data is authoritative.** A finding exists only if a
  `get_findings*` call returned it. Never invent findings or rules.
- **A rule's meaning comes only from `search_axivion_documentation` /
  `get_project_rules`** - never from memory. Quote the doc when you explain.
- **Never edit the analyzer's configuration directly** (rule / CI / compiler
  config, usually JSON under an `axivion/` dir). Query and reason about it via
  MCP tools.
- **Behavior-preserving only - for fixes AND refactors.** Confirm scope before
  adding suppressions or making a large structural refactor.
- **Always explain the rationale** for every change.

## What "SV" means here
`SV` = **Style Violation** = the coding-standard rule findings, whatever
standard the project runs (`MisraC2023-*`, `MisraC++2023-*`, `CertC-*`,
`CertC++-*`, `AutosarC++*`, `CWE-*`, `CUDA-*`, ...). Those are what `get_findings`
returns. The other categories - **MV** (metrics), **CL** (clones), **CY**
(cycles), **AV** (architecture), **DE** (dead code) - aren't the direct target,
but note that **Job 2's refactoring tends to clear MV and CL** (complexity,
length, nesting, duplicate code) as a bonus.

---

## Step 0 - Scope: what to work on

The scope comes from the **editor selection** (delivered in context as an
`ide_selection` block with a file path + line range) or from what the user
names. Resolve it in this order and **echo the resolved scope back** so the user
can confirm:

1. **A partial line range is selected** -> work on **exactly those marked
   lines** (the precise default mode). *"Fixing + simplifying `<file>` lines
   `<a>`-`<b>`."*
2. **A whole file is selected** (the selection spans the entire file - first
   line through end-of-file) -> work on the **whole file**: query findings for
   the file with no line bounds and refactor across it. *"Fixing + simplifying
   the whole file `<file>`."* To tell this from a partial selection, compare the
   selection's range to the file's line count (e.g. `wc -l <file>`) - a
   selection running from the first line to the last is a whole-file selection.
3. **No selection, but the user named a file / range / folder** -> use that. A
   folder or glob means that whole file set.
4. **No selection and no file at all** -> **the whole project (or the current
   folder)**: consider **all source files**. This is a large, many-file
   operation, so **confirm the breadth first**, then work **file by file in
   priority order** (mandatory -> required -> high -> the rest), triaging with
   `get_findings_overview` / `get_top_violating_files` before editing.

Track the work with `TodoWrite` - a per-target list for line / whole-file scope,
a per-file list for project / folder scope.

## Step 1 - Orient + detect the project (do NOT assume)

- `get_connection_info` + `get_current_project_name` -> mode, project, and which
  DB is being read.
- **Standard(s):** `list_database_standards` / `get_project_rules`; use
  `identify_rule_standard(rule_id)` when unsure which standard a rule belongs to.
- **Language & conventions:** infer from the file extension, and **read
  neighbouring code** to learn the project's own conventions - fixed-width
  typedefs vs base types, naming, brace style, and how internal-linkage helpers
  are written (`static` in C, unnamed namespace / `static` in C++). Match them;
  never impose foreign conventions.
- **Re-run flow (needed for Step 8):** detect how this project re-analyzes - a
  script (e.g. under `axivion/`, a `*.sh`, an `axivion_ci` / dashserver call), a
  build/CMake target, or **dashboard / CI-only**. If it's dashboard-only and you
  can't trigger it, say so up front.
- **Freshness trap:** the `axdashboard` MCP serves a **snapshot from when it
  started**, not the live tree. If a newer analysis DB exists, `select_database`
  it before trusting counts (`list_available_databases`). Verify suppression /
  fix effects by the finding **count**, not solely by `get_suppression_guidance`.

## Step 2 - Pull the findings for the resolved scope

Query the scope from Step 0 - don't exceed it:
- **Line range:** `get_findings(file_pattern="<basename>", start_line=<a>,
  end_line=<b>, rule_pattern="*", severity_pattern="*", include_statistics=true)`
- **Whole file:** the same call **without** `start_line` / `end_line`.
- **Project / folder:** triage with `get_findings_overview` /
  `get_top_violating_files` first, then pull per file (`file_pattern="<file>"` or
  a folder glob) and handle **one file at a time**.

Group results by rule. If a scope returns **0 findings**, say so - but still do
**Job 2** (refactor) where the code is complex or duplicated.

## Step 3 - Read the code: findings + complexity

`Read` the selection and enough surrounding context to see both:
- the real construct behind each finding, and
- **complexity to reduce**: duplicated / near-duplicated blocks, deep nesting,
  over-long functions, redundant or dead branches, repeated magic literals.

## Step 4 - Explain each rule

For each distinct rule: `search_axivion_documentation("<rule id> <topic>")` for
the official intent **and the compliant fix**; quote it briefly.

## Step 5 - Plan (behavior-preserving)

**Fixes - classify each finding fix vs suppress:**
- **Code-fixable** -> a behavior-preserving change lets the analyzer prove the
  property. Portable examples (confirm intent in the docs, match project
  conventions): `const`-qualify unmodified params/vars; use the project's
  fixed-width typedefs instead of bare base types; add braces to single-statement
  `if`/`else`/loop bodies; parenthesize the sub-expression the checker flags;
  name magic literals; keep comments in the basic source character set; keep a
  declaration and its definition in sync.
- **Suppression-only tail** -> no code change wins, or the "fix" adds *more*
  findings / dead code / hits a rule conflict. Leave or suppress (Step 7).

**Refactors - remove unneeded complexity:**
- Extract duplicated / similar blocks into **one well-named helper** with the
  project's linkage + type + style conventions (so it spawns no new findings).
- Flatten nesting, use guard clauses / early returns where idiomatic, delete
  redundant branches, hoist repeated literals into named constants.
- Prefer changes that also lower **metric violations** (function length,
  cyclomatic complexity, nesting depth, parameter count) and **clones**.

**CRITICAL equivalence caveat (both jobs):** "duplicate" blocks are often NOT
identical - watch for swapped operand/guard order, off-by-one, or a differing
early exit. **Prove** the difference is immaterial under the code's actual
contract/inputs, or **preserve** it. **Surface any asymmetry to the user** rather
than silently unifying. Never change observable behavior to satisfy a rule or to
de-duplicate.

## Step 6 - Apply

Make the smallest coherent set of edits covering both jobs. Keep header/`.c` (or
class declaration/definition) **in sync** so you don't trade one finding for a
signature-mismatch rule. Edits shift line numbers - you'll re-scope by
re-querying after the re-run.

## Step 7 - Suppress the residual tail (only with go-ahead)

Suppressions are **real config changes**. Apply all code fixes + refactors first,
then **list the remaining suppression-only findings** (rule, line, why no fix
wins) and **ask for the go-ahead** before writing a batch. Use
`get_suppression_guidance` for the project's **active** comment formats/scopes;
prefer the **narrowest scope** that covers the marked region (line-scoped over
file-wide). Every suppression needs a real justification.

## Step 8 - Re-run & verify

- Re-run via the flow detected in Step 1 (force a **clean build** if incremental
  builds skip re-analysis). If dashboard/CI-only, ask the user to trigger it,
  then reconnect / `select_database` the fresh DB.
- **Re-query** the same scope (re-scoped, since edits shift line numbers) and
  confirm:
  - the SV count dropped by the expected amount;
  - **no new findings appeared anywhere** (a fix/refactor can spawn a finding
    elsewhere - a quick `get_findings_overview` catches it);
  - **metric violations did not worsen** (the refactor should hold or improve
    them).
- **Behavior:** keep the build clean; run the project's tests if it has any.

---

## Reporting format

- **Scope:** file + marked line range; SVs before -> after.
- **Fixes:** rule, line, doc-backed reason, action (fixed / suppressed / left).
- **Refactors:** what was extracted/simplified, and **why it's behavior-
  preserving** (note any asymmetry you resolved).
- **New findings introduced:** list them, or "none". **Metrics:** better /
  unchanged.
- **Remaining tail:** suppression-only findings still open and why.

**Done** = the SV count in the marked range dropped as expected, no new findings
anywhere, metrics no worse, the build (and tests) still green, behavior unchanged.
