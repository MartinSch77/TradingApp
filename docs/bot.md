# The trading bot

@page bot Every switch on the simulated trading bot, and the QA helpers
@tableofcontents

The bot trades SIMULATED money on LIVE prices and has no route to an order endpoint
(REQ-F-029). What follows is every environment switch, plus the QA aids used to
capture it. The risk model, the exit rules and the decision log are specified in
[the requirements](requirements.md) under REQ-F-029 through REQ-F-037.

| Variable / setting | Effect |
|---|---|
| `TRADINGAPP_BOT_ARM=1` | arms the paper bot at start-up, for unattended runs (the armed flag is persisted anyway) |
| `TRADINGAPP_BOT_AI=off\|confirm\|lead` | how much say the local model gets over ENTRIES (REQ-F-030); it may also close positions it no longer believes in (REQ-F-032) |
| `TRADINGAPP_BOT_NET=off\|advise\|gate` | how much say the LEARNED model gets (REQ-F-033): score only, or refuse below the floor once it is trusted |
| `TRADINGAPP_BOT_TARGET` / `TRADINGAPP_BOT_LOSS_LIMIT` | the daily stopping rules in EUR of booked net; `0` switches one off (REQ-F-031) |
| `TRADINGAPP_FORCE_SIMULATION=1` | the app runs in SIMULATION whatever the keys say — no live mode, no broker network, no order path. What makes the Squish GUI suite safe on a machine that has real credentials (REQ-N-007) |
| `TRADINGAPP_BOT_TRAIN=1` | refit the learned model once at start-up (for headless machines with nobody to press the button) |
| `config.json`: `botDailyTarget`, `botDailyLossLimit`, `ollamaHost`, `ollamaModel` | the same settings, without the environment |

The bot writes three files next to its config (`~/.config/TradingApp/eToro Trader/`
on Linux): `botsim.json` (the book — open trades, closed trades, the day ledger),
`botsim-experience.jsonl` (one training example per closed trade) and `botnet.json`
(the model it fitted to them).

`TRADINGAPP_SHOT=/path/out.png ./build/TradingApp` grabs every visible window
to one PNG each (further windows get a `-1`, `-2`, … suffix) after 3000 ms and
exits — handy for headless screenshots (`QT_QPA_PLATFORM=offscreen`).
`TRADINGAPP_SHOT_OPEN=1` opens the decision and closed-trades windows first;
`TRADINGAPP_SHOT_DELAY_MS` overrides the capture delay.
