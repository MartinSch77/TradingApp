# Crowd Sentiment & AI subsystem

Status: **Phase 3 — first real providers (CFTC COT, FRED/VIX)** (REQ-F-039, REQ-F-040). This
document is updated per phase; it describes what exists today and what is deliberately deferred.

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

## Phase 3 components — the first real providers

The mock is now joined by three **real** network-backed providers. They share a small base,
`CrowdHttpProvider`, that carries the one async-HTTP pattern the rest of the app already uses —
`QNetworkAccessManager` + `JsonHttp`, which retries an idempotent GET on 429/5xx with backoff and
honours `Retry-After`, all off the GUI thread. A concrete provider therefore adds only its URL
and its parse; neither reimplements the networking nor is a clone of the other. `refresh()` is the
non-blocking network call; it emits `observationsReady` and caches the result for `fetch()`, and a
failed fetch or an unparsable body emits `providerError` while the app carries on.

- **`CftcCotProvider`** — the flagship, because it is free, keyless and has decades of history. It
  reads the CFTC *Traders in Financial Futures* report from the official public Socrata JSON API
  (`publicreporting.cftc.gov/resource/gpe5-46if.json`) and turns the latest release for an index's
  E-mini future into two `InstitutionalPositioning` observations — asset-manager net and
  leveraged-fund net (long − short) contracts. It honours the **publication lag**: `eventTime` is
  the Tuesday the report is *about*, `receivedTime` is the following Friday it was *released*, so a
  datum is never treated as known before it was. This series (`COT-ASSET-MGR-NET`) is exactly the
  institutional input the Phase 2 Crowd Score already consumes, so the score can now run on real
  institutional data.
- **`FredProvider`** — reads the CBOE volatility index close (`VIXCLS`) from FRED
  (`api.stlouisfed.org`) into a `Volatility` observation. FRED requires a **free** key, taken only
  from `TRADINGAPP_FRED_API_KEY`; without it the provider reports itself *unavailable* and makes no
  call. The key is only ever a request parameter — never source, never a log, never a fixture, and
  never interpolated into an error string (a test pins that last guarantee). A missing print (`"."`
  on a holiday) is a named error, never a zero VIX.
- **`IgSentimentProvider`** — the retail-positioning family (the one the Crowd Score reads
  **contrarian**): the percentage of IG clients positioned long in a market, from IG's official
  REST API. Strictly **optional** — it needs an IG account, and all three credentials (API key,
  identifier, password) must be present before it makes a single call; anything missing means
  `isConfigured()` is false, `refresh()` returns silently and the app carries on. IG authenticates
  per **session**: `POST /session` (VERSION 2) answers the `CST` and `X-SECURITY-TOKEN` response
  *headers*, which every later request carries; the provider logs in once and reuses the tokens
  until they age out (~6 h), and the login POST is never auto-retried (it is not idempotent),
  while the sentiment GET keeps the shared base's retry. No credential is ever committed, logged
  or interpolated into an error string — tests pin the wire shape (key and tokens as headers, the
  identifier in the login body, nothing in a URL) and the no-leak guarantee.

**Licences.** CFTC data is a US-government public-domain work (no key, no redistribution limit).
FRED data is redistributed under its published terms; the personal API key is never committed.
IG Client Sentiment is fetched over IG's official documented REST API on the user's own account
and is not redistributed; use is subject to IG's own terms, which is one reason the provider is
opt-in. All three are documented JSON APIs — no scraping, and no commercial dataset, credential
or personal datum is ever committed. Alpha Vantage and Twelve Data need keys and are deferred
within Phase 3 until their credentials and licence acceptance are in place.

The providers are driven in tests against the in-process `MockHttpServer` via
`setEndpointBaseForTesting` — valid parse, the UTC lag, unknown instrument, empty/malformed body,
a hard 500 (retried), a 429 (`Retry-After`, retried), the IG session handshake and reuse, and the
no-key/unavailable and no-leak paths (`tst_crowdproviders`, TS-CROWD-009…014). No test touches
the real network.

## Configuration

Provider credentials are read from environment variables or the **git-ignored**
`apiKeyEtoro.json` (the same layered mechanism as the eToro keys — JSON first, environment
overrides; never source, logs or fixtures). Missing keys are a recoverable "unavailable", not a
failure — every keyed provider is optional, and the subsystem runs without any of them.

| Variable | `apiKeyEtoro.json` key | Provider | Phase |
|---|---|---|---|
| *(none)* | — | CFTC COT — free, no key | 3 (done) |
| `TRADINGAPP_FRED_API_KEY` | — | FRED/VIX (free key) | 3 (done) |
| `TRADINGAPP_IG_API_KEY` | `igApiKey` | IG Client Sentiment (optional) | 3 (done) |
| `TRADINGAPP_IG_IDENTIFIER` | `igIdentifier` | IG account username | 3 (done) |
| `TRADINGAPP_IG_PASSWORD` | `igPassword` | IG account password | 3 (done) |
| `TRADINGAPP_IG_DEMO` | `igDemo` (bool) | IG demo account (`demo-api.ig.com`) | 3 (done) |
| `TRADINGAPP_ALPHA_VANTAGE_API_KEY` | — | Alpha Vantage (prices/news) | 3 |
| `TRADINGAPP_TWELVE_DATA_API_KEY` | — | Twelve Data (prices) | 3 |
| `TRADINGAPP_FINNHUB_API_KEY` | — | Finnhub social sentiment | 6 |
| `TRADINGAPP_REDDIT_CLIENT_ID` / `_SECRET` | — | Reddit | 6 |

To **opt in** to IG sentiment, add `igApiKey`, `igIdentifier` and `igPassword` to your
git-ignored `apiKeyEtoro.json` (or export the environment variables). Leave them out and the
provider simply reports itself unavailable — nothing else changes. No scraping where no public
API is offered; no commercial dataset, credential or personal datum is ever committed.

## Deferred to later phases

- **Phase 3** — CFTC COT, FRED/VIX and IG Client Sentiment **done** (above). Still deferred
  within the phase: a market-data provider (Alpha Vantage / Twelve Data — or reusing the app's
  existing keyless Yahoo feed), which needs a key or a design decision before it can be wired.
- **Phase 4** — the offline Python training pipeline (`tools/ml/`): dataset build, LONG/NO_TRADE/
  SHORT labels, logistic-regression + XGBoost baselines, purged walk-forward validation, ONNX
  export. Python stays an **offline** tool, never a runtime dependency of the C++ app.
- **Phase 5** — ONNX Runtime inference in C++ behind a mock-able interface (optional; the app
  builds without it).
- **Phase 6** — FinBERT text→sentiment features.
- **Phase 7** — the Crowd & AI dashboard and optional Ollama *explanations* (never prices,
  probabilities, stops or sizing).
