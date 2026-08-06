# The licensed Qt tools: how to get them, install them, point this project at them

@page qttools Licensed Qt tools (Axivion, Squish, Coco, Test Center)
@tableofcontents

Four of the tools this pipeline can use are **commercial products from The Qt
Company** and need a licence. None of them is installable by `setup.sh` — that is
the dividing line this repository draws:

* everything **open source** the pipeline needs is installed by `./setup.sh` /
  `.\setup.ps1`;
* everything **licence-bound** is installed by hand, and every stage that uses one
  reports **skipped** (exit code 3) when it is absent, is listed as a **missing
  licence** in the quality PDF, and **never fails a build**.

Run `tools/check_prerequisites.sh` at any time to see which of them this machine
has.

| Tool | What it gives this project | Stage | Without it |
|------|---------------------------|-------|-----------|
| **Axivion Suite** | MISRA C++ 2023, architecture/cycle/dead-code analysis, the dashboard the external findings are imported into | `axivion` | stage skips; the other seven analyzers still gate |
| **Squish** | the GUI test suite that drives the real application | `gui` | stage skips; the unit suite still runs |
| **Squish Coco** | statement / decision / condition / **MC/DC** coverage from an instrumented build | `coverage` | gcov + clang MC/DC still measure; Coco's numbers are simply absent |
| **Squish Test Center** | one place holding every test result, with history and traceability | `testcenter` | stage skips; the JUnit XML stays in `test-results/` |

---

## Getting a licence

All four are sold by **The Qt Company**. The route is the same for each:

1. Sign in to the **Qt Account** (<https://login.qt.io>) that holds the licence.
2. Open **Downloads** and pick the product (Axivion Suite, Squish, Squish Coco,
   Squish Test Center). Each is a self-contained installer or archive per platform —
   they are *not* part of the online Qt installer that brings Qt 6 itself.
3. The licence arrives either as a **licence file** to place in your home directory
   or as a **licence server** address, depending on the product and the agreement.
   Both are described in the mail that comes with the download.

Evaluation licences exist for all four and work exactly the same way; the scripts
here cannot tell an evaluation from a permanent licence, and do not care.

---

## Squish (GUI testing)

**Install.** The installer unpacks into the home directory by default:

```bash
chmod +x squish-for-qt-<version>-linux64.run
./squish-for-qt-<version>-linux64.run        # → ~/squish-for-qt-<version>
```

During installation it asks for the **Qt version to bind to**; point it at the same
kit this project builds against (`~/Qt/<version>/gcc_64`, see @ref platforms).

**Licence.** A file called `.squish-license` (or `.squish-3-license`) in your home
directory, exactly as delivered. Verify with:

```bash
~/squish-for-qt-<version>/bin/squishrunner --version
```

A version number means the licence is accepted; an error means it is not.

**Point the project at it.** Nothing to configure in the normal case — the runner
finds `~/squish-for-qt-*` on its own, newest first. Override when needed:

```bash
tools/squish_run.sh build --squish-dir ~/squish-for-qt-9.2.2
SQUISH_DIR=~/squish-for-qt-9.2.2 tools/squish_run.sh build
```

**What it runs.** `squish/suite_gui`, four test cases, every one of them in FORCED
SIMULATION (`TRADINGAPP_FORCE_SIMULATION=1` plus an isolated `XDG_CONFIG_HOME`), so
no GUI run can reach a real account — see REQ-N-007 and `tools/squish_run.sh`. The
object map addresses widgets by `objectName` only; `tools/check_object_names.py`
keeps those names from disappearing.

---

## Squish Coco (code coverage incl. MC/DC)

**Install.**

```bash
chmod +x SquishCoco-<version>.run
sudo ./SquishCoco-<version>.run              # → /opt/SquishCoco
```

**Licence.** Coco reads `~/.squish-3-license` or a licence server. Verify with:

```bash
/opt/SquishCoco/bin/cocolic --check          # prints the licence type and ID
```

**Point the project at it.** `COCO_DIR` if it is not in `/opt/SquishCoco`:

```bash
COCO_DIR=/opt/SquishCoco tools/coverage.sh coco
tools/coverage.sh coco-components            # per-test-case call coverage
tools/coverage.sh coco-ai                    # CocoAI test suggestions
```

`tools/coverage.sh auto` runs Coco **in addition to** gcov and clang MC/DC, not
instead of them — three back ends measuring different things is the point.

**Two things learned the hard way**, both handled by the script but worth knowing if
you run the tools by hand:

* an instrumented binary writes `<name>.csexe` into its **working directory**, not
  next to the executable;
* `cmreport` writes **one output file per invocation** — `--html` together with
  `--csv-excel` fails with *"Multiple output files defined"* and produces neither.
  `--stat` is what prints a number to the console; `--text=` writes a 0-byte file.

---

## Squish Test Center (results, history, traceability)

**Install.** An archive, no root needed:

```bash
tar xf testcenter-<version>-linux-x64.tar.gz -C ~
~/testcenter-<version>-linux-x64/bin/testcenter start
```

Then open <http://localhost:8800> **once** to create the first user — that step is
interactive by design and cannot be scripted.

**Licence.** Entered in the web UI on first use.

**Get an upload token**: user menu → access token. A token is preferable to a
username/password pair because it can be revoked without changing anyone's login.

**Point the project at it.** Everything is a parameter, with an environment
fallback:

```bash
export TESTCENTER_URL=http://localhost:8800
export TESTCENTER_TOKEN=<the token from the UI>
tools/testcenter_upload.sh                              # every XML in test-results/
tools/testcenter_upload.sh --dry-run                    # list, send nothing
tools/testcenter_upload.sh --testcenter-dir ~/testcenter-4.3.0-linux-x64 \
                           --project TradingApp --batch "$(git rev-parse --short HEAD)"
```

Uploads go through `testcentercmd`, which ships with both Test Center and Squish;
the script finds it in either. The batch defaults to the short git SHA, so a batch
in Test Center maps to exactly one commit.

**What gets uploaded.** Every JUnit XML under `test-results/` — the ~25 unit and
integration suites *and* the Squish GUI suite, which is why both write their results
to the same directory.

---

## Axivion Suite (MISRA C++ 2023)

**Install.** The Suite is an archive plus a licence; unpack it and set
`AXIVION_HOME` (the scripts also accept the default `~/bauhaus-suite`):

```bash
tar xf bauhaus-suite-<version>.tar.gz -C ~
export AXIVION_HOME=~/bauhaus-suite
export PATH="$AXIVION_HOME/bin:$PATH"
```

**Licence.** A file or a licence server, as delivered; `axivion_ci --version` is the
check.

**Point the project at it.**

```bash
axivion/start_analysis.sh                    # one run at a time (flock)
tools/mcp_env.sh --persist                   # env for the .mcp.json dashboard servers
```

Two constraints that are properties of the tool, not of this project:

* **x86-64 only.** On ARM64 (a Raspberry Pi build) the stage says so and skips.
* **Qt < 6.10** for its front end — Suite 7.12.3 asserts on Qt 6.10's `qvariant.h`,
  so the stage falls back to the newest older kit on its own.

The results are also imported back: `axivion/external_import.py` pushes the seven
other analyzers' findings into the same dashboard, so one place shows everything.

---

## Windows

Every one of the four exists for Windows and the PowerShell counterparts accept the
same parameters (`tools\squish_run.ps1 -SquishDir`, `tools\testcenter_upload.ps1
-TestCenterDir`, `COCO_DIR`, `AXIVION_HOME`). The install locations differ —
`C:\Squish...`, `C:\Program Files\squishcoco` — which is exactly why the paths are
parameters rather than constants. See @ref windows for the rest of the Windows
substitutions.

---

## If you have no licences at all

Nothing breaks. `./build_all.sh` runs every open-source stage, the seven analyzers
still gate at zero findings, gcov and clang MC/DC still measure coverage, and the
quality PDF prints the four licensed tools with "no licence here" against them. That
distinction — *measured and clean* versus *not measured on this machine* — is the
reason the report lists them at all.
