# Roadmap — the modern-Qt showcase, ordered by what it costs

@page roadmap Roadmap: Qt 6.11 features, measured availability, and what to build next
@tableofcontents

This page exists because a feature list ranked by *wow effect* is not a plan. Every entry
below was checked against the Qt kit this project actually builds with (**Qt 6.11.1**,
`~/Qt/6.11.1/gcc_64`) on 2026-08-07, and the ordering follows **cost**, which turns out to
disagree with the ordering by impressiveness in three places.

## What this Qt install can build TODAY

Measured, not assumed — `ls lib/cmake/Qt6*`, header presence, and the QML type files.

| Feature | Module | Available here? |
|---|---|---|
| **QRangeModel / QRangeModelAdapter** | Qt Core | ✅ `QtCore/qrangemodel.h`, `QRangeModel`, `QRangeModelAdapter` |
| **Qt Labs StyleKit** | `Qt6LabsStyleKit` | ✅ present (+ `Impl`) |
| **VectorImage / Lottie→QML** | `Qt6QuickVectorImage` | ✅ present (+ generator, helpers) |
| **DoubleSpinBox** | Qt Quick Controls | ✅ `Controls/{Material,Fusion,Universal}/DoubleSpinBox.qml` |
| Qt Quick + Controls 2 + QuickWidgets | `Qt6Quick*` | ✅ all present |
| **Qt SQL + SQLite driver** | `Qt6Sql` | ✅ `plugins/sqldrivers/libqsqlite.so` |
| Qt Quick 3D | `Qt6Quick3D` | ✅ present — but see the GPU note |
| Qt Charts | `Qt6Charts` | ✅ present (deprecated since 6.10) |
| **QCustomSeries** | `Qt6Graphs` | ✅ **installed 2026-08-07** — `qcustomseries.h`, QML type `QtGraphs/CustomSeries 6.11`, compile-checked; `setup.sh`/`setup.ps1` now install it |
| Qt WebSockets | `Qt6WebSockets` | ❌ not installed |
| Qt State Machine | `Qt6StateMachine` | ❌ not installed (ships via qtscxml) |
| Qt Remote Objects | `Qt6RemoteObjects` | ❌ not installed |
| **Qt TaskTree** | — | ❌ **not in this Qt build at all** |
| **Qt OpenAPI** | — | ❌ **not in this Qt build at all** |
| **Qt Canvas Painter** | — | ❌ **not in this Qt build at all** (no CMake package, no headers) |

`QCustomSeries` itself is confirmed real: Qt Graphs, **since Qt 6.11**, with a
`delegate : Component` property so each data point is rendered by a QML delegate — which is
what makes candlesticks and box plots possible from one series type. Its documentation
header also reads "Qt Graphs | Commercial or GPLv3", which is the second module after Qt
Charts obliging this project's GPL-3.0-or-later licence.

## Why the ordering is not the ranking

Three corrections fall out of the table:

1. **QRangeModel is free today.** It was ranked fourth; it needs no install at all. For a
   C++23 project it is also the cheapest way to show modern C++ reaching the UI.
2. **TaskTree is the most expensive item, not the second.** It is absent from this Qt
   package entirely, so it needs a different Qt build or a source build *before the first
   line of code*. Same for OpenAPI and Canvas Painter.
3. **Canvas Painter cannot be demonstrated on this machine even once installed.** It is
   built on QRhi and this host has no GPU (WSL2; the project's own notes record Quick3D
   crashing under WSLg). It would be a CI-and-real-hardware-only feature, which is a poor
   fit for something whose entire selling point is what it looks like.

## The plan

### Stage 1 — everything that needs no install

- **QRangeModel watchlist.** `std::vector<Position>` / `<Trade>` / `<MarketEvent>` exposed
  through `QRangeModelAdapter`, consumed by both Qt Widgets and QML. Note the existing
  `PositionsModel` is already a hand-written `QAbstractTableModel` and already works — so
  this is a *comparison*, and the honest framing is "here is the same data both ways",
  not "we replaced 200 lines".
- **StyleKit themes** — Trading Dark (default), Light, High Contrast. The colour system is
  already specified and validated in [the dashboard design](quick-dashboard-design.md);
  StyleKit is where those tokens live instead of being scattered as literals.
- **VectorImage/Lottie state indicators** — connection, simulation, volatility spike. Out
  of Technology Preview in 6.11, so this is the one animated feature that is production-safe.
- **DoubleSpinBox** for amount / leverage / SL / TP, replacing hand-rolled decimal handling.
- **Market Replay over SQLite** — see below. This is the deep one.

### Stage 2 — the biggest visual return (the install is DONE)

`qtgraphs` is installed and both setup scripts now request it (`-m qtcharts qtgraphs`),
alongside qtcharts rather than instead of it. Verified: `Qt6Graphs` and `Qt6GraphsWidgets`
CMake packages, `include/QtGraphs/qcustomseries.h`, the QML type exported as
`QtGraphs/CustomSeries 6.11`, and a compile check against `QCustomSeries` that passes.

One thing worth recording because the obvious command is wrong: aqt has no "add module"
subcommand, so adding qtgraphs to an EXISTING kit is `install-qt … -m qtgraphs
--noarchives`. Without `--noarchives` it re-downloads all of qtbase.

- What remains is the code: a **QCustomSeries candlestick chart** as a separate
  "Qt 6.11 Showcase" view. Keeping the Qt Charts implementation alongside it is right: it turns a migration
  into a side-by-side demonstration and cannot destabilise the working chart. The seam is
  `IPriceChartBackend` — legacy Charts backend, modern Graphs backend, one interface.

### Stage 3 — needs a different Qt package

- **TaskTree** for the market-analysis pipeline, **OpenAPI** for a generated eToro client
  (keeping the hand-written `EtoroClient` as the production path), **Canvas Painter** for
  order-flow and depth rendering — CI-only, given the GPU.

Everything in stage 3 is a Technology Preview outside Qt's compatibility promises, so all
of it stays behind an interface and **away from the money-moving path**, exactly as the
existing layering already enforces for QML.

## Market Replay — the feature with the most groundwork already done

This is ranked highest among the non-Qt-specific ideas because most of it exists.
`prediction-ledger.jsonl` already records **every evaluation** with its strength, how much
was measurable, the regime, the mark and — for the ones that stayed out — the refusal code.
`botsim-decisions.log` carries the human-readable reasoning and `botsim-experience.jsonl`
the labelled outcomes.

So replay is largely a *view* over data the application already produces, plus moving the
store to SQLite:

- `Qt6Sql` + SQLite in `trading_services`, one row per evaluation, indexed by time and
  instrument.
- A QML time slider bound to session time, with 1× / 10× / 100× playback.
- At any point in the session, show what the app *then* knew: chart state, the nine reads
  and which were unmeasurable, the model's rationale, and the refusal code if it stayed out.
- "Would this trade have won?" is already computable — that is exactly what
  `PredictionLedger`'s outcome pairing does.

This also subsumes the reproducible **showcase mode** (`--showcase --scenario …`): a
scenario is simply a recorded session replayed with a fixed seed, which is a far better
foundation than a synthetic generator because the data is real.

## Continuous testing against the next Qt

Worth doing and cheap: a GitHub Actions job against **Qt 6.12 Beta 2** with
`continue-on-error: true`, while the production build stays on 6.11.1. Qt 6.12.0 is
scheduled for 2026-09-22. That signals the project tracks the coming Qt generation without
making a release depend on a pre-release — the same reasoning that keeps SonarCloud and
Coverity informational rather than gating.

## What is deliberately NOT here

- **Qt Bridges for Rust** has its own entry in the project instructions, because it comes
  with a verification obligation rather than just a build one: any Rust component is checked
  by **both** Axivion (`AxivionRustFrontend`, and `Rust-CheckExternSignatures` for FFI
  mismatches across the merged C++/Rust RFG) **and** Clippy via `RustClippyIntegration`.
- **Qt Remote Objects** splitting the engine from the UI. Architecturally interesting, but
  it multiplies the deployment and test surface for a single-user desktop application, and
  it should come after Replay and the state machine rather than before them.
