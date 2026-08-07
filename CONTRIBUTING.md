# Contributing

Thanks for your interest! This project is a Qt 6 / C++23 desktop trading app
with an unusually complete quality toolchain — contributions are expected to
keep every leg of it green.

## Getting started

```bash
./setup.sh            # provision all tools on a naked Debian/Ubuntu (idempotent)
./build_all.sh        # build app + tests and generate every artefact
./clean_all.sh        # remove everything generated
```

On Windows the same pipeline runs natively (see `docs/windows.md`):

```powershell
.\setup.ps1           # provision the toolchain (winget + pip + aqtinstall)
.\build_all.ps1       # same stages, same order
.\clean_all.ps1       # remove everything generated
```

Individual stages: `./build_all.sh build test` for the quick loop. Copy
`apiKeyEtoro.example.json` to `apiKeyEtoro.json` for real-API work — without
it the app runs in a fully functional SIMULATION mode (preferred for
development).

## Quality bar for pull requests

1. **Tests pass**: `tools/run_tests.sh build` (Windows:
   `tools\run_tests.ps1`) — all Qt Test binaries, zero failures. New
   behaviour needs a new test.
2. **Traceability intact**: `python3 tools/trace_report.py` must report no
   hard gaps. New tests carry the tag block (see `docs/test_spec.md`):
   `//! @tstid TS-… @design DES-…` + `// @relation(REQ-…, scope=function)`.
   New requirements go into `requirements/requirements.sdoc` (StrictDoc,
   single source of truth) — `docs/requirements.md` is generated, never edit
   it by hand.
3. **Static analysis**: `tools/static_analysis.sh build` — do not add new
   cppcheck/clang-tidy findings (`.clang-tidy` documents the check set and
   the deliberate exclusions).
4. **Sanitizers stay clean**: `tools/sanitize.sh` (ASan+UBSan, TSan,
   valgrind) — CI runs the ASan leg on every PR.
5. **Style**: match the surrounding code; keep every boolean decision at
   ≤ 6 conditions (clang-18 MC/DC instrumentation limit — use the
   `hasAny()`/named-bool patterns already in the code).

## Architecture ground rules

- Layering is linker-enforced: `domain` (Qt Core only, pure) ← `services`
  (REST/feeds) ← `ui`. Never add an upward include.
- Money-moving actions require explicit user confirmation and must never be
  triggered by advisory features (REQ-N-005).
- Secrets never enter the repository (REQ-N-004): keys live only in the
  git-ignored `apiKeyEtoro.json`.

## Commit messages

Short imperative subject, body explains the why. Reference requirement ids
(`REQ-…`) when the change affects specified behaviour.

## Repository topics and keywords (moved from the README)

Searchable subject tags for this repository. These are the GitHub **topics** —
keep them in sync with the repository settings (Settings → General → Topics, or
the `gh` command below), since GitHub search and the topic pages only index what
is configured there, not what a README mentions.

`qt` `qt6` `cpp` `cpp23` `cmake` `cross-platform` `desktop-application`
`trading` `etoro` `technical-analysis` `monte-carlo`
`static-analysis` `axivion` `misra` `clang-tidy` `cppcheck` `sanitizers`
`code-coverage` `mcdc` `requirements-traceability` `strictdoc` `aspice`
`functional-safety`

Apply them in one go (needs the GitHub CLI, `gh auth login` once):

```bash
gh repo edit MartinSch77/TradingApp \
  --add-topic qt --add-topic qt6 --add-topic cpp --add-topic cpp23 \
  --add-topic cmake --add-topic cross-platform --add-topic desktop-application \
  --add-topic trading --add-topic etoro --add-topic technical-analysis \
  --add-topic monte-carlo --add-topic static-analysis --add-topic axivion \
  --add-topic misra --add-topic clang-tidy --add-topic cppcheck \
  --add-topic sanitizers --add-topic code-coverage --add-topic mcdc \
  --add-topic requirements-traceability --add-topic strictdoc --add-topic aspice \
  --add-topic functional-safety
```

GitHub allows at most 20 topics per repository, so if it rejects the tail, drop
the least specific ones (`cpp`, `cmake`, `cross-platform`) first — the
quality-toolchain tags are what make this repository findable, since a
"Qt trading app" is common and a "Qt trading app with MISRA C++, MC/DC coverage
and requirements-as-code traceability" is not.
