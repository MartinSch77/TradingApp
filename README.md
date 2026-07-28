# eToro Trader (Qt)

![CI](https://github.com/MartinSch77/TradingApp/actions/workflows/ci.yml/badge.svg)
![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)

A Qt 6 desktop app to trade **eToro instruments** — indices (SPX500, NSDQ100,
GER40, …), forex, commodities and eToro's thematic baskets — through the
**official eToro public API**
([api-portal.etoro.com](https://api-portal.etoro.com/), base URL
`https://public-api.etoro.com/api`). SPX500 is merely the start-up default;
the instrument selector switches everything live.

**Purpose:** placing a trade with stop-loss and take-profit through the eToro
web interface takes quite a few steps. This app puts everything on one
screen — amount, leverage and auto-proposed SL/TP are always ready, so a
trade is two deliberate clicks away — and it shows upcoming market events
plus a battery of indicators and independent sources that suggest **when to
buy or sell** (and, just as importantly, when to stay out).

It provides:

- an **instrument selector** with a **live time chart** of the selected
  instrument (Qt Charts) and a leverage screener across all instruments;
- an **amount** field, a **leverage** selector and auto-proposed **SL/TP**;
- **BUY** and **SELL** buttons (double-press guarded) to open a market position;
- a table of **open trades** with live P/L, editable SL/TP and marked-close;
- a **decision window**: multi-source composite call per instrument (technical
  ensemble, TradingView, news, VIX regime, Fear & Greed, Yahoo intraday,
  optional Claude synthesis) plus a costed **trade plan**;
- **closed-trades history** (7–13 weeks) with cost accounting, a macro-economic
  **event calendar** with activity proposals, and an activity log.

If no API keys are configured it runs in a clearly-labelled **SIMULATION** mode
with a synthetic price feed, so it is fully usable before you have credentials.

## Build

The full quality pipeline runs natively on **both Linux and Windows**. Each
`*.sh` entry point has a one-to-one PowerShell counterpart; see
[docs/windows.md](docs/windows.md) for the complete tool mapping and the
Windows-specific notes.

On a naked Debian/Ubuntu Linux, `./setup.sh` installs every required tool
and dependency (compilers, CMake, Qt 6 incl. Charts via aqtinstall, the
clang-18/LLVM tooling, cppcheck/clazy/valgrind/lcov, Doxygen + Java,
StrictDoc/Doorstop) idempotently; `./setup.sh update` brings them to their
latest versions and `./setup.sh status` reports what is present. On Windows,
`.\setup.ps1` does the same through winget + pip + aqtinstall. License-bound
tools (Axivion Suite, Squish Coco) are detected and reported but must be
installed manually.

The repository has three top-level entry points:

```bash
./build_all.sh            # everything: app, tests, traceability, docs,
                          # coverage, static analysis, sanitizers, Axivion
./build_all.sh app        # ONLY the TradingApp executable (build/TradingApp)
./build_all.sh build test # any subset of stages, in order
./build_all.sh --skip axivion  # everything except the (slow) Axivion analysis
./clean_all.sh [--deep]   # remove everything generated
```

```powershell
.\setup.ps1                     # provision/verify the Windows toolchain
.\build_all.ps1                 # same stages, same order
.\build_all.ps1 build test      # any subset of stages
.\build_all.ps1 -Skip axivion   # everything except the (slow) Axivion analysis
.\clean_all.ps1 [-Deep]         # remove everything generated
```

Stages: `build test trace docs coverage analysis sanitize axivion` (default:
all, continuing past failing stages with a summary at the end); `app` and
`release` are extra stages that are only run when named, and `build_all.ps1`
additionally offers `vs` and `deploy`. For a different single CMake target:
`cmake --build build --target <name>`.

**No licence, no problem.** Stage outcomes are `ok` / `skipped` / `FAILED`. A
stage needing a tool that is license-bound (Axivion Suite, Squish Coco) or
otherwise absent reports **`skipped`** with a message saying what to install, and
does *not* fail the run — so the whole pipeline goes green on a machine with only
the free toolchain. Everything **open source** that the pipeline needs is
installed for you by `./setup.sh` / `.\setup.ps1`; `setup.sh status` and
`setup.ps1 status` list what is present, what is license-bound, and what has no
counterpart on the platform.

`build_all.ps1` selects the Qt kit itself (newest kit containing Qt6Charts,
MSVC preferred) and imports the Visual Studio developer environment into the
session, so no "x64 Native Tools" prompt is required. Override the kit with
`$env:QT_PREFIX` or `-QtKit mingw_64`.

Requires Qt 6 with the **Widgets**, **Network**, and **Charts** modules
(developed against Qt 6.11.1), CMake ≥ 4.2 and a C++23-capable compiler
(GCC 13+, Clang 17+, MSVC 19.38+). The sources are
plain cross-platform Qt/C++ — the same code builds on Linux, Windows, and
Android; only the Qt kit and the packaging step differ.

### Linux / macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
cmake --build build
./build/TradingApp
```

### Windows (MSVC)

Install the Qt 6 **msvc2022_64** kit (with the Charts module), Visual Studio 2022
(or its Build Tools), and CMake — or let `.\setup.ps1` do it. Then, from an
ordinary PowerShell prompt:

```powershell
.\build_all.ps1 app          # -> build\TradingApp.exe
.\build_all.ps1 build test   # app + tests + JUnit results
```

Or by hand, from a *Developer* command prompt:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64
cmake --build build
build\TradingApp.exe
```

The **MinGW** kit works too: `.\build_all.ps1 -QtKit mingw_64` (the matching
`C:\Qt\Tools\mingw*\bin` is put on PATH automatically).

#### Visual Studio IDE

`CMakeLists.txt` is the single description of the build, so the `.sln` is
**generated**, not committed:

```powershell
.\tools\make_vs_solution.ps1 -Open   # -> build-vs\TradingApp.sln
.\build_all.ps1 vs                   # same thing, as a named stage
```

The solution contains `TradingApp`, `trading_domain`, `trading_services` and all
12 `tst_*` projects in Debug/Release/RelWithDebInfo; `TradingApp` is the startup
project, the Qt DLL directory is already on the debugger's PATH, and
*Test → Run All Tests* works. Re-run the script after adding or removing source
files. Two `.sln`-free alternatives, both driven by
[CMakePresets.json](CMakePresets.json):

- **File → Open → Folder** on the repository root — Visual Studio offers the
  `windows-msvc-debug` and `visual-studio` presets directly.
- `cmake --preset windows-msvc-debug && cmake --build --preset windows-msvc-debug`
  from any shell. Linux has `linux-gcc-debug` / `linux-gcc-release` presets too.
  All presets take the Qt kit from `$QT_PREFIX`.

To make the built executable runnable on its own — Qt DLLs, the platform
plugin, the Schannel TLS backend and the compiler runtime copied next to it:

```powershell
.\build_all.ps1 deploy               # -> build\TradingApp.exe runs with nothing on PATH
.\tools\deploy_app.ps1 -IncludeTests # also make the tst_*.exe standalone
```

Qt 6 uses the **Schannel** TLS backend on Windows, so HTTPS to the eToro API
works with no OpenSSL install. For a distributable package rather than a
runnable build tree, see [Packaging](#packaging-desktop) below.

The Windows pipeline substitutes a few tools that do not exist there — MSVC
`/analyze` for `g++ -fanalyzer`, OpenCppCoverage for gcov/lcov, ASan (MSVC) plus
UBSan (clang-cl) for the combined GCC sanitizer build — and reports the genuine
gaps (clazy, TSan, valgrind) instead of hiding them. MC/DC coverage is measured
**twice**, by Squish Coco and by clang-cl/llvm-cov. Details, and the PowerShell
pitfalls worth knowing about, are in [docs/windows.md](docs/windows.md).

### Android

Requires the Qt 6 **Android** kit (e.g. `android_arm64_v8a`), the Android SDK +
NDK, and a JDK. The simplest setup is to open the project in **Qt Creator** with
an Android kit selected, which fills in the SDK/NDK paths and toolchain. On the
command line, configure with the Android kit's `qt-cmake` wrapper:

```bash
~/Qt/6.11.1/android_arm64_v8a/bin/qt-cmake -S . -B build-android -G Ninja \
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
| Username       | `username`       | `ETORO_USERNAME`       | *(empty)*                           |
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
| [`TradePlan.*`](src/domain/TradePlan.h)           | Costed trade proposal: verdict, P(win), risk factor, leverage, SL/TP, cost bill |
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
| [`PositionsModel.*`](src/ui/PositionsModel.h) | Open-trades table model, in-place re-price |
| [`TradeGauge.*`](src/ui/TradeGauge.h)         | Per-trade gauge window |
| [`Palette.h`](src/ui/Palette.h)               | Shared UI colors |
| [`main.cpp`](src/main.cpp)                    | Composition root: builds the services, injects them into the UI |

## QA helper

`TRADINGAPP_SHOT=/path/out.png ./build/TradingApp` grabs every visible window
to one PNG each (further windows get a `-1`, `-2`, … suffix) after 3000 ms and
exits — handy for headless screenshots (`QT_QPA_PLATFORM=offscreen`).
`TRADINGAPP_SHOT_OPEN=1` opens the decision and closed-trades windows first;
`TRADINGAPP_SHOT_DELAY_MS` overrides the capture delay.

## Topics / keywords

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

## Disclaimer

Trading involves risk of financial loss. This is example software provided as-is,
is not affiliated with or endorsed by eToro, and is not financial advice. Verify
every order in eToro's own interface. Use `demo` mode until you fully trust the
behaviour on your account.

## License

[MIT](LICENSE). Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md)
and [SECURITY.md](SECURITY.md) for the quality bar and how to report
vulnerabilities.
