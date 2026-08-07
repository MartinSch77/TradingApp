# Case study: Axivion Suite

@page csaxivion Case study: Axivion Suite

**The claim being tested:** an architecture that exists only in a diagram is a wish.
One that exists as code can be checked on every run — and it can *fail*.

| | |
|---|---|
| Tool | Axivion Suite 7.12.3 |
| Scope | MISRA C++ 2023, CERT/CWE rules, code metrics, **architecture verification** |
| Model | `axivion/architecture.py` — architecture as code, reviewable in a diff |
| Runner | `axivion/start_analysis.sh` / `.ps1`, or `./build_all.sh axivion` |
| Result | **0 architecture findings** — zero divergences, zero absences |

## The problem

This project's layering is `domain ← services ← ui`, with `main.cpp` as the
composition root. The linker already enforces it: `trading_domain` links only
Qt::Core, so a domain file that reaches for the network or a widget does not compile.
That is strong, and it is also *incomplete* — the linker can tell you that an illegal
dependency does not exist, but it cannot tell you whether the dependencies that *do*
exist are the ones the design intended, nor notice when an intended one quietly
disappears.

## The configuration

`axivion/architecture.py` builds the Architecture and Mapping RFG views via
Architecture-ScriptedArchitecture, and Architecture-ArchitectureCheck verifies the
code against them. Two properties make it worth having:

- **It is code, not an export.** A hand-exported GXL model is unreviewable in a diff
  and drifts the moment someone regenerates it. A Python model shows up in review as
  lines someone chose.
- **Edges are declared only where a dependency is both intended AND present.** A
  declared-but-unused edge is an **Absence** finding by design, so the model cannot
  quietly over-declare to stay green. Divergences and absences both land on the
  dashboard as AV findings, and the views open in Gravis for a graphical walk-through.

Two things measured the hard way:

1. **There is no `Tests` component**, deliberately. The analysis IR is the
   `TradingApp` executable, so test binaries never enter the RFG — modelling them
   produced exactly 3 Absence AVs. Measured, then removed.
2. **Comment keys inside rule entries must use `".#"`.** A bare `"#"` fails the
   Suite's own config validator. Also measured, because the error message does not say
   so.

## The finding, and the honest part

Architecture verification: **0 AV findings.** Zero divergences, zero absences,
verified on the dashboard after a full run.

The rule findings are a different and more interesting story, and this is where a
case study earns its keep by not overstating. The analysis reports **154,183**
findings. Their composition matters more than their count:

| Class | Count | What it actually is |
|---|---|---|
| CWE-200 / 20 / 502 / 79 | ~139,500 | Taint fan-out. One untrusted source — JSON from a public API — reaching thousands of sinks. The analysis log itself warns it hit its maximum reported sinks. |
| MISRA 21.6.1 "allocating function is called" | 2,536 | Every `new`. Qt's parent/child ownership model requires it. |
| MISRA 0.3.1 "use of floating-point arithmetic" | 832 | In a program that computes prices and P&L. |
| "Unreachable code" (Qt-Autosar M0.1.1, MISRA 0.0.1) | 1,573 | Dominated by `operator new` false positives. |
| **Individually actionable** | **~490** | Global-namespace symbols, discarded return values, side effects in the right-hand operand of `&&`, unsequenced calls, base types outside a typedef. |

So roughly 0.3% of the reported findings are things a person can act on one at a time.
The rest need either a documented project-level deviation (dynamic memory in a Qt
application is not a defect to be fixed) or a narrower rule configuration.

**Axivion is therefore not a pass/fail gate in this pipeline.**
`axivion/start_analysis.sh` exits 0 regardless of finding counts, and the seven
analyzers that `tools/publish_release.sh` requires to be at zero do not include it.
Saying "our MISRA analysis is green" while sitting on 154,183 findings would be the
kind of claim this repository exists to make impossible.

## The measurable result

- The intended architecture is machine-checked on every run, and currently matches
  the code exactly: 0 divergences, 0 absences.
- An illegal dependency fails the **build** (linker), and a drift from the intended
  model raises an **AV finding** (Axivion). Two independent mechanisms, one property.
- The rule findings are inventoried and classified rather than either fixed
  cosmetically or ignored silently.

## What it does not prove

MISRA C++ 2023 conformance. This code is not MISRA-conformant and does not claim to
be; the standard forbids things (dynamic allocation, exceptions in places, floating
point) that a Qt desktop application is built on. Axivion is here for the
architecture check and for the ~490 real findings, not for a certificate.

Note also that cppcheck's free MISRA addon is MISRA **C** 2012 and this code is
C++23 — 514 of its 527 findings are that mismatch. It is informational only
(`tools/misra_cppcheck.sh`) and deliberately not in any gate.

## Source

- Architecture model: `axivion/architecture.py`
- Runner: `axivion/start_analysis.sh`, `axivion/start_analysis.ps1`
- Findings PDF, via Axivion's own delivered report module: `tools/axivion_report.sh`
- External findings import: `axivion/external_import.py`
- Commit: `06e79d0` (the reference architecture, as code, checked on every run)
- How to obtain and install the tool: [docs/qt-tools.md](../qt-tools.md)
