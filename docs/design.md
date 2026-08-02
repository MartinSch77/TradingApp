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
| DES-DOM-DEC | Multi-source composite decision engine incl. crowd tilt, market regime and Yahoo intraday momentum | `DecisionEngine.h/.cpp` (`computeDecisionRows`, `crowdTilt`, `marketRegime`, `newsSentimentScore`, `intradayTilt`, `buildDecisionEvidence`) | REQ-F-008, REQ-F-009, REQ-F-020, REQ-F-022 |
| DES-DOM-PLAN | Costed trade planner — verdict with actionability gates (confidence floor after risk trims, win-rate clear of break-even by 2 SE, minimum net edge), probabilities, risk factor, leverage recommendation, SL/TP geometry, cost bill | `TradePlan.h/.cpp` (`buildTradePlan`, `proposedSlFraction`, `recommendLeverage`) | REQ-F-010, REQ-F-011, REQ-F-012 |
| DES-DOM-SCRIPT | Trade-script format: parser (all-or-nothing per file, per-line errors), leverage snap-to-next-lower, and the pure per-entry execution predicates (time window, SIGNALS = ensemble + AI agreement, unconfigured AI never places) | `TradeScript.h/.cpp` (`parseTradeScript`, `snapLeverage`, `scriptEntryShouldRest`, `scriptEntryExpired`) | REQ-F-028 |
| DES-DOM-POS | Position/money arithmetic (FX-free value-per-point identity, eToro's unrealised-P/L identity `units × (close − open) × conversion` with the quote-freshness bound, SL/TP amount↔rate) | `PositionMath.h/.cpp` | REQ-F-003, REQ-F-016, REQ-F-025 |
| DES-DOM-EVT | Calendar-event impact heuristics + advisory activity proposal (side + before/after timing) | `EventInsight.h/.cpp` (`parseNum`, `guessImpact`, `eventAbout`, `proposeActivity`) | REQ-F-020, REQ-F-023 |
| DES-DOM-MODEL | Shared value types (Instrument, Candle, Position, OrderRequest, PendingOrder, ClosedTrade, InstrumentPnl, MonthlyPnl, WebRating, …) | `Models.h` | REQ-F-014, REQ-F-027, REQ-N-002 |

## Services layer (`src/services/`, target `trading_services`)

| ID | Element | Implementation | satisfies |
|----|---------|----------------|-----------|
| DES-SVC-CLIENT | eToro REST client: resolution, rates/candles, market orders + confirmation, broker-side limit orders (orderType `mit` + triggerRate) with a pending-order registry, a pending-order registry fed by the broker's own portfolio breakdown (`clientPortfolio.orders[]`, so earlier/foreign orders are visible) with a dedicated 4 s status cycle (round-robin, rate-pool aware), a prompt post-placement check that surfaces eToro's own rejection reason, cancel, and adjust-by-cancel-and-re-place (targeting the order's own instrument), portfolio/P-L (live-portfolio position set, marked from a per-instrument quote book that one bulk rates call per tick keeps current for the shown AND held instruments, with a delayed rates row re-based on the instrument's live 1-minute candle bid), trade-history pager with spread-cost estimation, fee/spread caches, tradeability inference (quote timestamp advancing between polls — delay-tolerant) | `EtoroClient.h/.cpp` | REQ-F-001, REQ-F-002, REQ-F-003, REQ-F-014, REQ-F-015, REQ-F-016, REQ-F-025, REQ-F-027, REQ-N-003 |
| DES-SVC-SIM | Self-contained simulation (synthetic feed, virtual account, SL/TP/trailing execution, resting limit orders released on a trigger touch (adjustable, renumbered like the real re-placement), closed-trade log) | `SimulationEngine.h/.cpp` | REQ-F-017, REQ-F-027 |
| DES-SVC-FEEDS | Public web feeds: VIX, TradingView ratings (all 26 instruments — verified tickers, ETF/index proxies for thematic baskets, RUBBER n/a), news, CNN Fear & Greed, Yahoo reference quote + intraday series | `MarketFeeds.h/.cpp` | REQ-F-009, REQ-F-019, REQ-F-020, REQ-F-022 |
| DES-SVC-HTTP | Shared JSON/HTTP plumbing with idempotent-GET retry/backoff | `JsonHttp.h/.cpp` | REQ-N-003 |
| DES-SVC-CFG | Layered configuration incl. split secrets file | `Config.h/.cpp` | REQ-F-018, REQ-N-004 |
| DES-SVC-CAL | Macro-economic calendar feed | `EconomicCalendar.h/.cpp` | REQ-F-020 |
| DES-SVC-AI | Claude decision synthesis (optional) | `AiAdvisor.h/.cpp` | REQ-F-008, REQ-N-005 |

## UI layer (`src/ui/`, `src/main.cpp`, target `TradingApp`)

| ID | Element | Implementation | satisfies |
|----|---------|----------------|-----------|
| DES-UI-MAIN | Main window: trade panel (3750 default, auto SL/TP), limit-order panel (rate per side, independent double-press gate, resting-order table with each order's live instrument rate, click routing (Instr. cell switches the traded instrument, value cells open the SL/TP/trigger editor) and cancel), signals panel, decision window with trade plan + Apply (plan + Monte-Carlo run off the GUI thread via QtConcurrent), in-place verdict/edge explanations + Stay-out column, event activity proposals, close-proposal watchdog, closed-trades summary + detail dialog, order guards incl. the logged "Trade anyway" market-closed override | `MainWindow.h/.cpp` | REQ-F-003, REQ-F-004, REQ-F-012, REQ-F-013, REQ-F-014, REQ-F-015, REQ-F-021, REQ-F-023, REQ-F-026, REQ-F-027, REQ-N-005 |
| DES-UI-CHART | Price/change chart windows (price-axis labels and the price tag follow the shown instrument's magnitude, min 3 decimals) | `PriceChart.h/.cpp`, `ChartView.h/.cpp` | REQ-F-002 |
| DES-UI-POSMODEL | Open-trades model (allocation-free per-tick refresh; every row marked from its own instrument's live quote, a not-live mark shown as such; invested / P-L totals that sum the columns as shown; editors/marks survive polls; an open SL/TP cell editor is shielded from refreshes via the edit-guard delegate) | `PositionsModel.h/.cpp` | REQ-F-012, REQ-F-025, REQ-N-006 |
| DES-UI-GAUGE | Per-trade gauge window (QPainter dial: SL→open→TP scale, live needle, P/L read-out); opened from the Side cell of the open-trades table, Instrument cell switches the app to that instrument (click routing in `MainWindow.cpp`) | `TradeGauge.h/.cpp` | REQ-F-024 |
| DES-UI-SCREEN | Leverage screener dialog | `ScreenerDialog.h/.cpp` | REQ-F-008 |
| DES-UI-SCRIPT | Trade-script runner + window: entries executed as broker-side limit orders (REQ-F-027 machinery) with the load→ARM two-step commitment, dry-run logging while disarmed, cancel-on-disarm/expiry/signal-flip, exposure-cap gate shared with manual orders | `TradeScriptPanel.h/.cpp` | REQ-F-028 |
| DES-UI-ROOT | Composition root, platform selection (WSL/xcb), QA screenshot hooks | `main.cpp` | REQ-N-001 |

## Deployment / build design

| ID | Element | Implementation | satisfies |
|----|---------|----------------|-----------|
| DES-BLD-CMAKE | Single CMake project: desktop install/deploy (windeployqt/macdeployqt), Android (OpenSSL bundling), iOS guards, CPack packaging, CTest test suite | `CMakeLists.txt`, `tests/CMakeLists.txt` | REQ-N-001 |
