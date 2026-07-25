# eToro SPX500 Trader (Qt)

A small Qt 6 desktop app to trade **SPX500** through the **official eToro public
API** ([api-portal.etoro.com](https://api-portal.etoro.com/), base URL
`https://public-api.etoro.com/api`).

It provides:

- a **live time chart** of the SPX500 price (Qt Charts);
- an **amount** field and a **leverage** selector;
- **BUY** and **SELL** buttons to open a market position;
- a table of **open trades** with live P/L and a **Close selected trade** button;
- an activity log.

If no API keys are configured it runs in a clearly-labelled **SIMULATION** mode
with a synthetic price feed, so it is fully usable before you have credentials.

## Build

Requires Qt 6 with the **Widgets**, **Network**, and **Charts** modules
(developed against Qt 6.10.2), CMake ≥ 3.21 and a C++17 compiler. The sources are
plain cross-platform Qt/C++ — the same code builds on Linux, Windows, and
Android; only the Qt kit and the packaging step differ.

### Linux / macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.2/gcc_64
cmake --build build
./build/TradingApp
```

### Windows (MSVC)

Install the Qt 6 **msvc2022_64** kit (with the Charts module), Visual Studio 2022
(or its Build Tools), and CMake. From a *Developer* command prompt:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
      -DCMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2022_64
cmake --build build
build\TradingApp.exe
```

Qt 6 uses the **Schannel** TLS backend on Windows, so HTTPS to the eToro API
works with no OpenSSL install. To run the `.exe` outside the build tree, see
[Packaging](#packaging-desktop) below.

### Android

Requires the Qt 6 **Android** kit (e.g. `android_arm64_v8a`), the Android SDK +
NDK, and a JDK. The simplest setup is to open the project in **Qt Creator** with
an Android kit selected, which fills in the SDK/NDK paths and toolchain. On the
command line, configure with the Android kit's `qt-cmake` wrapper:

```bash
~/Qt/6.10.2/android_arm64_v8a/bin/qt-cmake -S . -B build-android -G Ninja \
      -DQT_ANDROID_ABIS=arm64-v8a
cmake --build build-android --target apk     # produces the APK
```

`qt_add_executable()` builds the app as a shared library and `androiddeployqt`
packages it into an APK. The build **bundles OpenSSL** (fetched at configure
time — see [`CMakeLists.txt`](CMakeLists.txt)) because Android's Qt does not ship
it and HTTPS to eToro would otherwise fail.

Caveats specific to Android:

- The UI is **Qt Widgets** — a desktop-style layout. It runs on a phone but is
  not touch-optimised.
- The APK is sandboxed and has no working directory or `ETORO_*` environment, so
  `config.json` / env-var configuration does not apply and the app starts in
  **SIMULATION** mode. To trade for real you would have to ship credentials into
  the app's data dir (bundle a `config.json` via `QT_ANDROID_PACKAGE_SOURCE_DIR`,
  or write to `AppConfigLocation`) — private builds only; never publish keys.

### Packaging (desktop)

`cmake --install build --prefix dist` produces a self-contained folder with the
binary and every Qt library/plugin it needs, ready to zip or hand to `cpack`. It
runs **windeployqt** on Windows and **macdeployqt** on macOS automatically.

## Configuration / API keys

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
| Symbol         | `symbol`         | `ETORO_SYMBOL`         | `SPX500`                            |
| Base URL       | `baseUrl`        | `ETORO_BASE_URL`       | `https://public-api.etoro.com/api`  |
| Order currency | `orderCurrency`  | `ETORO_ORDER_CURRENCY` | `usd`                               |
| Leverage       | `defaultLeverage`| `ETORO_LEVERAGE`       | `1`                                 |
| Poll interval  | `pollIntervalMs` | `ETORO_POLL_MS`        | `5000`                              |

### Demo vs. live (real money)

- **`mode: "demo"`** (default) trades your eToro **virtual** account — no real
  money. The app uses the `/demo/` endpoint variants.
- **`mode: "real"`** trades **real money**. This is opt-in only: the mode badge
  turns red, the window title says *LIVE*, and every buy/sell/close asks for
  confirmation first.

The app will never place a real-money order unless you both provide credentials
*and* explicitly set `mode` to `real`.

## eToro endpoints used

All requests send the documented `x-api-key`, `x-user-key`, and per-request
`x-request-id` (UUID) headers. See [`src/services/EtoroClient.cpp`](src/services/EtoroClient.cpp).

| Purpose            | Method & path | Verified live |
|--------------------|---------------|:---:|
| Resolve instrument | `GET /v1/market-data/search?internalSymbolFull=SPX500&fields=…` | ✅ (SPX500 = id `27`) |
| Chart history      | `GET /v1/market-data/instruments/{id}/history/candles/{dir}/{interval}/{count}` | ✅ |
| Live price         | `GET /v1/market-data/instruments/rates?instrumentIds={id}` | ✅ |
| Open position      | `POST /v2/trading/execution/{demo\|}/orders` | ⚠️ needs trading token |
| Close position     | `POST /v1/trading/execution/{demo\|}/market-close-orders/positions/{positionId}` | ⚠️ needs trading token |
| Portfolio          | `GET /v1/trading/info/{demo\|}/portfolio` | ⚠️ needs trading token |

Confirmed real quirks (already handled in code):
- Search **ignores** a free-text `query=`; filter with **`internalSymbolFull`**. The
  first result row `{"instrumentId":-100000}` is a placeholder and is skipped.
- Candles are **nested**: `{ candles: [ { instrumentId, candles: [ {fromDate,open,high,low,close} ] } ] }`.
- Rates fields are `lastExecution` / `bid` / `ask` (no `currentRate`/`close`).

**Note on JSON schemas.** Order and portfolio response schemas are only visible in
the authenticated reference. The client parses responses **defensively** (tries
several field names, unwraps `data`) and logs anything it cannot parse to the
Activity panel. Adjust the `pick(...)` key lists in `src/services/EtoroClient.cpp`, and the
candle interval/direction/count near the top of `EtoroClient.h`
(`m_candleInterval`, `m_candleDirection`, `m_candleCount`) if needed.

### Troubleshooting: HTTP 403 `InsufficientPermissions`

Market-data calls succeed but trading/portfolio calls return
`403 {"errorCode":"InsufficientPermissions"}`. This means your API token is an
**`UnregisteredApplication`** token without trading scope. Register/approve the
application in the API portal and regenerate the keys to get trading + portfolio
access; no code change is needed afterwards.

## Architecture & project layout

The code is organised in three layers, each built as its own target so the
dependency direction (UI → services → domain) is enforced by the linker: the
domain cannot reach the network, and the services cannot reach the widgets.
All layers are plain cross-platform Qt/C++, so the same split holds on Linux,
Windows and Android.

### `src/domain/` — pure trading logic (`trading_domain`, Qt Core only)

Deterministic functions with no I/O and no UI, in `namespace trading` —
independently unit-testable and shared by every view that shows a signal.

| File | Responsibility |
|------|----------------|
| [`Models.h`](src/domain/Models.h)                 | `Instrument`, `Candle`, `Position`, ... value types |
| [`Indicators.*`](src/domain/Indicators.h)         | SMA, RSI, MACD, Bollinger, stochastic, volatility, ROC |
| [`Forecasting.*`](src/domain/Forecasting.h)       | OLS regression, kNN analogs, Hurst, Monte-Carlo outlook |
| [`SignalEnsemble.*`](src/domain/SignalEnsemble.h) | The BUY/SELL indicator vote + VIX confidence haircut |
| [`DecisionEngine.*`](src/domain/DecisionEngine.h) | Weighted multi-source composite + AI evidence prompt |
| [`PositionMath.*`](src/domain/PositionMath.h)     | SL/TP amount↔rate maths, value-per-point, price decimals |
| [`EventInsight.*`](src/domain/EventInsight.h)     | Macro-event impact heuristics and descriptions |

### `src/services/` — integration (`trading_services`, adds Qt Network)

| File | Responsibility |
|------|----------------|
| [`Config.*`](src/services/Config.h)                   | Load keys/settings from JSON + env; demo/live decision |
| [`EtoroClient.*`](src/services/EtoroClient.h)         | The broker: eToro REST calls (rates, orders, portfolio, history) |
| [`SimulationEngine.*`](src/services/SimulationEngine.h) | Synthetic feed + virtual account (no-credentials fallback) |
| [`MarketFeeds.*`](src/services/MarketFeeds.h)         | Public web feeds: VIX, TradingView ratings, news |
| [`AiAdvisor.*`](src/services/AiAdvisor.h)             | Claude (Anthropic API) decision synthesis |
| [`JsonHttp.*`](src/services/JsonHttp.h)               | Shared reply/retry/JSON plumbing for all REST calls |
| [`EconomicCalendar.*`](src/services/EconomicCalendar.h) | Macro-economic calendar feed |

### `src/ui/` + `src/main.cpp` — presentation (Qt Widgets/Charts)

| File | Responsibility |
|------|----------------|
| [`MainWindow.*`](src/ui/MainWindow.h)         | Main window: trade panel, signals, positions, events |
| [`ScreenerDialog.*`](src/ui/ScreenerDialog.h) | Leverage screener window |
| [`PriceChart.*`](src/ui/PriceChart.h)         | Live time-vs-price Qt Charts widget |
| [`ChartView.*`](src/ui/ChartView.h)           | Interactive pan/zoom chart view |
| [`main.cpp`](src/main.cpp)                    | Composition root: builds the services, injects them into the UI |

## QA helper

`TRADINGAPP_SHOT=/path/out.png ./build/TradingApp` grabs the window to a PNG
after ~2.5 s and exits — handy for headless screenshots
(`QT_QPA_PLATFORM=offscreen`).

## Disclaimer

Trading involves risk of financial loss. This is example software provided as-is,
is not affiliated with or endorsed by eToro, and is not financial advice. Verify
every order in eToro's own interface. Use `demo` mode until you fully trust the
behaviour on your account.
