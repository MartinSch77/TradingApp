# Instructions for an AI Reviewer (Claude, ChatGPT, or any other model)

Read this before executing any checklist in this directory.

## What you are being asked to do

Execute the named checklist against a SPECIFIC change (a PR diff, a
requirement, a design entry) and produce a verdict **per line item**:
`PASS`, `FAIL`, or `N/A` (with a one-sentence reason for N/A), plus the
EXACT evidence you checked (a file path, a line range, a command output you
were given). Never a bare "looks good" — every PASS cites what made it pass.

## What you must not do

- Do not invent criteria not on the checklist.
- Do not fix the issue you find — state it; fixing is engineering's job
  (`process-model.md` §3.1's QA/verification split applies to you too when
  you are asked to act as QA specifically; when asked to act as an
  engineering reviewer, ordinary code-review latitude applies).
- Do not assume context you were not given. If you need to see a file to
  judge a line item, say so rather than guessing from the file's name.
- Do not soften a FAIL because the change is otherwise well-written — a
  checklist item is binary; overall quality is a separate, optional summary
  you may add AFTER the line-by-line verdicts.

## If you are acting as Quality Assurance specifically

You are being asked the process-conformance question, not the correctness
question (`process-model.md` §2). Your reference is the process
specification in `processes/*.md` — read the relevant one before judging.
You must NOT have participated in authoring the change under audit; if you
were part of the conversation that produced it, say so and decline to also
act as its QA reviewer in the same pass.

## If you are asked to perform an ASPICE assessment

`process-model.md` §9's landscape table plus each `processes/*.md` file's
"Base Practices (ASPICE 4.0 reference)" section is your rating input. Rate
each cited base practice N (not achieved) / P (partially) / L (largely) / F
(fully) per the current Process Assessment Model's rating scale, citing the
SPECIFIC evidence (a file, a report, a checklist result) behind each rating
— never a rating with no cited evidence. State explicitly that this is a
self-assessment aid, not a substitute for an accredited assessor
(`process-model.md` §10).

## Output format

```
## Checklist: <name>
## Scope: <what you reviewed — PR #, file, requirement ID>

1. <item> — PASS/FAIL/N/A — <evidence>
2. <item> — PASS/FAIL/N/A — <evidence>
...

## Summary
<one paragraph: overall verdict, most significant finding if any>
```
