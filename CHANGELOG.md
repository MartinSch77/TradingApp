# Changelog

Application-level release history (`vX.Y.Z` tags). See `process/CHANGELOG.md`
for the separately-versioned process framework's own history
(`process-vX.Y.Z` tags) — the two are independent baselines
(`process/strategies/configuration-management-strategy.md`).

Filed to close GitHub Issue #16 (QA nonconformance: root CHANGELOG.md
absent) — `docs/roadmap.md` remains the forward-looking plan; this file is
the realized-history counterpart `process/strategies/project-management-
strategy.md`'s planning-artefact composition names.

## v1.1.2

The first release since v1.0.6. `v1.1.0` and `v1.1.1` were tagged but never
published: v1.1.0's packaging died because Qt Graphs' CMake package needs
`qtquick3d` + `qtshadertools`, which no install step requested (fixed in
`f81d17d`), and v1.1.1's release gate then stopped on one cppcheck finding
that only an ONNX-Runtime-less build — the CI configuration — produces.
Everything below the first four bullets landed in those unpublished tags.

- Release gate: `FinBertSentiment`'s stub `scoreText` no longer trips
  cppcheck's `functionStatic` on builds without ONNX Runtime.
- Traceability: a `STATUS: superseded` requirement is traced THROUGH its
  successors (`tools/trace_report.py`, `tools/sdoc_to_md.py`) instead of
  being reported as untested; REQ-F-057 gained its dedicated test
  (TS-PM-006); the traceability-gate workflow, red on `main` since it was
  added, passes (the process model now names the human final approver).
- Issue #15 (Coco GUI coverage under Squish) re-scored as an accepted,
  monitored low risk — `process/risk-register.md` RISK-001 is the live
  record.
- CI actions: `actions/checkout` 7, `actions/setup-java` 6.0.0,
  `fsfe/reuse-action` 6 (Dependabot #25/#26/#27).
- Qt Graphs resolves in every CI and release job (qtquick3d/qtshadertools);
  the release gate's timeout is 90 minutes.
- ICA added as a second, independent clang-based analyzer beside Axivion
  (informational); Python unit tests with branch coverage for
  `tools/*.py` and `tools/ml/*.py`.
- Issue #20 (JsonHttp TSan race) fixed with `Qt::SingleShotConnection`;
  every `test-results/*.xml` consumer now reads the nested Squish results.
- Bot strategy redesign (7/N): `SwingPullbackStrategyV1` wired into
  `BotSimRunner` live — additive, off by default (`BotConfig::
  useSwingStrategy`); new `PaperTrader::paperStakeCeiling` extracted so the
  swing entry path shares the composite bot's own portfolio/margin/
  correlation budget rather than a second copy of it.
- REQ-F-004 (exposure-cap guard) and REQ-N-002 (pure domain layer,
  `tst_architecture.cpp`) closed the project's last two automated-test
  coverage gaps.
- Mull mutation-testing pilot backlog closed for `PositionMath.cpp`,
  `Money.cpp`, `ConfirmGate.cpp`.
- Added an ASPICE-mapped process framework (`process/`, `process-v0.1.0`):
  independent SUP.1 Quality Assurance distinct from engineering
  verification, 16 process specs, `tools/qa_report.py`, GitHub issue/PR
  template operationalization.
- `requirements/requirements.sdoc`'s schema extended with SOURCE/RATIONALE/
  PRIORITY/STATUS/ACCEPTANCE_CRITERIA/ISSUE fields.

## v1.0.6

Release-pipeline hardening: fixed analysis-stage failures found by a full
`build_all.sh` run, an optional CBMC proof for `priceDecimals`'s pure core,
two more libFuzzer targets (Ollama/Yahoo response parsers extracted to
domain), an AppImage reproducibility check, the OpenSSF Scorecard workflow
and README architecture diagram.

## v1.0.5

Release gates now cover the console front ends (analysis/test/sanitize
green); the bot defaults to trading SPX500/NSDQ100, with peripheral
instruments needing more conviction; `TradingAdvise`/`TradingPortfolioAdvise`
skip Android packaging like `TradingBot` already did.

## v1.0.4

Crowd-sentiment/AI feature branch merged; packaging fixes for what that work
changed (no Mimer in the AppImage, no console APK); the console detects it
is a POSIX-terminal program rather than failing on MSVC; decision-log
timestamps are UTC-stable across machine timezones.

## v1.0.3

GPL-3.0-or-later relicensing; REQ-F-035's nine independent confluence reads;
a README a first-time visitor can actually read; MC/DC coverage taken from
77.0% to ~88% across several rounds, each finding at least one real defect
along the way; Squish and Test Center wired for real; an early read on index
heavyweights.

## v1.0.2

Money counted exactly (`domain::Money`, REQ-N-008) rather than approximated
in floating point; the order path validated, armed, and recorded
end-to-end.

## v1.0.1

First tagged release.
