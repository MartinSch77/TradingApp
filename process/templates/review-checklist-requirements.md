# Review Checklist: Requirements

Read `templates/ai-reviewer-instructions.md` first. Scope: one requirement
(or one PR's worth of added/changed requirements) in
`requirements/requirements.sdoc`.

1. States exactly ONE principal obligation (no "and" joining two
   independently verifiable demands).
2. `UID` is new (not reused) and follows the `REQ-F-xxx`/`REQ-N-xxx` scheme.
3. `SOURCE` names a real stakeholder, incident, or measured finding — not
   "self-evident" or blank.
4. `RATIONALE` explains WHY, distinct from the `STATEMENT`'s WHAT.
5. `PRIORITY` is set (must/should/could).
6. `VERIFICATION` is set to T, A, I, or a combination, and matches how the
   requirement will actually be checked (a requirement demanding an
   observable, testable behavior should not be marked `I`-only).
7. `ACCEPTANCE_CRITERIA` states an observable condition, not a restatement
   of the requirement itself.
8. `STATUS` is `proposed` (new) or correctly advanced with evidence for the
   transition (e.g. `verified` only once a passing test exists).
9. `ISSUE` links the GitHub Issue/PR that introduced or changed it.
10. No forward reference to HOW it will be implemented (that belongs in
    design).
11. If this requirement supersedes another, the superseded one's `STATUS` is
    set to `superseded` and names the successor, rather than being deleted.
