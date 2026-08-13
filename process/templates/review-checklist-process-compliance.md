# Review Checklist: Process Compliance (QA's own instrument)

Read `templates/ai-reviewer-instructions.md` first, especially "If you are
acting as Quality Assurance specifically." Scope: one process from
`process-model.md`'s landscape table, for one audit cycle.

1. Read the process's own file in `processes/` — this IS the reference; do
   not audit against habit or memory of what the pipeline "usually does."
2. For each Task listed in the process file, find the evidence it should
   have left (a file, a report, a commit, a review record) and cite it.
3. Mark the process `CONFIRMED` only if every Task's evidence was found;
   `PARTIAL` and name exactly which Task's evidence was missing; `NOT FOUND`
   if no evidence exists for most/all Tasks.
4. Check the Role(s) responsible per `roles.md` actually match who the
   evidence shows did the work (a task done by the wrong role is a finding
   even if the output looks fine).
5. For SUP.1 itself (auditing QA's own conformance — used by `PIM.3`'s
   effectiveness lens, not a second QA layer): confirm every process in the
   landscape appeared in the last QA report, and that no verdict was
   asserted without cited evidence.
6. File every NOT FOUND/PARTIAL into the risk register the same day
   (`processes/SUP.1` Task 5) — this checklist is not complete until that
   filing is done, not merely noted.
