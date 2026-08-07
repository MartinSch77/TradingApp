# Case study: Squish Coco

@page cscoco Case study: Squish Coco

**The claim being tested:** "the line ran" is not "the condition combination was
tested". Line coverage can be high while the logic underneath is barely exercised.

| | |
|---|---|
| Tool | Squish Coco (`coveragescanner`, `cmcsexeimport`, `cmreport`) |
| Scope | statement, decision, condition and **MC/DC**, reported per suite |
| Runner | `tools/coverage.sh` / `.ps1`, or `./build_all.sh coverage` |
| Result | MC/DC **77.0% → ~88%** across three deliberate rounds |

## The problem

The project had 90.5% line coverage from gcov and no idea what that meant. A covered
line can contain a boolean decision whose combinations were never tried, and for code
that decides whether to risk money the combinations are the interesting part. Without
a decision-level number, "90.5%" is a comfortable figure that answers a question
nobody asked.

## The configuration

Three things about Coco that the manual does not make obvious, all measured here:

1. **An instrumented binary writes `<name>.csexe` into its WORKING DIRECTORY**, not
   next to the executable. The first attempt failed with `Cannot open CSExe file
   tst_aiadvisor.csexe for reading`. Each test now runs from its own directory.
2. **`cmreport --html` takes a FILE, not a directory** — handed a directory it
   silently produced nothing. The CSV switch is `--csv-excel`, and `--text=` writes a
   0-byte file whatever `--section` values it is given. `--stat` is what prints a
   number.
3. **Coco's front end parses only up to C++20**, so that build tree alone is
   configured with `-DCMAKE_CXX_STANDARD=20` while everything that ships or is
   analyzed stays on C++23. `CMAKE_AR=cslib` and `CMAKE_LINKER=cslink` are both
   required for static libraries, and Qt/STL/SDK headers must be excluded from
   instrumentation or `cmreport` crashes on the merged database.

A fourth was a defect in this project's own wiring rather than in Coco:
`coverage.sh auto` ran Coco **instead of** gcov and clang MC/DC the moment a licence
appeared, which silently emptied `coverage/gcov` and left the quality PDF reporting
"no coverage artefacts were produced". All three back ends measure different things,
so all three now run.

## The finding

The first honest measurement, with all four levels reported:

```
statement  82.790% (1169/1412)
decision   80.913% (2713/3353)
condition  78.253% (3109/3973)
mcdc       77.424% (2867/3703)
```

Against gcov's 90.5% of lines. The 13-point gap between "lines executed" and
"condition combinations tested" is the entire argument for having the tool.

## The correction

Three rounds of tests, each written **against Coco's own list of unexecuted
conditions** rather than against a guess about what might be untested. That
distinction is the method: guessing produces tests for code that was already covered,
while the report names the exact combinations nobody has tried.

| Round | MC/DC | Commit |
|---|---|---|
| baseline, measured for the first time | 77.0% | `5b92ef1` |
| tests written against the unexecuted-condition list | **84.5%** | `57a42da` |
| …continued, and one real defect found on the way | **86.9%** | `356e896` |
| the payload shapes eToro really sends | **~88%** | `e41c812` |

The middle round is the one worth reading: writing tests to cover conditions found a
genuine defect, which is the outcome coverage work is supposed to have and usually is
not honest enough to report.

## The measurable result

MC/DC from 77.0% to roughly 88%, with every step traceable to a commit and none of it
achieved by deleting or weakening a test. GUI coverage is measured in its **own**
database and its own report, never merged with the unit figure — a blended number
would let a thoroughly tested domain conceal an untested interface.

## The GUI coverage figure is currently ABSENT, not zero

The unit MC/DC figure above is measured. The **GUI** coverage figure is not, and the
distinction is deliberate.

`tools/coverage.sh coco-gui` builds a second, Coco-instrumented tree with `csg++` and
runs the Squish suite against it. Measured 2026-08-07: the suite ran green (7 cases, 62
verifications) but **the Coco runtime wrote no `.csexe` execution report**, so there is
nothing to turn into a percentage. The step exits 3 and says so rather than reporting
0%, which would read as "the GUI is untested" — a different and false claim.

The likely cause is the same working-directory lesson as above, one level further out:
an instrumented binary writes its report into its own working directory, and under
Squish that directory belongs to `squishserver`, not to the script. `coverage.sh`
already pins the path with `COVERAGESCANNER_ARGS=--cs-exec=…` and falls back to
searching for the file, but the environment `export` may not reach an AUT that
`squishserver` launches as a separate process. The suite's own `squish/suite_gui/envvars`
is the channel Squish *does* apply to the AUT, which is where the fix probably belongs.

That is a hypothesis, not a diagnosis — it has not been confirmed, so nothing here
claims GUI coverage exists. Until it is measured, the quality report lists it as absent.

## What it does not prove

MC/DC at 88% means 12% of condition combinations are still untried, and the clang
MC/DC back end has a hard limit of 6 conditions per decision, which is why this
codebase keeps every boolean decision at or below that width. High MC/DC also says
nothing about whether the *right* thing is being decided — that is what the
requirements traceability is for.

## Source

- Runner: `tools/coverage.sh`, `tools/coverage.ps1`
- Windows specifics: [docs/windows.md](../windows.md)
- Verification narrative: [docs/verification.md](../verification.md)
- Commits: `5b92ef1`, `57a42da`, `356e896`, `e41c812`
- How to obtain and install the tool: [docs/qt-tools.md](../qt-tools.md)
