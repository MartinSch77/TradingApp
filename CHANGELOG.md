# Changelog

Application-level release history (`vX.Y.Z` tags). See `process/CHANGELOG.md`
for the separately-versioned process framework's own history
(`process-vX.Y.Z` tags) — the two are independent baselines
(`process/strategies/configuration-management-strategy.md`).

Filed to close GitHub Issue #16 (QA nonconformance: root CHANGELOG.md
absent) — `docs/roadmap.md` remains the forward-looking plan; this file is
the realized-history counterpart `process/strategies/project-management-
strategy.md`'s planning-artefact composition names.

## Unreleased (since v1.0.6)

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
