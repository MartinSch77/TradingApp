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
