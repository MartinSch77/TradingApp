# TradingApp — Software Design (constructive counterpart)

@page design Software Design
@tableofcontents

Design element IDs (`DES-…`) are the constructive counterpart in the
traceability chain: requirement → design element → test. Each element names
the implementing classes/files and the requirements it satisfies
(`satisfies:` list is machine-read by `tools/trace_report.py`).

Architecture overview diagrams: see @ref architecture (PlantUML).

## Domain layer (`src/domain/`, target `trading_domain`)

| ID | Element | Implementation | satisfies |
|----|---------|----------------|-----------|
| DES-DOM-IND | Technical indicators — pure functions over close series | `Indicators.h/.cpp` (`trading::sma`, `rsi`, `stochasticK`, `emaSeries`, `macdHistogram`, `bollingerPercentB`, `volatilityPct`, `roc`, `returnsOf`, `meanReturn`) | REQ-F-005, REQ-N-002 |
| DES-DOM-FC | Statistical forecasting — regression, kNN analogs, Hurst, bootstrap Monte-Carlo with TP/SL barrier outcome split | `Forecasting.h/.cpp` (`linRegForecast`, `knnForecast`, `hurstExponent`, `monteCarlo`, `sigmoid`) | REQ-F-006, REQ-N-002 |
| DES-DOM-ENS | Directional ensemble vote + VIX confidence haircut | `SignalEnsemble.h/.cpp` (`computeEnsemble`, `applyVixHaircut`) | REQ-F-007 |
| DES-DOM-DEC | Multi-source composite decision engine incl. crowd tilt and market regime | `DecisionEngine.h/.cpp` (`computeDecisionRows`, `crowdTilt`, `marketRegime`, `newsSentimentScore`, `buildDecisionEvidence`) | REQ-F-008, REQ-F-009, REQ-F-020 |
| DES-DOM-PLAN | Costed trade planner — verdict, probabilities, risk factor, leverage recommendation, SL/TP geometry, cost bill | `TradePlan.h/.cpp` (`buildTradePlan`, `proposedSlFraction`, `recommendLeverage`) | REQ-F-010, REQ-F-011, REQ-F-012 |
| DES-DOM-POS | Position/money arithmetic (FX-free value-per-point identity, SL/TP amount↔rate) | `PositionMath.h/.cpp` | REQ-F-003, REQ-F-016 |
| DES-DOM-EVT | Calendar-event impact heuristics | `EventInsight.h/.cpp` (`parseNum`, `guessImpact`, `eventAbout`) | REQ-F-020 |
| DES-DOM-MODEL | Shared value types (Instrument, Candle, Position, ClosedTrade, InstrumentPnl, MonthlyPnl, WebRating, …) | `Models.h` | REQ-F-014, REQ-N-002 |

## Services layer (`src/services/`, target `trading_services`)

| ID | Element | Implementation | satisfies |
|----|---------|----------------|-----------|
| DES-SVC-CLIENT | eToro REST client: resolution, rates/candles, orders + confirmation, portfolio/P-L, trade-history pager with spread-cost estimation, fee/spread caches, tradeability inference | `EtoroClient.h/.cpp` | REQ-F-001, REQ-F-002, REQ-F-003, REQ-F-014, REQ-F-015, REQ-F-016, REQ-N-003 |
| DES-SVC-SIM | Self-contained simulation (synthetic feed, virtual account, SL/TP/trailing execution, closed-trade log) | `SimulationEngine.h/.cpp` | REQ-F-017 |
| DES-SVC-FEEDS | Public web feeds: VIX, TradingView ratings, news, CNN Fear & Greed, Yahoo reference quote | `MarketFeeds.h/.cpp` | REQ-F-009, REQ-F-019, REQ-F-020 |
| DES-SVC-HTTP | Shared JSON/HTTP plumbing with idempotent-GET retry/backoff | `JsonHttp.h/.cpp` | REQ-N-003 |
| DES-SVC-CFG | Layered configuration incl. split secrets file | `Config.h/.cpp` | REQ-F-018, REQ-N-004 |
| DES-SVC-CAL | Macro-economic calendar feed | `EconomicCalendar.h/.cpp` | REQ-F-020 |
| DES-SVC-AI | Claude decision synthesis (optional) | `AiAdvisor.h/.cpp` | REQ-F-008, REQ-N-005 |

## UI layer (`src/ui/`, `src/main.cpp`, target `TradingApp`)

| ID | Element | Implementation | satisfies |
|----|---------|----------------|-----------|
| DES-UI-MAIN | Main window: trade panel (3750 default, auto SL/TP), signals panel, decision window with trade plan + Apply, close-proposal watchdog, closed-trades summary + detail dialog, order guards | `MainWindow.h/.cpp` | REQ-F-003, REQ-F-004, REQ-F-012, REQ-F-013, REQ-F-014, REQ-F-015, REQ-N-005 |
| DES-UI-CHART | Price/change chart windows | `PriceChart.h/.cpp`, `ChartView.h/.cpp` | REQ-F-002 |
| DES-UI-SCREEN | Leverage screener dialog | `ScreenerDialog.h/.cpp` | REQ-F-008 |
| DES-UI-ROOT | Composition root, platform selection (WSL/xcb), QA screenshot hooks | `main.cpp` | REQ-N-001 |

## Deployment / build design

| ID | Element | Implementation | satisfies |
|----|---------|----------------|-----------|
| DES-BLD-CMAKE | Single CMake project: desktop install/deploy (windeployqt/macdeployqt), Android (OpenSSL bundling), iOS guards, CPack packaging, CTest test suite | `CMakeLists.txt`, `tests/CMakeLists.txt` | REQ-N-001 |
