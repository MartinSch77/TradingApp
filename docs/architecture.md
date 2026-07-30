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
