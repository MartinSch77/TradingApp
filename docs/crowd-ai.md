# Crowd Sentiment & AI subsystem

Status: **Phase 2 — transparent Crowd Score** (REQ-F-039, REQ-F-040). This document is updated
per phase; it describes what exists today and what is deliberately deferred.

> **These signals are experimental.** The subsystem produces, at most, paper-trading and
> advisory output. It is **not financial advice**, past performance does not predict future
> performance, sentiment is unreliable on its own, and probabilities can be wrong. Real-money
> automation is intentionally excluded — risk management stays deterministic and outside any AI
> model, and the existing double-press human gate (REQ-N-005) is the only path to a real order.

## Why a data foundation first

Before any model can be trusted, the application needs **reliable, timestamped data** and a
**transparent baseline** the model must demonstrably beat. Phase 1 builds only the data layer:
normalized observations, a provider seam with a mock, and SQLite persistence. No machine
learning, no external API calls, no automatic trading.

## Data flow (target; Phase 1 in **bold**)

```
providers  ──▶  **normalized Observation**  ──▶  **CrowdStore (SQLite, raw layer)**
                                                       │
                                                       ▼
                                        features ─▶ Crowd Score (Phase 2)
                                                       │
                                                       ▼
                                     trained model (ONNX, Phase 4–5)
                                                       │
                                                       ▼
                              deterministic risk checks ─▶ paper proposal ─▶ Qt UI
```

## Phase 1 components

| Layer | File | Responsibility |
|---|---|---|
| domain | `src/domain/CrowdObservation.{h,cpp}` | the normalized, immutable `Observation` value type |
| services | `src/services/CrowdProvider.h` | the `ICrowdProvider` seam (name / category / isConfigured / fetch) |
| services | `src/services/MockCrowdProvider.{h,cpp}` | deterministic offline provider spanning all families |
| services | `src/services/CrowdStore.{h,cpp}` | SQLite persistence of the **raw** observation layer |

### The normalized observation

Every provider turns its raw payload into a stream of `Observation`s. Each carries its
instrument, family (`Source`), the concrete provider name, a series id, value + unit, a schema
version, quality flags, and — critically — **two UTC timestamps**:

- `eventTime` — what the datum is **about** (a COT report is about a Tuesday);
- `receivedTime` — when it became **known** (that report publishes the following Friday).

Keeping both is the single most important guard against look-ahead bias: a datum must never be
treated as known before its `receivedTime`. A datum that could not be measured is `valid =
false` and is **never read as zero**; freshness is `live` / `stale` / `absent` against a
configured threshold, with `absent` distinct from `stale`.

### The provider seam

One interface, `ICrowdProvider`, mirroring the existing `OrderGateway` seam rather than a fan of
near-identical per-source interfaces. Real providers (Phase 3) fetch **asynchronously** over Qt
Network off the GUI thread; the interface is a plain pull so a mock and recorded fixtures drive
it in tests. A missing credential is a recoverable **`available = false`**, not a crash.

`MockCrowdProvider` is deterministic — the same instrument on the same UTC day yields the same
observations (a seeded generator, not Qt's per-process-randomised `qHash`) — spans every family,
and models the **CFTC publication lag** so leakage handling is exercised from Phase 1.

### Persistence (SQLite)

`CrowdStore` keeps the **raw** observation layer only. Features, labels, predictions and realized
outcomes are **separate tables** added in their own phases — never one oversized table. Times are
stored as ISO-8601 UTC strings; `(source_name, series_id, instrument, event_time)` is UNIQUE, so
re-fetching the same datum is an idempotent no-op (`INSERT OR IGNORE`). A `schema_meta` table
records the schema version for deliberate migration. Pass `":memory:"` for tests.

## Phase 2 components — the transparent Crowd Score

| Layer | File | Responsibility |
|---|---|---|
| domain | `src/domain/RollingZScore.{h,cpp}` | past-only z-score normalization (`zScore`, `RollingZScore`) |
| domain | `src/domain/CrowdScore.{h,cpp}` | the pure rule-based score (`crowdScore`, config, result) |
| services | `src/services/CrowdScoreBuilder.{h,cpp}` | `buildCrowdScore` — store observations → readings → score |
| services | `src/services/CrowdStore.{h,cpp}` (v2) | z-history query + `crowd_scores` persistence layer |
| ui | `src/ui/CrowdScoreModel.{h,cpp}` | Qt view-model (computes nothing; binds the result) |

`CrowdScore = 0.35·retail + 0.30·options + 0.20·institutional + 0.15·social` — **weights are
hypotheses, configurable, not validated rules.** Each family is a **past-only z-score** of its
latest datum against its own history (no look-ahead). **Sign convention:** a positive component z
is bullish, **except retail, which is contrarian** — a crowd that is heavily long is a *bearish*
input, so `crowdScore` negates it; options is oriented in the builder (a high put/call ratio is
bearish). Missing families are **excluded and named**, never zero; the result carries **coverage**,
a freshness-weighted **confidence**, per-factor **contributions**, warnings and a **version**, and
is persisted with its input snapshot in the separate `crowd_scores` table. It is **not** a
probability and it does not trade — it is the transparent baseline a model must beat.

## Configuration (for later phases — no keys are used in Phase 1 or 2)

Provider credentials will be read from environment variables (never source, logs or fixtures).
Missing keys are a recoverable "unavailable", not a failure. Planned names:

| Variable | Provider | Phase |
|---|---|---|
| *(none)* | CFTC COT, FRED/VIX — free, no key | 3 |
| `TRADINGAPP_ALPHA_VANTAGE_API_KEY` | Alpha Vantage (prices/news) | 3 |
| `TRADINGAPP_TWELVE_DATA_API_KEY` | Twelve Data (prices) | 3 |
| `TRADINGAPP_IG_API_KEY` | IG Client Sentiment | 3 |
| `TRADINGAPP_FINNHUB_API_KEY` | Finnhub social sentiment | 6 |
| `TRADINGAPP_REDDIT_CLIENT_ID` / `_SECRET` | Reddit | 6 |

The first, licence-safe providers are **CFTC Commitments of Traders** and **FRED** (including the
volatility index) — both free, both with long public history. No scraping where no public API is
offered; no commercial dataset, credential or personal datum is ever committed.

## Deferred to later phases

- **Phase 3** — real CFTC/FRED (and one market-data) providers with async networking, retry,
  rate-limit handling and recorded fixtures.
- **Phase 4** — the offline Python training pipeline (`tools/ml/`): dataset build, LONG/NO_TRADE/
  SHORT labels, logistic-regression + XGBoost baselines, purged walk-forward validation, ONNX
  export. Python stays an **offline** tool, never a runtime dependency of the C++ app.
- **Phase 5** — ONNX Runtime inference in C++ behind a mock-able interface (optional; the app
  builds without it).
- **Phase 6** — FinBERT text→sentiment features.
- **Phase 7** — the Crowd & AI dashboard and optional Ollama *explanations* (never prices,
  probabilities, stops or sizing).
