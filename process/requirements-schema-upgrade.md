# Requirements Schema Upgrade — record and atomization

Real record, not illustrative — tracks a genuine SUP.10 change against
`requirements/requirements.sdoc`. Both atomization passes it planned have
landed (GitHub issue #14).

## What landed first: the schema

`requirements/requirements.sdoc`'s `[GRAMMAR]` gained six OPTIONAL fields, so
every existing requirement stays valid unchanged:

- `SOURCE` — the stakeholder or measured finding behind the requirement.
- `RATIONALE` — why, distinct from what.
- `PRIORITY` — must / should / could.
- `STATUS` — `proposed | analyzed | approved | implemented | verified |
  superseded`.
- `ACCEPTANCE_CRITERIA` — the observable condition for `STATUS: verified`.
- `ISSUE` — the GitHub Issue/PR of record.

Verified via `strictdoc export` (no parse errors) and
`tools/trace_report.py` (0 hard gaps, unchanged) both before and after.

## Why the atomization was deferred at first (history)

REQ-F-034 and REQ-F-035 were each a single `STATEMENT` bundling roughly a
dozen and ten independently verifiable obligations respectively — a textbook
non-atomic requirement. Splitting them correctly required re-classifying
every existing `@relation(REQ-F-034…)`/`@relation(REQ-F-035…)` tag across
~15 and ~23 test-function references (respectively) plus their
`docs/design.md` `satisfies` entries onto the CORRECT new atomic id — work
that needs careful, individual reading of each test to avoid the opposite
defect: a traceability link that is now WRONG rather than merely coarse.

Rather than rush that reclassification, both requirements were first marked
`STATUS: proposed` with a `RATIONALE` stating the bundling explicitly, and
this file recorded the scoped plan. That was the intended behavior of
`processes/SUP.1-quality-assurance.md`'s independence principle applied to
self-review: an honest "not done yet, tracked" beats a rushed "done" that a
QA audit would later have to un-confirm.

## The atomization — both splits have landed (GitHub issue #14)

Tool support came in between the two passes: `tools/sdoc_to_md.py` marks a
`STATUS: superseded` requirement in the generated table and names its
successors (every REQ id its `ACCEPTANCE_CRITERIA` cites), and
`tools/trace_report.py` traces a superseded requirement THROUGH those
successors — no longer an open coverage gap, but a HARD gap if it names no
successor or an unknown one, and a HARD gap if any test still cites it.

### First pass — REQ-F-034 → REQ-F-049..REQ-F-058 (PR #21)

REQ-F-049 churn control (cooldown, pace limit, minimum hold) · REQ-F-050
reversal conviction gate · REQ-F-051 reversal economics gate · REQ-F-052
session-phase sit-out windows · REQ-F-053 reluctant/peripheral symbols ·
REQ-F-054 exit record by rule · REQ-F-055 per-bucket leverage cap ·
REQ-F-056 session-structure reads · REQ-F-057 local model shown as a source ·
REQ-F-058 leverage caps before the ladder fold. The three suspected
duplicates (REQ-F-031, REQ-F-022, REQ-F-030) were checked against their
STATEMENTs and none was real — recorded in REQ-F-055/-056/-057's RATIONALE.
REQ-F-057's initial coverage gap was closed later by TS-PM-006.

### Second pass — REQ-F-035 → REQ-F-059..REQ-F-075

The nine reads — REQ-F-059 futures leadership · REQ-F-060 futures push over
several horizons, counted as ONE read · REQ-F-061 volatility direction ·
REQ-F-062 US 10-year yield · REQ-F-063 curve shape, naming its front-end
source · REQ-F-064 heavyweight participation, named a stand-in for breadth ·
REQ-F-065 heavyweights above their own session VWAP · REQ-F-066 up/down
volume · REQ-F-067 opening-range position as a counted read. The gate —
REQ-F-069 the agreement count, shown, given to the model and gating the bot.
The two displays — REQ-F-074 the constituents in their own view ·
REQ-F-075 the cap-weighted constituent lead.

Seventeen rather than the planned twelve, because the cross-cutting
obligations were decided one by one (each successor's RATIONALE records the
decision) and five of them constrain several reads, or a layer below them,
rather than one read: REQ-F-068 measurability (UNKNOWN never counts as
agreement) · REQ-F-070 read inputs keyed as their sources are keyed ·
REQ-F-071 each index read from its own constituent list, named · REQ-F-072
volume bars aligned bar for bar, UNKNOWN without volume · REQ-F-073 the
volatility term structure, a conviction damper and never a direction. Two
stayed with the one read they constrain: the STAND-IN rule with
participation (REQ-F-064) and the "one read, not three" rule with the
futures push (REQ-F-060).

Where the plan was wrong or loose, the split says so rather than following it:

- "volume-delta" is not what REQ-F-035 states and not something this app can
  measure (order flow needs level-2 data); REQ-F-066 is named for what it
  is, the up/down volume split across the heavyweights.
- "majority-agreement gate": REQ-F-069 keeps REQ-F-035's own wording (a
  "configured number" of the measured reads) and records the implemented
  majority rule in SOURCE instead of promoting it — a split must not add
  obligations. REQ-F-035's SOURCE credited the majority regression to
  TS-PAPER-025; its assertions are in TS-PAPER-027.
- The two displays are NOT REQ-F-038 territory (the cockpit never mentions
  the constituents) and not duplicates of REQ-F-036 either (its heavyweight
  GRAPH and its equal-weight input are different obligations).
- The opening-range read is not a duplicate of REQ-F-022, and only PARTLY
  one of REQ-F-056 (same `openingRange` computation): REQ-F-067 keeps only
  what REQ-F-056 lacks, the read's role as a counted vote. The futures
  leadership read likewise shares REQ-F-056's `relativeStrength`
  measurement but not its obligation (REQ-F-059).

Re-tagging: all 22 `@relation(REQ-F-035, …)` markers in `tests/*.cpp` were
moved onto the successors each test actually verifies, after reading its
body; TS-PAPER-026 lost the tag outright (its body holds no confluence
assertion — TS-PAPER-027 does), and TS-LEAD-003 gained REQ-F-073 because it
is the test that shows an inverted term structure reducing conviction.
One constraint shaped the tags: StrictDoc (0.30.1, measured with a probe)
honours only ONE `@relation` marker per test function — a second marker
silently drops the first — so every id must fit one line inside the
100-column format limit; TS-CONF-002 therefore carries six ids and not
REQ-F-068, which TS-CONF-003 pins through the count anyway. The
`docs/test_spec.md` sentences of the re-tagged tests were re-read against
their assertions (`templates/review-checklist-test.md` item 3) and eight
were corrected: TS-CONF-001 (nineteen tickers, not fifteen), TS-CONF-002,
TS-CONF-003 (nine unknowns, not five), TS-CONF-005, TS-DEC-010,
TS-LEAD-003, TS-PAPER-026 and TS-PAPER-027.

Two findings the split surfaced and did NOT paper over — both requirements
stay at `STATUS: approved` with the unmet clause named in their RATIONALE:
REQ-F-064's stand-in label is missing from the participation read's own
detail text (which reaches the model's evidence and the cockpit meter), and
REQ-F-075's "approximate static weights" label is not shown anywhere the
cap-weighted lead is.

## Acceptance for closing the tracked issue — met

Both `STATUS` fields are `superseded`, naming their successor ids;
`tools/trace_report.py` reports 0 hard gaps and no new "no automated test"
gap for any successor; every re-tagged test's `docs/test_spec.md` sentence
was re-verified against its actual assertions (not just carried over) per
`templates/review-checklist-test.md` item 3.
