# Using AI code generation with a safety net

@page aiassisted Using AI code generation with a safety net

Most of this repository's code was written with an AI coding assistant. That is not the
interesting part — by 2026 it is unremarkable. The interesting part is the question it
raises, which nobody gets to skip:

> **How do you know it is right?**

This project's answer is not "the model is good". It is a harness that makes every change
*checkable*, *traceable* and *reversible*, and that produces evidence a reviewer can
disagree with. What follows is what that harness is, what it demonstrably catches — and,
in the last section, what it demonstrably does **not**, with real examples from this
repository. The honest limits are the reason the rest is worth reading.

## What the harness is

| Layer | Mechanism | What it gives a reviewer |
|---|---|---|
| **Intent is written down first** | requirements live only in `requirements/requirements.sdoc` (StrictDoc); `docs/requirements.md` is generated | you can read what the code was *supposed* to do without reading the code, and a requirement cannot be quietly reinterpreted by an edit |
| **Traceability is a gate, not a report** | every test carries `@tstid` / `@design` / `@relation(REQ-…)`; `tools/trace_report.py` fails on hard gaps | a new behaviour that nothing verifies fails the build; the requirement→design→test→result chain is machine-checked |
| **Architecture is declared and verified** | `axivion/architecture.py` (intended architecture as code) + linker-enforced layering in `CMakeLists.txt` | an illegal dependency fails the *build*; a drift from the intended model raises an architecture violation. See [Why the architecture check is Axivion's job](tools.md) |
| **Many analyzers, not one** | cppcheck, clang-tidy, Clang Static Analyzer, `g++ -fanalyzer`, clazy, Axivion (MISRA C++ 2023), Coverity, CodeQL; SonarCloud informational | tools disagree, and the disagreements are where the interesting defects are. Each disabled check carries a written reason **and** the measured hit count that justifies it |
| **Undefined behaviour is hunted at runtime** | ASan+UBSan, TSan, valgrind | the classes of defect static analysis structurally cannot see |
| **Coverage is measured at decision level** | MC/DC via Squish Coco and clang, reported per suite; GUI coverage measured and reported **separately** | "the line ran" is not "the condition combination was tested", and a well-covered domain cannot hide an untested UI |
| **The real application is driven** | Squish GUI workflows against the built app | unit tests pass on code that cannot be used; a GUI test notices |
| **Complexity is ratcheted** | `tools/lizard_metrics.py` against a baseline of existing debt | an assistant cannot quietly grow a 300-line function: new debt, a worsened number, or a stale entry all fail |
| **Duplication is gated** | PMD CPD (≥100 tokens) | the most common failure mode of generated code — the same logic pasted with a variation — fails the build instead of accumulating |
| **Evidence is required to release** | `tools/publish_release.sh` refuses unless tests are green, the analyzers are at zero, the ratchet is clean, there are no hard gaps, and every artefact is newer than the newest source | you cannot ship a claim that the artefacts do not support |
| **Reporting is reproducible** | `tools/make_test_report.sh` → `downloads/TradingApp-quality-report.pdf`; results also pushed to Squish Test Center | one command regenerates the whole evidence set, so a reviewer can *re-derive* it rather than trust it |
| **Everything is under review discipline** | git, issue templates, `CONTRIBUTING`/`SECURITY`, Dependabot, four-platform release workflow | ordinary software-engineering hygiene, which AI assistance makes more important rather than less |

Two properties of the code itself do more for reviewability than any tool:

- **Comments explain *why*, and carry the measured number.** Where a threshold exists, the
  measurement that produced it is written next to it (`3 USDOLLAR trades for −19.22 EUR`;
  `median hold 5.2 min, gross +1.64 EUR against 19.38 EUR of costs`). A constant with its
  evidence attached can be argued with; a bare constant can only be trusted.
- **Unmeasurable is a distinct state from zero.** Throughout the signal code an input that
  could not be read is `UNKNOWN` and never counts as agreement. This is the single
  discipline that most often turns a plausible-looking generated aggregate into an honest
  one.

## Domain-specific safety, because this one moves money

Generic quality tooling does not make a trading application safe. These do:

- **Simulation is the default**: without credentials the app cannot trade, and
  `TRADINGAPP_FORCE_SIMULATION` forces that state at the one place every mode question is
  asked — so a GUI test run **cannot** reach a real account (pinned by a unit test).
- **Advisory features never trade.** The local-LLM advisor supplies a direction only; it
  can never exceed the stake, exposure, leverage or ruin limits, and silence from the model
  can never close a position.
- **Money-moving actions need explicit human confirmation**, and the live-order path is
  separately validated, armed with expiring caps, kill-switched and audit-logged.
- **Money is integer minor units**, not floating point, and mixed currencies yield an
  amount that reports itself *invalid* rather than a plausible wrong number.
- **A probability must be measured, not asserted.** Forecast quality is scored against
  baselines on identical samples, and the app says `UNCALIBRATED` rather than quoting a
  number the record cannot support.

## What this does **not** prove

This is the section that decides whether the rest is credible.

**None of it proves the code is correct.** It catches classes of defect. Two real examples
from this repository, both of which the full pipeline passed:

1. **A dead signal that every test agreed was alive.** The futures-lead read — the most
   immediate directional input the app has — looked up the futures proxies in the wrong one
   of two look-up tables (references are keyed by ticker, instruments by the app's own
   symbol). It was therefore permanently "unmeasurable" in the running application, and its
   unit test passed *because the test filed the data in the same wrong table*. Analyzers,
   sanitizers, MC/DC coverage and the traceability gate were all green. It was found by
   reading the data flow. The fix made the mistake unrepresentable (one factory, named
   fields) rather than merely correcting the lookup — and a regression test now pins it.
2. **A gate that silently loosened as the code improved.** The bot required "at least 3 of
   the independent reads agree". When the number of reads grew from five to nine, that
   constant went from being a *majority* to a *minority* — the main protection weakened
   with no failing test, because no test tied the threshold to the number of reads. The fix
   makes the bar track what was actually measured.

Both are the same failure mode, and it is the one to expect from AI-assisted work: **code
that is locally correct, internally consistent, and wrong about its own context.** Tests
written alongside the code inherit its assumptions. That is why the pipeline is necessary
and not sufficient.

Other limits, stated plainly:

- The Axivion MISRA C++ 2023 analysis reports a large number of open style violations,
  dominated by a handful of rule classes whose applicability to a Qt desktop application
  has to be ruled on rather than mechanically "fixed". They are open, counted, and not
  hidden.
- Some requirements have no automated test and are verified by inspection; the
  traceability report names which, rather than omitting them.
- The metrics gate is a **ratchet over existing debt**, not a clean bill of health: the
  over-threshold functions are recorded with their numbers.
- Coverage measures what was executed, not whether the assertions would have noticed a
  wrong answer. Mutation testing would answer that, and is not yet here.
- The strategy's profitability is explicitly **not** claimed. The record is measured after
  costs, decomposed by exit rule, compared against naive baselines, and reported with its
  own unmet thresholds.

## The honest summary

AI assistance made this codebase larger and faster to build than it would otherwise have
been. It did not make it trustworthy. What makes a change trustworthy here is that it must
survive a requirement, a traceable test, nine analyzers, three sanitizers, decision-level
coverage, a GUI run against the real application, a complexity ratchet, an architecture
check, and a release gate that refuses to ship without the evidence — and that when all of
that passes, a human still reads the data flow, because the two worst defects found so far
passed every one of those checks.

That is the claim: **AI code generation can be used responsibly when the verification is
independent of the generator.** Not that it is safe on its own.
