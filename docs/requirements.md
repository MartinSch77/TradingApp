# TradingApp — Software Requirement Specification (SRS)

@page requirements Software Requirements
@tableofcontents

Requirement IDs are stable and referenced from the design (`docs/design.md`),
the test specification (`docs/test_spec.md`), the test implementations
(`tests/tst_*.cpp`, tag `@verifies`) and the generated traceability matrix
(`docs/traceability.html`). Format: `REQ-F-xxx` functional, `REQ-N-xxx`
non-functional. Each requirement lists its ASPICE-style attributes:
verification method (T = test, A = analysis, I = inspection) and status.

## Functional requirements

| ID | Requirement | Verify |
|----|-------------|--------|
| REQ-F-001 | The app shall resolve a user-selected instrument symbol to its eToro instrument id and only enable trading once the resolution is confirmed. | T |
| REQ-F-002 | The app shall poll live prices for the selected instrument and display them in a chart and price read-out. | T |
| REQ-F-003 | The app shall place market orders with a configurable cash amount, leverage, stop-loss and take-profit (entered as account-currency amounts), guarded by a deliberate double-press confirmation. | T |
| REQ-F-004 | The app shall reject orders that would exceed a configured total open-exposure cap, and enforce a cooldown between consecutive orders. | T |
| REQ-F-005 | The app shall compute the classic technical indicators (SMA, EMA, RSI, stochastic %K, MACD histogram, Bollinger %B, volatility, ROC, mean return) as pure functions over a close-price series. | T |
| REQ-F-006 | The app shall provide statistical forecasts: OLS regression (slope, R²), k-nearest-neighbour analog forecast, Hurst exponent, and a bootstrap Monte-Carlo outlook reporting P(up), a 5–95% range, and the probabilities that a long/short hits take-profit before stop-loss and vice versa. | T |
| REQ-F-007 | The app shall combine the indicators into a directional ensemble (BUY / SELL / NEUTRAL with a confidence), and trim the confidence in high-VIX and imminent-event conditions. | T |
| REQ-F-008 | The app shall blend the available per-instrument sources (technical ensemble, TradingView rating, news sentiment, market regime, crowd sentiment) into a weighted composite call per instrument, renormalising weights over available sources, and rank instruments by confidence. | T |
| REQ-F-009 | The app shall obtain crowd sentiment (CNN Fear & Greed, 0–100) and tilt the composite with it: mildly with the crowd for ordinary readings, contrarian at extremes (≤20, ≥80). | T |
| REQ-F-010 | The app shall build a costed trade plan per instrument: a BUY/SELL/STAY-OUT verdict with reason, Monte-Carlo win/lose probabilities against the proposed barriers, a break-even comparison, a 1–5 risk factor with reasons, and a volatility-targeted leverage recommendation that keeps the loss-at-stop within 25% of the stake, stepped to the instrument's allowed leverage values and cap. | T |
| REQ-F-011 | The trade plan's cost bill shall account for half the spread on opening and half on closing, the per-night rollover fee, and the one-off ~3× weekend rollover whenever the holding window includes Friday–Sunday; the expected edge shall be reported net of these costs. | T |
| REQ-F-012 | The trade panel shall default the invest amount to 3750 (display currency) and auto-propose SL/TP from recent volatility (stop ≈ 1.5σ of the horizon move, TP = 1.5 × SL) until the user edits them; an instrument switch re-enables the automatics. | T |
| REQ-F-013 | The app shall propose (never execute) closing an open position when the live price leaves the prediction corridor against it or the ensemble flips against it with ≥60% confidence, with a per-position log cooldown. | T |
| REQ-F-014 | The app shall list closed trades over a selectable 7–13-week lookback with the API's net P/L and rollover fees plus estimated open/close spread costs (half current spread × notional per side), and summarise per instrument with a Costs column = spread costs + fees. | T |
| REQ-F-015 | The app shall infer per-instrument market-open state from quote freshness and block opening orders on closed markets. | T |
| REQ-F-016 | The app shall display account figures in EUR using the live EUR/USD rate and convert user inputs back to the account currency for the API. | T |
| REQ-F-017 | Without API credentials the app shall run fully functional against a self-contained simulation (synthetic feed, virtual account, closed-trade log). | T |
| REQ-F-018 | Configuration shall be layered: built-in defaults ← `config.json` (non-secret) ← `apiKeyEtoro.json` (secrets, git-ignored) ← environment variables. | T |
| REQ-F-019 | The app shall display an independent web reference quote (Yahoo Finance) for the selected instrument with its exchange timestamp and the delta versus the eToro rate. | T |
| REQ-F-020 | The app shall obtain market context from public feeds: CBOE VIX, TradingView technical ratings, per-instrument news headlines, and a macro-economic calendar with per-event impact heuristics. | T |

## Non-functional requirements

| ID | Requirement | Verify |
|----|-------------|--------|
| REQ-N-001 | The app shall build and run on Linux and Windows desktops with Qt ≥ 6.5, and shall be buildable for Android (APK via androiddeployqt) and iOS from the same CMake project. | A/I |
| REQ-N-002 | The domain layer shall be pure (Qt Core only, no I/O, no UI); layering (ui → services → domain) shall be enforced by the linker. | A/T |
| REQ-N-003 | REST access shall survive transient failures: idempotent GETs are retried on 429/5xx honouring Retry-After / RateLimit-Reset; non-GETs are never auto-retried. | T |
| REQ-N-004 | No secret (API key, user key) shall be stored in version control. | I/T |
| REQ-N-005 | Money-moving actions shall require explicit user confirmation (double-press) and shall never be triggered by advisory features (plans, watchdog, AI). | T/I |
