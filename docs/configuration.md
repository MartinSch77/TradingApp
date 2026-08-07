# Configuration and API keys

@page configuration Configuration, API keys, and the demo/real-money gates
@tableofcontents

Moved out of the README so the front page stays readable. This is the whole story of
how the application is configured and how it is kept away from real money until you
deliberately let it near.

The settings are split into two files so the repo never carries a secret:

- `config.json` — non-secret settings (mode, symbol, leverage, …); committed.
- `apiKeyEtoro.json` — the API keys only, looked up beside `config.json`;
  **git-ignored — never commit it**.

1. Sign in at **[api-portal.etoro.com](https://api-portal.etoro.com/)** →
   **Settings → Trading → API Key Management** → **Create New Key**.
2. Copy `apiKeyEtoro.example.json` to `apiKeyEtoro.json` (next to the binary /
   `config.json`, or beside the file the `ETORO_CONFIG` env var points at) and
   fill in `apiKey` / `userKey`.

Config resolution order (later wins): built-in defaults → `config.json` →
`apiKeyEtoro.json` → environment variables. Any field can also be set via env
var:

| Setting        | JSON key         | Env var                | Default                             |
|----------------|------------------|------------------------|-------------------------------------|
| API key        | `apiKey`         | `ETORO_API_KEY`        | *(empty → simulation)*              |
| User key       | `userKey`        | `ETORO_USER_KEY`       | *(empty → simulation)*              |
| Mode           | `mode`           | `ETORO_MODE`           | `demo`                              |
| Username       | `username`       | `ETORO_USERNAME`       | *(empty)*                           |
| Symbol         | `symbol`         | `ETORO_SYMBOL`         | `SPX500`                            |
| Base URL       | `baseUrl`        | `ETORO_BASE_URL`       | `https://public-api.etoro.com/api`  |
| Order currency | `orderCurrency`  | `ETORO_ORDER_CURRENCY` | `usd`                               |
| Leverage       | `defaultLeverage`| `ETORO_LEVERAGE`       | `1`                                 |
| Poll interval  | `pollIntervalMs` | `ETORO_POLL_MS`        | `5000`                              |

## Demo vs. live (real money)

- **`mode: "demo"`** (default) trades your eToro **virtual** account — no real
  money. The app uses the `/demo/` endpoint variants.
- **`mode: "real"`** trades **real money**. This is opt-in only: the mode badge
  turns red, the window title says *LIVE*, and every buy/sell/close asks for
  confirmation first.

The app will never place a real-money order unless you both provide credentials
*and* explicitly set `mode` to `real`.

### The machinery behind a real order (REQ-N-008, REQ-N-009)

The double-press keeps a human in the loop for one action. It does not make the
request *correct*, and it leaves no record. Four separate pieces do that, built
and tested before anything is wired to them:

- **[`Money`](../src/domain/Money.h)** — every amount that can move real money is an
  integer number of minor units plus its currency, with **one** named lossy
  conversion (`fromDouble`, rounding half away from zero) and exact arithmetic
  after it. Mixing currencies yields an amount that reports itself **invalid**
  rather than a plausible wrong number, and comparison across currencies is
  *unordered*, so no cap check can pass by accident.
- **[`OrderRequestValidator`](../src/domain/OrderRequestValidator.h)** — a pure check
  that **refuses rather than repairs**: an amount that is not the validated stake,
  a leverage off the instrument's ladder, an order currency that is not the
  account's (eToro accepts that one and rejects it at execution), a units count
  over the per-order cap, a stop bigger than the money at risk, a limit trigger on
  the wrong side that would fill at once. Every refusal carries a stable code
  beside its sentence, because a reason only a human can read cannot be counted.
- **[`LiveArm`](../src/services/OrderGateway.h)** — an explicit, **time-bounded**
  armed state carrying the per-order and per-day caps it was granted under, plus a
  **sticky kill switch**: one action disarms immediately, outranks every other
  refusal, and stays tripped until it is explicitly cleared, so a panic action
  cannot be undone by the next timer tick.
- **`IOrderGateway` + `FakeOrderGateway` + `OrderAudit`** — the seam that makes the
  send testable at all (the fake records exactly what it was asked to do), and an
  append-only JSON-Lines record of **every** attempt — sent, refused by validation,
  refused by the arming state, rejected by the broker — with the order's
  fingerprint, both timestamps, the request id and the broker's verbatim answer.

`guardedSend()` is the single composed entry point, because the three guarantees
are only guarantees when none of them can be skipped individually. Wiring this to
a live account is still a separate, deliberate act under REQ-N-005 — the machinery
exists so that act does not have to be written under time pressure.
