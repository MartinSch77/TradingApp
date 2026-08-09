# Crowd Sentiment & AI subsystem

Status: **all seven phases complete, including the Phase 7 explanations** (REQ-F-039 …
REQ-F-045; Phase 6 was deliberately taken after Phase 7). This document describes what exists
today and what is deliberately deferred — see the deferred list at the end.

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
              trained model (ONNX — **trained offline in Phase 4, loaded in-process in Phase 5**)
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

## Phase 4 components — the offline training pipeline (REQ-F-041)

| Layer | File | Responsibility |
|---|---|---|
| tools | `tools/ml/crowd_dataset.py` | dataset build (as-of joins, labels, manifest) + purged walk-forward splits + price fetch — **stdlib-only** |
| tools | `tools/ml/train_crowd_model.py` | logistic-regression + XGBoost baselines, fold evaluation beside named baselines, ONNX export |
| tools | `tools/ml/requirements.txt` | the optional venv's pinned floors, provisioned by `./setup.sh ml` |

Python is an **offline development-time tool, never a runtime dependency**: the app builds, runs
and passes its tests with none of this installed, and nothing the pipeline produces places a
trade. The dataset/split half is deliberately **stdlib-only** (sqlite3, csv, json), so the rules
that make the dataset honest are testable on every machine; only the model-fitting half needs
the optional environment (`./setup.sh ml`, Windows `.\setup.ps1 ml`, venv at
`~/.local/tradingapp-ml`, override with `ML_VENV_DIR`) and **exits 3 ("skipped") writing
nothing** without it — the same convention as every licence-bound stage.

**The dataset build** reads the very SQLite store the app writes (so the schema contract between
the languages is pinned by a test that drives both, TS-ML-001) and joins every series — the
Phase 2 score's four families plus both COT legs and the VIX level — to each decision time **as
of its received time**: the COT report about a Tuesday, released Friday, is absent from
Wednesday's row. Per-series z-scores are normalized only against values received *before* the
datum (the Phase 2 past-only rule); a missing series is an **empty cell beside a 0/1 `_measured`
marker**, never a zero. Labels are **LONG / NO_TRADE / SHORT** from the forward return over a
configurable horizon (default 5 rows) with a **dead zone** representing the round-trip cost
(default 0.25%): a move that would not clear its own cost is a NO_TRADE — staying out is an
outcome the model is taught, not a failure — and rows whose horizon outruns the price history
are dropped, never invented. Outputs carry no wall-clock, so the same inputs give
**byte-identical files**; the versioned feature manifest is **append-only** and consumers match
columns **by name**. Prices come from a `date,close` CSV (`fetch-prices` pulls daily closes from
the same keyless Yahoo chart API the app already uses; a proper historical backfill must set
`received_time` to the official publication schedule, which the CFTC provider already computes).

**Validation is purged walk-forward**: contiguous validation blocks over the time-ordered tail,
training only on earlier rows, and any training row whose **label window + embargo** reaches
into the block is **purged** — forward-return labels overlap in time, and a random split would
let the model see the future and report a fiction. Every fold's numbers sit beside **named
baselines on identical rows**: the training block's majority class, always-NO_TRADE, and the
transparent Phase 2 crowd-score sign — so "the model beats the baseline" is measured, never
felt. The trainer refuses (exit 3) a dataset below `--min-samples` or with one label class,
imputes missing values with the **training fold's** medians (the `_measured` flags keep absence
representable), and writes `training-report.json` with per-fold and mean metrics plus the
library versions.

**The export** refits both models on the full record (the report keeps the honest walk-forward
numbers) and writes `crowd-logreg.onnx` + `crowd-xgb.onnx` carrying, as ONNX metadata, the
feature names in order, the **imputation medians a consumer must apply**, the class order and
the manifest version — the Phase 5 C++ inference contract. Each graph is run through
onnxruntime and compared to the trained model's own probabilities **before anything reaches
disk**; a disagreement is a reported failure and no file. Verified end to end on this machine:
walk-forward balanced accuracy 0.91 (XGBoost) / 0.81 (logistic regression) against 0.50
(majority) on a constructed learnable fixture, parity ≤ 1e-6 (`tst_crowdml`, TS-ML-001…005).

## Phase 5 components — optional in-process inference (REQ-F-042)

| Layer | File | Responsibility |
|---|---|---|
| domain | `src/domain/CrowdInference.{h,cpp}` | the pure contract: metadata parse, by-name assembly + counted imputation, probability shaping |
| services | `src/services/CrowdModel.{h,cpp}` | the `ICrowdModel` seam + `OnnxCrowdModel` (ONNX Runtime, or an honest stub) |

The app can now **load and score the very files the pipeline exports** — behind a seam,
`ICrowdModel`, mirroring the provider seam, so consumers and tests run on a double and never
need the runtime. The dependency is **optional at build time**: CMake resolves ONNX Runtime
from `ONNXRUNTIME_ROOT` (environment or cache), falling back to the directory `./setup.sh ml`
provisions (`~/.local/onnxruntime`; the same mode installs it beside the training venv, on
Windows `.\setup.ps1 ml` fetches the win-x64 build). Without it the **same class compiles as a
stub**: `available()` is false and `status()` names the remedy — a visibly absent capability,
never a broken-looking one, and never a build failure.

**The model's own embedded metadata drives everything**, so the trainer and the consumer cannot
drift apart silently: inputs are matched **by name** against the embedded feature list, a
missing or non-finite input carries the trainer's **embedded median** — with the number of
imputed features **reported**, because a prediction made mostly of fill-ins is a weaker claim —
and the probability columns are labelled from the embedded class order, never by assumption. A
model whose metadata is absent, unparsable or inconsistent is **refused with a reason**; so is
an answer that does not form a probability distribution (repairing one would invent an
opinion). Every failure — missing file, junk graph, a runtime error mid-score — is a named
result while the app carries on, and a failed load leaves **no half-usable session**.

Verified end to end on this machine (`tst_crowdmodel`, TS-INF-004/005): the pipeline's XGBoost
export loads in-process and, scored over every dataset row with features assembled by name from
the CSV, reproduces its fit — label agreement well above 0.7 on the learnable fixture — and a
caller supplying **nothing** still gets an honest answer with every feature counted as imputed.
The prediction is **evidence only**: nothing here wires it to a trade, and any later consumer
stays paper/advisory behind the deterministic risk rules with the REQ-F-037 measurement
discipline in front of any probability claim.

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
| `TRADINGAPP_FINBERT_DIR` | — | local text-sentiment model directory (no key — a local model) | 6 (done) |
| `TRADINGAPP_FINNHUB_API_KEY` | — | Finnhub social sentiment (alternative social source) | deferred |
| `TRADINGAPP_REDDIT_CLIENT_ID` / `_SECRET` | — | Reddit (alternative social source) | deferred |

To **opt in** to IG sentiment, add `igApiKey`, `igIdentifier` and `igPassword` to your
git-ignored `apiKeyEtoro.json` (or export the environment variables). Leave them out and the
provider simply reports itself unavailable — nothing else changes. No scraping where no public
API is offered; no commercial dataset, credential or personal datum is ever committed.

## Phase 7 components — the dashboard and the collection loop (REQ-F-043)

| Layer | File | Responsibility |
|---|---|---|
| services | `src/services/CrowdCollector.{h,cpp}` | the collection loop: providers → store → score → optional model |
| ui | `src/ui/CrowdDashboardWindow.{h,cpp}` | the dashboard view (computes nothing; "Crowd…" button in the main window) |

The subsystem now **runs**: `CrowdCollector` owns the three real providers, asks the
**configured** ones for SPX500/NSDQ100 every 30 minutes (crowd data is slow-moving), persists
observations idempotently into `crowd.db` beside the bot's books, recomputes and persists the
transparent crowd score, and — when an exported model is present (`TRADINGAPP_CROWD_MODEL`, or
`crowd-model.onnx` in the app config dir) — scores it through the Phase 5 seam. An unconfigured
provider is shown **unavailable in words** and asked nothing; a provider failure becomes a named
status, never a crash. The model inputs the collector cannot compute in-process — the four
price-context features — are **left missing on purpose**: recomputing them in C++ would be a
second implementation of the trainer's arithmetic, free to drift, so the model imputes them
with its own embedded medians and the dashboard shows the count ("N inputs imputed").

The dashboard is **evidence only**: provider states, the score's own headline with its warnings
(missing families named), the model verdict with labelled probabilities and the imputation
count, and a disclaimer that is part of the layout. No trading affordance exists in the window,
and the collector holds no broker object — there is no route from here to an order.

**Explanations (REQ-F-045, done).** The optional local model (the same REQ-F-030 Ollama
machinery — no cloud, no key) can put the evidence the dashboard currently shows into plain
words: the request carries the SHOWN text, the answer is **displayed and consumed by
nothing** — no parser, no decision path — under a caveat that is part of the text, and the
model is instructed to give no prices, targets, sizes, stops, probabilities or buy/sell
instructions (the safety property is the wiring, not the instruction). Without a configured
model the box is disabled with the remedy named; failures are named errors, never a blank
(`tst_ollamaadvisor`, TS-OLLAMA-009).

## Phase 6 components — local text sentiment for the social family (REQ-F-044)

| Layer | File | Responsibility |
|---|---|---|
| domain | `src/domain/WordPieceTokenizer.{h,cpp}` | faithful BERT WordPiece encoding (pure, testable everywhere) |
| services | `src/services/FinBertSentiment.{h,cpp}` | the ONNX text classifier: load, score, labelled meaning |
| tools | `tools/ml/export_finbert.py` | offline exporter: model.onnx + vocab.txt + labels.txt |

The **social family runs on measured data now**: the news headlines the app already fetches are
scored by a LOCAL financial-domain BERT classifier (default `ProsusAI/finbert`) and become
normal `NET-SENTIMENT` observations — no social-network API, no key, no scraping. The
capability is **doubly optional**: it needs the Phase 5 ONNX Runtime build AND a model
directory provisioned offline by `tools/ml/export_finbert.py` (which names the model's own
licence terms before fetching, and skips with exit 3 naming the install command when the
exporter stack — `optimum`, `optimum-onnx`, CPU `torch` — is absent; those are deliberately
NOT in requirements.txt). The app finds the directory via `TRADINGAPP_FINBERT_DIR` or
`finbert/` in its config dir; absent either piece, the dashboard's FinBERT row says
"not configured" and headlines keep flowing to the panels that already show them.

Three honesty rules: tokenization is **faithful to the model's own vocabulary** (WordPiece:
greedy longest-match, unknown words to `[UNK]` whole, `[CLS]`/`[SEP]` framing surviving
truncation — a mismatched tokenizer scores noise with confidence); the class meaning comes
from a **labels file exported beside the model**, never an assumed column order, and a label
set without positive/negative is refused; and the published number is the **net**
(P positive − P negative) over the scored batch, stored with its **event time quantized to the
hour** so the idempotent store turns every news re-poll into a no-op instead of a flood. The
tests drive a ~1 kB BERT-shaped fixture (`tests/make_finbert_fixture.py`), never the real
400 MB model (`tst_finbert`, TS-FB-001…004).

## Deferred to later phases

- **Phase 3** — CFTC COT, FRED/VIX and IG Client Sentiment **done** (above). Still deferred
  within the phase: a market-data provider (Alpha Vantage / Twelve Data — or reusing the app's
  existing keyless Yahoo feed), which needs a key or a design decision before it can be wired.
- **Phase 4** — the offline Python training pipeline (`tools/ml/`) **done** (above): dataset
  build, LONG/NO_TRADE/SHORT labels, logistic-regression + XGBoost baselines, purged
  walk-forward validation, ONNX export. Python stays an **offline** tool, never a runtime
  dependency of the C++ app.
- **Phase 5** — ONNX Runtime inference in C++ behind a mock-able interface **done** (above):
  optional at build time, the exported models' own metadata (feature names, imputation medians,
  class order) driving the inference so the two sides cannot drift apart silently.
- **Phase 6** — local text-sentiment features **done** (above; taken after Phase 7). Still
  deferred: the keyed social APIs (Finnhub, Reddit) as alternative sources.
- **Phase 7** — the dashboard, collection loop and Ollama explanations **done** (above).
  Still deferred: a real options-family provider — `PUT-CALL` has only the mock, so the
  score's options input runs unmeasured on real data. CBOE was probed on 2026-08-09: the
  daily put/call statistics have **no public JSON API** (the CDN answers 403, the statistics
  page is a JS application), and scraping is not used where no API is offered — the family
  stays deferred until a licensed source exists.
