---
name: clean-build-release
description: Take TradingApp from a clean tree to a published GitHub release — full clean, full build_all (all nine stages incl. Axivion), then commit, push, tag and tools/publish_release.sh. Use when the user asks to "clean, build and release", "ship a release", "cut a release", or "clean_all, build_all, commit and push". Refuses to publish on a red pipeline.
---

# Clean → build → release

Three phases, each gating the next. The pipeline is the evidence a release
claims to have; **a red stage or stale evidence stops the release, it does not
get worked around.**

Budget ~1.5–2 h wall clock for phases 1–2. Run them in the background and
monitor; never sit in a foreground call waiting.

## Phase 0 — preflight (do this first; it costs 2 minutes and saves two hours)

```bash
git status --porcelain | wc -l          # what is uncommitted
git rev-parse --abbrev-ref HEAD         # which branch
grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt
git tag --list | tail -5                # does that version already have a tag?
gh auth status                          # publishing needs a working gh
pgrep -af 'start_analysis|build_all'    # nothing may be running already
```

Resolve these **before** cleaning:

- **Version already tagged.** `tools/publish_release.sh` derives the tag from
  `CMakeLists.txt` (`VERSION x.y.z` → `vx.y.z`). If that tag exists, decide with
  the user: bump the version (normal) or re-publish onto the existing tag
  (rare). Discovering this after the build wastes the whole run.
- **An Axivion analysis is already running.** There is a flock in
  `axivion/start_analysis.sh`; one run at a time, and **no clean or build while
  one runs**. Wait or stop it.
- **On the default branch.** `main` is the default here. Commit to a feature
  branch and push that, so the change goes in through a PR. Only commit directly
  to `main` if the user says so.

## Phase 1 — clean

```bash
./clean_all.sh
```

Do **not** pass `--deep` unless the user asks. `--deep` also removes
`.axivion-cache/` and `.fslckout`, which forces Axivion to re-analyse from
scratch and **loses the local finding history** — that history is what produces
the delta in the Axivion PDF.

Verify the trees are gone (`build`, `build-cov-*`, `build-san*`, `build_axivion`,
`analysis-results`, `test-results`, `downloads`). `clean_all.sh` is the only
thing that should delete them; do not hand-remove others.

## Phase 2 — build everything

```bash
nohup ./build_all.sh > /tmp/.../build.log 2>&1 &
```

Nine stages: `build test trace docs coverage analysis sanitize axivion report`.

**While it runs, change nothing in the repository.** The coverage, analysis,
sanitize and Axivion stages each compile from the working tree; a file edited
mid-run produces evidence for code that no longer exists. This is the single
most common way this pipeline produces a confident lie.

Note the licence-bound stages are **not** in the default run: `gui` (Squish) and
`testcenter` are `EXTRA_STAGES`. The quality PDF from a default run therefore
reports them absent *even on a machine that has the licences*. If the release
should carry the full picture, run `tools/make_test_report.sh` after this phase —
that is the chain that adds the Squish suite, its separate Coco coverage, the
traceability CSV, the Test Center upload and the Axivion PDF.

## Phase 3 — judge the result honestly

Read the stage summary at the end of the log, plus the final `build_all exit=`
line. **Do not trust the exit code of a wrapper command** — `nohup … &` returns
0 immediately, and a completion notification for a background shell reports the
wrapper, not the pipeline.

- `ok` — green.
- `skipped` / exit code **3** — a licence-bound or unavailable stage. **Not a
  failure.** Exists in `axivion/start_analysis.sh`, `tools/coverage.sh` (Coco /
  OpenCppCoverage / LLVM-mcdc) and `tools/make_docs.ps1`.
- `FAILED` — stop. Diagnose per the `verify` skill, fix the cause, then **re-run
  the affected stages** (`./build_all.sh <stage>`), because the fix changed the
  sources the earlier evidence described.

Two failures seen here that are *not* code defects — check for them before
diagnosing deeper:

- **A link race in the ASan tree** (`undefined reference to 'main'` for one test
  while the same test links fine in `build/`). Non-reproducible; the object file
  exists afterwards. Remove `build-san build-san-tsan` and re-run `sanitize`.
- **An Axivion worker killed** (`Worker N terminated unexpectedly … exit code
  -15`). SIGTERM is a kill, not a finding — check whether someone interrupted the
  run before treating it as a defect. The dashboard then holds an *aborted*
  analysis, so re-run the stage before generating the Axivion PDF.

Verify the test result from the XML, not the console: `test-results/*.xml`
carry `tests`/`failures`/`errors` attributes, and console output can be
truncated.

## Phase 4 — commit and push

Only once phase 3 is green (skips allowed).

1. Branch if on `main`: `git switch -c <topic-branch>`.
2. Stage everything the release includes, and check the untracked list — new
   source, tests, tools and docs are easy to leave behind (`git status
   --porcelain | grep '^??'`).
3. Write the message as a **file** and pass `-F`; these messages are long.
   State what changed and why, name defects found, and say what is deliberately
   *not* done. If the commit carries unrelated in-flight work, say so rather
   than implying one coherent change.
4. End with the required trailer:
   `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
5. `git push -u origin <branch>`.

Committing does not change file mtimes, so the evidence stays newer than the
sources — which is what `publish_release.sh` checks.

## Phase 5 — release

`tools/publish_release.sh` is the **only** way to publish. Do not script `gh
release` by hand, and do not confuse it with `./build_all.sh release`, which is
just an optimised RelWithDebInfo build for profiling.

**Confirm with the user before this phase.** Pushing a tag and creating a
release are outward-facing and awkward to reverse.

```bash
tools/publish_release.sh --dry-run      # says exactly what it would publish
```

It refuses unless: the working tree is clean, JUnit results exist with no
failures and are newer than the newest tracked source, every analyzer output
totals **zero**, the metrics ratchet and traceability pass (re-run, not trusted),
and the quality PDF is newer than the test results. If it refuses, it is right —
fix the gap.

Then, in order:

1. Merge the branch (PR) so the tag points at reviewed code on `main`.
2. `git tag vX.Y.Z && git push origin vX.Y.Z` — this triggers
   `.github/workflows/release.yml`, which rebuilds **all four platforms** and
   attaches their binaries. Never reimplement that packaging.
3. Wait for that workflow (`gh run watch`).
4. `tools/publish_release.sh` — adds the docs zip and the **qualification
   bundle**, which is the one artefact CI cannot produce alone because its PDF
   carries the Axivion result from a licensed Suite.
5. Report the release URL and list the attached assets.

## Rules

- Never publish, or claim green, on a red pipeline — and never weaken a check to
  get there. Findings get fixed, not baselined (the lizard gate is a ratchet, and
  PMD CPD clones get a helper extracted).
- Exit 3 is "skipped" and stays green; every other non-zero code is real.
- Nothing in the repo is edited between the start of phase 2 and the end of
  phase 3.
- Report what actually happened: which stages were skipped and why, which
  numbers came from this run, and anything left undone. A release note that
  overstates the evidence defeats the purpose of the pipeline.
