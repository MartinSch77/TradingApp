# Case study: Squish for Qt

@page cssquish Case study: Squish for Qt

**The claim being tested:** unit tests pass on code that cannot be used. A GUI test
notices.

| | |
|---|---|
| Tool | Squish for Qt 9.2.2 |
| Scope | 7 scenarios, 35 verifications, driving the real built application |
| Config | `squish/suite_gui/` — `suite.conf`, `envvars`, `shared/scripts/names.py` |
| Runner | `tools/squish_run.sh` / `.ps1`, or `./build_all.sh gui` |
| Verdict | green; licence-bound, so it reports `skipped` (exit 3) where no licence exists |

## The problem

The suite existed before Squish did. It was written from the documentation, which
means it was written against an application nobody had actually driven. That is a
comfortable state to be in and a false one: a GUI test that has never run is a
description of intent, not evidence.

There is also a safety problem specific to this project. The application trades real
money through the eToro API. A GUI test that clicked BUY on a live account would be
the single worst defect this repository could ship, so "the tests are green" is not
enough — the tests must be *unable* to reach a real account.

## The configuration

Two decisions carry the setup.

**A GUI run cannot reach real money.** `TRADINGAPP_FORCE_SIMULATION` makes
`Config::hasCredentials()` answer false at the ONE place every mode question reads,
and `squish/suite_gui/envvars` sets it for every run. It is pinned by a unit test
(TS-CFG-007) rather than trusted, because the safety property is more important than
the feature it guards.

**Every widget is addressable by name.** The object map addresses by `objectName`
only — never by position, index or caption, all of which change when a layout does.
`tools/check_object_names.py` runs in the analysis stage and fails the build if any
widget in `src/ui` lacks a stable `objectName` (REQ-N-007), so the map cannot rot
silently as the UI grows.

## What the first real run found

Two configuration defects, on the first attempt, neither of which any amount of
re-reading would have produced:

1. **The scripted object map has to live under the *suite's* `shared/scripts`.** It
   was in a sibling directory, so `import names` simply failed. The path looks
   correct until the moment a runner resolves it.
2. **The map addressed a `modeBadge` that the application has never had.** The widget
   is `modeLabel`. A name invented while writing a test from documentation, and
   nothing but execution can tell you the difference.

A third finding came from the test being written backwards: the signals window is
opened by the *application* at startup, so the toggle button **closes** it. The test
asserted the opposite and failed for exactly that reason — which is the test doing
its job on the tester.

## The correction

The map moved under the suite, `modeBadge` became `modeLabel`, and the toggle test
was inverted to match what the application actually does. Both tool install locations
became parameters (`--squish-dir`, environment fallback, then discovery of
`~/squish-for-qt-*`) because these tools live wherever their owner put them, and a
hard-coded path is a machine-specific absolute path by another name.

## The measurable result

Seven scenarios, 35 verifications, green:

- startup is SIMULATION, and every window opens
- the double-press gate: one press does **not** open a position, the second does
- the bot window states its account, its live-readiness verdict and its books
- the decision window names its sources and survives a refresh
- the heavyweights window shows its early read
- the lead signal states its evidence
- the live gate refuses on an empty record

The last two are the interesting ones for a reviewer: they assert that the
application *refuses* — that the live-money gate says no when the record is empty,
and that a combined signal reports how much of its evidence was measurable. Those are
claims about honesty, and they are checked against the running program.

## What it does not prove

Seven scenarios are not a UI test suite; they are the load-bearing paths. GUI coverage
is therefore measured **separately** from the unit figure (see
[the Coco case study](coco.md)) precisely so that a well-covered domain cannot hide an
untested interface behind one blended number.

## Source

- Configuration: `squish/suite_gui/`
- Runner: `tools/squish_run.sh`, `tools/squish_run.ps1`
- Safety property: `TRADINGAPP_FORCE_SIMULATION`, pinned by TS-CFG-007
- Name gate: `tools/check_object_names.py`
- Commits: `7235753` (the two configuration defects, the suite made real),
  `96fc6ca` (GUI tests that cannot reach real money)
- How to obtain and install the tool: [docs/qt-tools.md](../qt-tools.md)
