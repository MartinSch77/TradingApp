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
| **Axivion Suite** | MISRA C++ 2023, architecture/cycle/dead-code analysis, the dashboard the external findings are imported into | `axivion` | stage skips; the other eight analyzers still gate |
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

**Get an upload token.** It is **not** in the user menu — that wording was wrong and cost
time. In Test Center 4.3.0 it lives under **Admin → User Management**, per user, and the
product calls it an *upload token* (route `/admin/accesstokens`; the built-in help anchor is
`user-management.html#create-and-manage-upload-tokens`). A token is preferable to a
username/password pair because it can be revoked without changing anyone's login.

**Store the token once, and keep it out of everything.** `testcentercmd` has its own
credential store, which is the route to prefer: the secret then lives in
`~/.squish/ver1/testcentercmd.ini` (`%APPDATA%` equivalent on Windows) and never appears in
a command line, an environment variable, a shell history or this repository.

```bash
~/testcenter-4.3.0-linux-x64/bin/testcentercmd config token <the token from the UI>
tools/testcenter_upload.sh          # finds it by itself — no TESTCENTER_TOKEN needed
```

`testcentercmd config remove` deletes it again. The upload script passes **no**
credentials when none were given to it, precisely so this store is what gets used; if the
store is empty too, the client fails immediately with `No authentication provided`
(measured: exit 1, no prompt), which the script reports as `skipped`.

**A token is optional.** `testcentercmd` accepts *either* a token *or* the login email and
password, so an upload can be made the moment the first user exists:

```bash
TESTCENTER_USER=you@example.com TESTCENTER_PASSWORD='…' tools/testcenter_upload.sh
```

**There is no usable REST API for this.** Test Center answers every unregistered REST
caller with `{"error":{"code":1006,"message":"Only known clients permitted for REST
access"}}` — measured with a bearer token, with the token as a query parameter, and with a
browser user agent. An earlier version of the Windows script POSTed JUnit XML to an
invented `/api/v1/...` path and therefore could never have worked. Uploads go through
`testcentercmd` on both platforms.

**Not to be confused with the public RSA key** also shown in the administration area: that
belongs to Test Center's OAuth manager for the **Jira / Xray application link**
(`applicationlinks.js`), not to result uploads. Pasting it anywhere near
`--token` will simply fail to authenticate.

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

### Labels, three of which are the product's

The upload script attaches labels automatically. Three names are **not** free choices —
Test Center gives them meaning:

| Label | Filled from | What Test Center does with it |
|---|---|---|
| `.git.revision` | `git rev-parse HEAD` | drives the **Commit Summary** section of the printable report |
| `.git.branch` | the current branch | selects the branch for repository file lookups |
| `.reference.url` | the `origin` remote + the SHA | becomes a clickable link in the **References** column |
| `worktree=dirty` | added when the tree has uncommitted changes | *see below* |
| `suite=…` | `--suite`, default `unit+integration` | tells the Qt Test batch from the Squish GUI batch |
| `platform=…` | `uname` / `PROCESSOR_ARCHITECTURE` | which of the four supported platforms produced it |

`worktree=dirty` is the honest one and it matters. A batch still gets a `.git.revision`
when the tree has uncommitted changes — the report needs a commit to anchor on — but the
sources that were tested are then *not* that commit. Without the extra label, a green
batch reads as evidence for code that was never tested. Add your own labels with
`--label key=value` (repeatable) or `TESTCENTER_LABELS`.

### Starting and stopping the server

The server is a long-running process that owns its own database; it is not started by
`build_all.sh`, because a results server outlives any one build.

```bash
TC=~/testcenter-4.3.0-linux-x64          # wherever the archive was unpacked
$TC/bin/testcenter start                 # foreground; add & or use a unit file
$TC/bin/testcenter status                # is it up, and on which port
$TC/bin/testcenter stop
```

Defaults worth knowing:

| Thing | Default | Note |
|---|---|---|
| URL | <http://localhost:8800> | `TESTCENTER_URL` for the upload script |
| Data | under the installation directory | back this up, not the repository — the history is not in git |
| First user | created interactively in the browser | cannot be scripted; do it once before the first upload |

A run that finds no server does **not** fail the build: `tools/testcenter_upload.sh`
exits 3 (“skipped”, like every licence-bound stage) and the JUnit XML stays in
`test-results/` for a later upload. Nothing is lost by uploading late.

### Where the results are, once uploaded

Open <http://localhost:8800> and select the **TradingApp** project. One upload is one
**batch**, named after the short git SHA, so “which commit produced this?” is answered
by the batch name alone.

| What you want to know | Where to look |
|---|---|
| Did this commit pass? | *Batches* → the SHA → pass/fail per suite |
| Which test case failed, and its message | *Test Results*, filter by batch → drill into the suite → the function |
| One test's history across commits | *Test Results* → pick the case → *History* — this is the view that shows a flaky test as flaky |
| GUI vs unit results, kept apart | the suite name: `squish-suite_gui` is the Squish run, `tst_*` are the Qt Test suites |
| Coverage per test case | the `coco-components` and `coco-gui` XMLs upload as their own suites (see below) |
| What changed since the last batch | *Compare* two batches — new failures, fixed cases, and cases that disappeared |

Suite names map to files one-to-one, which is what keeps the two kinds of evidence
separable in Test Center:

| Suite in Test Center | File | What it measures |
|---|---|---|
| `tst_<name>` | `test-results/tst_<name>.xml` | one Qt Test suite (unit / integration) |
| `squish-suite_gui` | `test-results/squish-suite_gui.xml` | the Squish GUI workflows |
| `coco-components` | `test-results/coco-components.xml` | which component functions the integration tests reach, per test |
| `coco-gui` | `test-results/coco-gui.xml` | the Squish suite's own coverage — **never merged** with the unit suites' |

### Activation is interactive, and nothing works before it

A freshly unpacked Test Center redirects every URL to `/activation/index` until a licence
has been entered and the first user created **in a browser**. That step is interactive by
design and cannot be scripted, so until it is done:

- `tools/testcenter_upload.sh` has nothing to upload into and reports `skipped` (exit 3);
- there is no data to export and nothing to screenshot;
- the quality PDF lists Test Center under MISSING LICENCES, which is accurate.

Tell the three states apart by the redirect target of `GET /`, because they need different
fixes and only one of them is a problem:

| `GET /` answers | Meaning |
|---|---|
| connection refused | server not running — `bin/testcenter start` |
| `302 → /activation/index` | up, but no licence and no first user yet — do it in a browser |
| `302 → /login/index` | up and activated; an upload needs only a credential |

Measured on this machine: `302 → /activation/index` on 2026-08-06 morning (up, not
activated), and `302 → /login/index` the same day once activation was completed, after
which an upload token authenticated and **304 test cases across 27 suites** imported into
project `TradingApp`. The upload path is therefore verified end to end, not assumed.

### Coverage: two figures, deliberately not one

`tools/coverage.sh coco` measures the unit/integration suites over `src/domain` +
`src/services` and **excludes** `src/ui`. `tools/coverage.sh coco-gui` measures the
Squish suite over an instrumented tree that **includes** `src/ui`, and writes a separate
database and report:

```bash
tools/coverage.sh coco          # coverage/coco/index.html      — unit + integration
tools/coverage.sh coco-gui      # coverage/coco-gui/index.html  — Squish GUI suite only
```

They are kept apart on purpose. A single blended percentage would answer neither
question a reader actually has — “are the domain decisions exercised?” and “does a user
driving the real window reach this code?” — and blending them lets a large, easily
covered domain hide an untouched UI.

### The Test Center report with illustrations, and why this one step is manual

Test Center's own document is the **Printable Batch Report**: *Explore* → select the batch
→ **Printable Report**. It contains the illustrations that no script here reproduces —
pie charts for tests, test runs, suites and reports — plus a Commit Summary (from
`.git.revision`), a per-report table with the labels, a suite summary and the full test
results grouped either by suite or by report. The configuration menu on the right toggles
each section, and the menu itself is excluded from the printed output.

To get a PDF: click **Print** in that menu and choose *Print to PDF* in the browser.

**This step cannot be automated on this machine, and the reason is worth writing down**
rather than quietly skipping. Per the shipped manual (`pdf-report-generation.html`), the
product's *only* PDF route is the browser's own print dialog — there is no
`testcentercmd` export subcommand and no reachable REST endpoint (error 1006, above).
Automating it needs a headless browser, and this box has none: no Chromium, Firefox,
`wkhtmltopdf` or Playwright, no QtWebEngine in the Qt installation, and installing one
needs root. So:

- everything *else* in the reporting chain is scripted and reproducible
  (`tools/make_test_report.sh`);
- this one document is a documented two-click manual step;
- save it as `downloads/TradingApp-testcenter-report.pdf` if you want it beside the
  others, and `tools/make_test_report.sh` will list it as present.

A note on honesty in the quality PDF: it reports whether Test Center was *reachable and
uploaded to*, which is a fact a script can establish. It does not claim a printable report
exists unless the file is there.

### Requirement traceability, fed from the same sdoc file

Test Center can show which requirements a batch covers, but it only knows requirements it
has been told about, and normally it learns them from Jira, Xray, Polarion or Azure DevOps.
This project keeps its requirements in StrictDoc, so the **generic traceability**
integration is the applicable one: it accepts a CSV of `id,name,uri,project`.

```bash
python3 tools/testcenter_traceability.py        # -> test-results/testcenter-traceability.csv
```

That file is generated from `requirements/requirements.sdoc` — the same single source as
`docs/requirements.md` and the traceability matrix. This is the point: a requirement list
typed into Test Center by hand would be a second source of truth, and the first time it
drifted, the coverage view would be reporting on requirements that no longer exist. The
`uri` column links each row back to its heading in the generated document on the code host,
derived from the `origin` remote rather than hardcoded.

Two steps remain in the UI, neither of which has a command-line equivalent in 4.3.0:

1. *Global Settings* → enable **Generic Integration** → upload the CSV. Leave
   “remove traceability data not contained in this upload” **off** unless the CSV is
   known to be complete.
2. Map the external project (`SRS-TRADINGAPP`) to the Test Center project (`TradingApp`).

After that the test-to-requirement links can be generated instead of clicked, because the
test sources already carry the ids:

```bash
testcentercmd integration map --integration=generic --project=TradingApp \
    --repository=<id> --branch=main --prefix='@relation('
```

The `--prefix` is what keeps the match honest. Every requirement reference in a test file
is written `// @relation(REQ-…, scope=function)`, so requiring the prefix means a bare
mention of an id in a comment or a string literal is not counted as a verification link.
Requiring nothing would inflate coverage with prose.

`--repository=<id>` needs a repository configured first: *Global Settings* → **Repository
Integration** → add a Git repository pointing at the **root** of a clone on the machine
running Test Center, with a URL template containing `${COMMIT}` (for this project,
`https://github.com/MartinSch77/TradingApp/commit/${COMMIT}`), then *Connect a Project*.
That is also what turns file paths under a failed verification into clickable source.

Note the division of labour: `tools/trace_report.py` remains the **gate** — it fails the
build on a hard traceability gap and does not depend on any server. Test Center adds the
history and the per-batch view on top. Neither replaces the other.

### The Axivion findings as their own PDF

`tools/axivion_report.sh` (and `.ps1`) produces
`downloads/TradingApp-axivion-report.pdf` — the findings per rule and per file, with the
delta against the previously analysed version. Reproducible, and **not** a reimplementation:
it drives the Suite's own reporting framework.

```bash
axivion/start_analysis.sh          # analyse first, or the PDF describes the LAST analysis
tools/axivion_report.sh            # -> downloads/TradingApp-axivion-report.pdf
tools/axivion_report.sh --no-details    # summary only (the detailed run is ~330 pages)
```

| Piece | What it is |
|---|---|
| `$AXIVION_HOME/bin/report_runner` | the Suite's report/visualization runner |
| `$AXIVION_HOME/example/reports/report_misra_pdf.py` | Axivion's delivered MISRA PDF module — the document's layout, rule tables and delta logic are the vendor's, so a Suite upgrade improves the report for free |

Two things that cost time if guessed:

- `--noninteractive` belongs to the **runner**, before the subcommand. After it, the parser
  rejects it with "unrecognized arguments".
- `create_visualization` takes **no** `--output_dir`: it publishes a visualization into the
  dashboard rather than writing a file. The PDF module is the file artefact.

The script prints the analysis version it reported on, because the most likely mistake is a
PDF generated before the analysis it appears to describe. It is separate from the quality
PDF on purpose: `make_report.py` quotes Axivion's finding *counts* inside a run summary,
while this is the findings themselves — a different document for a different reader.

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

Nothing breaks. `./build_all.sh` runs every open-source stage, the eight analyzers
still gate at zero findings, gcov and clang MC/DC still measure coverage, and the
quality PDF prints the four licensed tools with "no licence here" against them. That
distinction — *measured and clean* versus *not measured on this machine* — is the
reason the report lists them at all.
