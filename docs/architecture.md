# Architecture

@page architecture Architecture (PlantUML)
@tableofcontents

## Layered component view

The dependency direction (ui → services → domain) is enforced by the linker:
each layer is a static library that cannot see the ones above it
(REQ-N-002, DES-BLD-CMAKE).

@startuml layers
skinparam componentStyle rectangle
package "TradingApp (ui layer, Qt Widgets/Charts)" {
  [MainWindow] as MW
  [PriceChart / ChartView] as PC
  [ScreenerDialog] as SD
  [main.cpp\ncomposition root] as MAIN
}
package "trading_services (Qt Network)" {
  [EtoroClient\nbroker REST] as EC
  [SimulationEngine] as SIM
  [MarketFeeds\nVIX · TradingView · news\nFear&Greed · Yahoo quote] as MF
  [EconomicCalendar] as CAL
  [AiAdvisor (Claude)] as AI
  [JsonHttp\nretry/backoff] as JH
  [Config\nconfig.json + apiKeyEtoro.json + env] as CFG
}
package "trading_domain (Qt Core only, pure)" {
  [Indicators] as IND
  [Forecasting\nOLS · kNN · Hurst · Monte-Carlo] as FC
  [SignalEnsemble] as ENS
  [DecisionEngine\ncomposite + crowd tilt] as DEC
  [TradePlan\ncosted proposal] as TP
  [PositionMath] as PM
  [EventInsight] as EI
  [Models (value types)] as MOD
}
MAIN --> MW
MAIN --> EC
MAIN --> MF
MAIN --> AI
MW --> EC
MW --> MF
MW --> CAL
MW --> AI
MW --> PC
MW --> SD
EC --> SIM
EC --> JH
EC --> CFG
MF --> JH
AI --> JH
CAL --> JH
EC ..> MOD
MW ..> IND
MW ..> FC
MW ..> ENS
MW ..> DEC
MW ..> TP
MW ..> PM
MW ..> EI
DEC ..> ENS
TP ..> FC
TP ..> ENS
TP ..> IND
@enduml

## Decision data flow (multi-source consolidation)

How the independent sources become the decision window's ranked list and the
costed trade plan (REQ-F-008 … REQ-F-011).

@startuml decision_flow
autonumber
participant "EtoroClient" as EC
participant "MarketFeeds" as MF
participant "MainWindow" as MW
participant "DecisionEngine\n(domain)" as DEC
participant "TradePlan\n(domain)" as TP
participant "AiAdvisor" as AI

EC -> MW : screenerRow(closes, maxLev) ×N
MF -> MW : instrumentRatingsUpdated (TradingView)
MF -> MW : instrumentNewsUpdated
MF -> MW : vixUpdated / fearGreedUpdated
MW -> DEC : computeDecisionRows(MarketSnapshot)
DEC --> MW : ranked DecisionRows (composite, confidence)
MW -> TP : buildTradePlan(closes, spread%, fees,\nvix, event risk, F&G, 3750€, horizon 24h)
TP --> MW : verdict · pWin/pLose · risk 1–5 ·\nleverage · SL/TP · cost bill · net edge
MW -> AI : buildDecisionEvidence(rows, snapshot)
AI --> MW : AiDecision (optional Claude verdict)
MW -> MW : render conclusion + plan + sources table
@enduml

## Order placement (money-moving path)

Advisory features never place orders; the double-press gate and the guards
sit between every proposal and the API (REQ-F-003, REQ-F-004, REQ-N-005).

@startuml order_flow
autonumber
actor Trader
participant "MainWindow" as MW
participant "EtoroClient" as EC
participant "eToro API" as API

Trader -> MW : BUY pressed (1st)
MW -> MW : arm (650 ms window), log € and $ size
Trader -> MW : BUY pressed (2nd, in time)
MW -> MW : guards: market open? EUR/USD known?\ncooldown? exposure cap (incl. resting orders)?
MW -> EC : openPosition(OrderRequest, triggerRate = 0)
EC -> API : POST orders (cfd, orderType mkt, SL/TP as rates)
API --> EC : orderId (submitted ≠ filled)
EC -> API : GET orders:lookup (poll until terminal)
API --> EC : Filled + positionExecutions
EC --> MW : orderResult(ok) → portfolio refresh
@enduml

A conditional entry takes the same path with a trigger rate, but eToro — not
this app — waits for the price (REQ-F-027). The order rests at the broker and
survives the app exiting; the app only tracks and cancels it.

@startuml limit_order_flow
autonumber
actor Trader
participant "MainWindow" as MW
participant "EtoroClient" as EC
participant "eToro API" as API

Trader -> MW : "Place limit BUY" pressed twice (own gate)
MW -> EC : openPosition(OrderRequest, triggerRate = X)
EC -> API : POST orders (orderType mit, triggerRate X,\nSL/TP as rates measured from X)
API --> EC : orderId → pendingOrdersUpdated (resting)
loop every ~10 poll ticks
  EC -> API : GET orders:lookup (order status)
  API --> EC : WaitingForMarket / PendingTriggeredRate
end
API --> EC : Filled (the broker's own feed touched X)
EC --> MW : orderResult(triggered) → portfolio + balance refresh
Trader -> MW : (alternatively) Cancel selected limit order
MW -> EC : cancelPendingOrder(orderId)
EC -> API : DELETE orders/{orderId}
@enduml

## Project layout, module by module (moved from the README)

The code is organised in three layers, each built as its own target so the
dependency direction (UI → services → domain) is enforced by the linker: the
domain cannot reach the network, and the services cannot reach the widgets.
All layers are plain cross-platform Qt/C++, so the same split holds on Linux,
Windows and Android.

## `src/domain/` — pure trading logic (`trading_domain`, Qt Core only)

Deterministic functions with no I/O and no UI, in `namespace trading` —
independently unit-testable and shared by every view that shows a signal.

| File | Responsibility |
|------|----------------|
| [`Models.h`](../src/domain/Models.h)                 | `Instrument`, `Candle`, `Position`, ... value types |
| [`Indicators.*`](../src/domain/Indicators.h)         | SMA, RSI, MACD, Bollinger, stochastic, volatility, ROC |
| [`Forecasting.*`](../src/domain/Forecasting.h)       | OLS regression, kNN analogs, Hurst, Monte-Carlo outlook |
| [`SignalEnsemble.*`](../src/domain/SignalEnsemble.h) | The BUY/SELL indicator vote + VIX confidence haircut |
| [`DecisionEngine.*`](../src/domain/DecisionEngine.h) | Weighted multi-source composite + AI evidence prompt |
| [`TradePlan.*`](../src/domain/TradePlan.h)           | Costed trade proposal: verdict, P(win), risk factor, leverage, SL/TP, cost bill |
| [`PositionMath.*`](../src/domain/PositionMath.h)     | SL/TP amount↔rate maths, value-per-point, price decimals |
| [`EventInsight.*`](../src/domain/EventInsight.h)     | Macro-event impact heuristics and descriptions |
| [`TradeScript.*`](../src/domain/TradeScript.h)       | Trade-script parsing + per-entry execution predicates |
| [`PaperTrader.*`](../src/domain/PaperTrader.h)       | Paper-trading bot: simulated account, real cost model, entry/exit rules |

## `src/services/` — integration (`trading_services`, adds Qt Network)

| File | Responsibility |
|------|----------------|
| [`Config.*`](../src/services/Config.h)                   | Load keys/settings from JSON + env; demo/live decision |
| [`EtoroClient.*`](../src/services/EtoroClient.h)         | The broker: eToro REST calls (rates, orders, portfolio, history) |
| [`SimulationEngine.*`](../src/services/SimulationEngine.h) | Synthetic feed + virtual account (no-credentials fallback) |
| [`MarketFeeds.*`](../src/services/MarketFeeds.h)         | Public web feeds: VIX, TradingView ratings, news |
| [`AiAdvisor.*`](../src/services/AiAdvisor.h)             | Claude (Anthropic API) decision synthesis |
| [`OllamaAdvisor.*`](../src/services/OllamaAdvisor.h)     | Local-LLM trading proposal (Ollama, optional, no key) |
| [`JsonHttp.*`](../src/services/JsonHttp.h)               | Shared reply/retry/JSON plumbing for all REST calls |
| [`EconomicCalendar.*`](../src/services/EconomicCalendar.h) | Macro-economic calendar feed |

## `src/ui/` + `src/main.cpp` — presentation (Qt Widgets/Charts)

| File | Responsibility |
|------|----------------|
| [`MainWindow.*`](../src/ui/MainWindow.h)         | Main window: trade panel, signals, positions, events |
| [`ScreenerDialog.*`](../src/ui/ScreenerDialog.h) | Leverage screener window |
| [`PriceChart.*`](../src/ui/PriceChart.h)         | Live time-vs-price Qt Charts widget |
| [`ChartView.*`](../src/ui/ChartView.h)           | Interactive pan/zoom chart view |
| [`PositionsModel.*`](../src/ui/PositionsModel.h) | Open-trades table model, in-place re-price |
| [`TradeGauge.*`](../src/ui/TradeGauge.h)         | Per-trade gauge window |
| [`TradeScriptPanel.*`](../src/ui/TradeScriptPanel.h) | Trade-script runner + window |
| [`BotSimPanel.*`](../src/ui/BotSimPanel.h)       | Paper-trading bot runner + window (simulated money, live prices) |
| [`Palette.h`](../src/ui/Palette.h)               | Shared UI colors |
| [`main.cpp`](../src/main.cpp)                    | Composition root: builds the services, injects them into the UI |

## The four licensed Qt tools

Axivion Suite, Squish, Squish Coco and Squish Test Center are commercial products
from The Qt Company and cannot be installed by `setup.sh`. **How to obtain, install
and configure each of them — including where the licence file goes and which
parameter points this project at the installation — is in
[docs/qt-tools.md](../docs/qt-tools.md).** `tools/check_prerequisites.sh` reports which
of them the current machine has.

Without them nothing breaks: their stages report *skipped*, the seven open-source
analyzers still gate at zero findings, gcov and clang MC/DC still measure coverage,
and the quality PDF lists the missing licences so a reader can tell "measured and
clean" from "not measured here".
