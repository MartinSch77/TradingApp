# Verification Strategy

Referenced by `SWE.4`, `SWE.5`, `SWE.6`. States the coverage minimums,
integration approach, and acceptance criteria those processes point back to
— written once, here, per `process-model.md` §5's "loosely coupled, state
once" rule.

## Levels

| Level | Scope | Tooling | Owning process |
|---|---|---|---|
| Unit | One class/module in isolation | `tests/tst_*.cpp` tagged `@tstid`, `.clang-tidy`, cppcheck, Clang Static Analyzer, g++ `-fanalyzer`, clazy | SWE.4 |
| Integration | Cross-module/layer interaction, no network | The subset of `tests/` exercising `EtoroClient` against `MockHttpServer`, config layering, the simulation engine composed with the paper book | SWE.5 |
| Qualification/System | End-to-end, requirement-level | Squish GUI suite + Coco coverage, Test Center upload, `docs/traceability.html` | SWE.6 |

## Coverage minimums

- **Every requirement with `VERIFICATION: T`** (or containing `T` in a
  combined field like `A/T`) **must** trace to at least one executed,
  passing test — `tools/trace_report.py`'s "0 hard gaps" is the mechanical
  acceptance bar, not a target to approach.
- **Line/branch coverage**: reported by `tools/coverage.sh` (gcov/LLVM
  source-based); no fixed percentage floor is set project-wide (a floor
  chosen without per-module context invites either meaningless 100%-via-
  trivial-tests or an arbitrarily low bar) — instead, MC/DC coverage is
  required specifically for every boolean decision gating money-relevant
  logic (`CLAUDE.md`'s "≤ 6 conditions" non-negotiable exists precisely so
  MC/DC stays tractable there).
- **Mutation testing** (`tools/mutation_test.sh`, Mull): informational, not a
  gate, per `CLAUDE.md` — but a file explicitly brought into the mutation
  pilot (currently `PositionMath.cpp`, `Money.cpp`, `ConfirmGate.cpp`,
  `PathOutcome.cpp`) is expected to reach ≥ 90% kill rate or have every
  surviving mutant DOCUMENTED as a genuine equivalent mutant (the
  `accountValuePerPoint`-redundancy pattern `CLAUDE.md` records) — an
  undocumented survivor is a verification gap, not an accepted one.
- **Fuzzing** (`tools/fuzz.sh`, libFuzzer): informational; required for any
  new untrusted-input parser (the pattern `YahooChartParser`/
  `OllamaResponseParser` already follow) before it is considered
  unit-verified for REQ-N-009-adjacent input-validation concerns.

## Integration test approach

Integration tests in this project deliberately do **not** stand up a real
network: `MockHttpServer` runs in-process, so the integration level tests
real HTTP retry/backoff/rate-limit logic and real JSON parsing without
network flakiness entering the acceptance decision. An integration test adds
value only when it exercises a path a unit test structurally cannot (e.g. two
modules' state interacting across a signal/slot boundary, or a retry policy
that only manifests across several requests) — `SWE.5`'s review confirms this
before accepting a new integration test rather than counting bodies.

## Acceptance criteria — "sufficiently tested" checklist

A change is accepted as sufficiently verified when **all** of:

1. `tools/trace_report.py` reports 0 hard gaps for every requirement the
   change touches.
2. `tools/run_tests.sh` reports 0 failures, with `test-results/*.xml` newer
   than the changed sources.
3. `tools/static_analysis.sh` reports 0 findings (or every finding is a
   pre-existing, documented, justified suppression — never a new one added
   silently).
4. `tools/lizard_metrics.py`'s ratchet is unchanged or improved.
5. For a change touching money-relevant logic (order sizing, risk gates,
   cost model): the relevant Mull-pilot file (if any) still meets its kill
   rate, and a new boundary condition introduced is pinned by an explicit
   test (this project's own established pattern — see `TS-POS-013..018`).

Meeting 1–4 with 5 not applicable is sufficient; 5 is additive, not optional,
whenever it applies.
