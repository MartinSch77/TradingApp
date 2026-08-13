# Work Product: Project Plan

**Produced by:** MAN.3. **Owning role:** Project Manager.
**Location:** `docs/roadmap.md` (forward plan) + `CHANGELOG.md`/git tags
(realized history) + GitHub Milestones (scheduling instance).

## Content rules

The roadmap states planned work ORDERED BY COST (its own stated principle,
"a feature list ranked by wow effect is not a plan"), each item's measured
prerequisite state (what the toolchain can/cannot do TODAY, checked, not
assumed). A milestone groups issues toward one roadmap item and closes only
when every issue in it reaches `state:closed`.

## Quality criteria

No planned item claims availability of a capability that was not actually
measured (`docs/roadmap.md`'s own "Measured, not assumed" discipline).

## Review requirement

`templates/review-checklist-process-compliance.md`'s planning section, each
QA cycle.

## KPIs

Milestone completion rate; count of roadmap items whose "measured" claim was
later found stale (a `PIM.3` trigger if it recurs).

## Traceability

Roadmap item → GitHub Milestone → issues/PRs → the requirements/design/tests
those issues touch.
