# Case study: Qt Test Center

@page cstestcenter Case study: Qt Test Center

**The claim being tested:** a test result that exists only in a CI log is not
evidence. Someone has to be able to find the run that verified a requirement six
months later.

| | |
|---|---|
| Tool | Qt Test Center 4.3.0 |
| Scope | every JUnit XML in `test-results/`, one batch per commit |
| Client | `testcentercmd` (the product's own CLI) |
| Runner | `tools/testcenter_upload.sh` / `.ps1`, or `./build_all.sh testcenter` |
| Verdict | licence-bound; reports `skipped` (exit 3) when unreachable, never a gate |

## The problem

The suite produces a JUnit XML per test function. Those files are written, read once by
whoever is watching the build, and then deleted by the next `clean_all`. The
requirement-to-test chain is machine-checked by `tools/trace_report.py`, but the
*results* — which run, on which commit, passing or failing — had no home.

## The mistake worth documenting

The first uploader **POSTed JUnit XML to a REST path that did not exist.** It was
invented from a plausible reading of how such a product ought to work, and it looked
right: a URL, a payload, a 2xx expectation. The product ships `testcentercmd`, and
using it is not a detail — it is the difference between an integration and a
guess that happens to compile.

This is the same class of error as the Squish object map addressing a widget that had
never existed (see [the Squish case study](squish.md)), and it has the same cause:
writing against a mental model of a tool instead of against the tool.

## The configuration

One call, every XML in `test-results/`, batch = the short git SHA. Three details are
load-bearing:

- **Authentication never appears in the repository or a command line.** The preferred
  route is `testcentercmd`'s own credential store (`testcentercmd config token …`,
  which writes `~/.squish/ver1/testcentercmd.ini`). `TESTCENTER_TOKEN` exists for CI,
  where a secret arrives as an environment variable, and user/password works too.
- **The stage checks that a server is actually answering first.** Without that check
  the client waits for interactive credentials and the pipeline stage **hangs** rather
  than reporting — a hang is worse than a failure, because nothing tells you why.
- **Three label names are the product's, not free choices.** `.git.revision` drives
  the Commit Summary section of the printable report, `.git.branch` selects the branch
  for repository lookups, and `.reference.url` becomes a clickable link in the
  References column. Getting these wrong produces an upload that succeeds and is
  useless.

`tools/testcenter_traceability.py` additionally uploads the requirement→test chain, so
the portal shows not just which tests ran but what they were verifying.

## The measurable result

Every pipeline run that has a reachable server files its results as one batch,
identified by the commit that produced them, with the requirement chain attached. The
question "what verified REQ-F-037, and when" has an answer that outlives the working
tree.

The stage is deliberately **not** a gate. It exits 3 (`skipped`) when the tool or the
server is absent, and the quality PDF lists it as a missing licence — so the whole
pipeline stays green on a machine with only open-source tooling.

## What it does not prove

Test Center stores and organises results; it does not judge them. A green batch of a
weak suite is still a green batch. The evidence that the suite is worth trusting comes
from the coverage figures ([Coco](coco.md)) and from the requirements traceability
having 0 hard gaps.

## Source

- Uploader: `tools/testcenter_upload.sh`, `tools/testcenter_upload.ps1`
- Traceability upload: `tools/testcenter_traceability.py`
- Ordered report chain: `tools/make_test_report.sh`
- Commit: `7235753` (the invented REST endpoint replaced by `testcentercmd`)
- How to obtain and install the tool: [docs/qt-tools.md](../qt-tools.md)
