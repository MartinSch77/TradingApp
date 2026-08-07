# Qt framework feature map

@page qtshowcase Qt framework feature map

What this project uses each part of Qt 6 for, with one representative file per row so
the repository can be read as a worked example rather than a feature list.

Every row links to code that is compiled, tested and shipped — nothing here was added
to lengthen the table.

| Qt technology | Use in TradingApp | Representative source |
|---|---|---|
| **Qt Core** — value types, dates, JSON, signals/slots | The whole domain layer, which links **Qt::Core only** so it cannot reach the network or a widget. Money arithmetic, instrument models, exchange-clock reasoning. | [`src/domain/Money.cpp`](../src/domain/Money.cpp), [`src/domain/Models.h`](../src/domain/Models.h) |
| **Qt Core** — timezones and session logic | `sessionPhaseFor` reads the instrument's OWN exchange clock and the New York clock for US releases, never a fixed offset — Europe and the US shift clocks on different days. | [`src/domain/PaperTrader.cpp`](../src/domain/PaperTrader.cpp) |
| **Qt Widgets** | The desktop interface: trade panel, positions table, limit orders, event timeline, and the separate decision / heavyweights / bot windows. | [`src/ui/MainWindow.cpp`](../src/ui/MainWindow.cpp) |
| **Qt Charts** | Live price visualisation, and the constituent field normalised to each name's own session open. | [`src/ui/PriceChart.cpp`](../src/ui/PriceChart.cpp), [`src/ui/HeavyweightsPanel.cpp`](../src/ui/HeavyweightsPanel.cpp) |
| **Qt Quick / Qt QML** | The second front end: a declarative cockpit over the same `CockpitModel`, hosted both as a `QQuickWidget` page inside the Widgets window and as the standalone `TradingCockpit` binary. `qt_add_qml_module` with a `pragma Singleton` theme; every number is shaped in C++ so the view is unit-testable without rendering. | [`src/quick/qml/Main.qml`](../src/quick/qml/Main.qml), [`src/ui/CockpitModel.cpp`](../src/ui/CockpitModel.cpp), [`src/quick/cockpit_main.cpp`](../src/quick/cockpit_main.cpp) |
| **Qt Graphs** — `CustomSeries` (6.11) | The intraday candlestick chart. Qt Graphs 2D has no candlestick type, so each bar is a `CustomSeries` data item with its own delegate; direction is drawn hollow-up / solid-down so the fill, not the colour, carries it. | [`src/quick/qml/PriceChart.qml`](../src/quick/qml/PriceChart.qml), [`src/domain/Candles.cpp`](../src/domain/Candles.cpp) |
| **Qt Network** | The eToro REST client (orders, portfolio, P/L, eligibility) and the public market feeds, both over one retrying JSON transport. | [`src/services/EtoroClient.cpp`](../src/services/EtoroClient.cpp), [`src/services/MarketFeeds.cpp`](../src/services/MarketFeeds.cpp), [`src/services/JsonHttp.cpp`](../src/services/JsonHttp.cpp) |
| **Qt Concurrent** | Monte-Carlo forecasting and plan building stay off the GUI thread; the bot's neural-net retrain runs on a `QFutureWatcher` so a scan never blocks the window. | [`src/ui/MainWindow.cpp`](../src/ui/MainWindow.cpp), [`src/ui/BotSimPanel.cpp`](../src/ui/BotSimPanel.cpp) |
| **Model/View** | The open-positions table is model/view and allocation-free per tick, with in-place SL/TP editing through a delegate. | [`src/ui/PositionsModel.cpp`](../src/ui/PositionsModel.cpp) |
| **Qt Test** — unit | Pure domain logic, one test function per behaviour, each carrying its requirement and design tags. | [`tests/tst_papertrader.cpp`](../tests/tst_papertrader.cpp), [`tests/tst_indexconfluence.cpp`](../tests/tst_indexconfluence.cpp) |
| **Qt Test** — integration | Services driven against a local `MockHttpServer`, so no test touches the real network. | [`tests/tst_etoroclientlive.cpp`](../tests/tst_etoroclientlive.cpp), [`tests/tst_marketfeeds.cpp`](../tests/tst_marketfeeds.cpp) |
| **Qt Test** — benchmarks | `QBENCHMARK` over the hot paths, so an optimisation can be shown rather than asserted. | [`tests/tst_benchmarks.cpp`](../tests/tst_benchmarks.cpp) |
| **Qt deployment** | AppImage via linuxdeploy (x86-64 and ARM64), Windows portable ZIP via windeployqt, Android APK, all from CMake install rules. | [`tools/package_appimage.sh`](../tools/package_appimage.sh), [`tools/package_portable.ps1`](../tools/package_portable.ps1), [`tools/build_android.sh`](../tools/build_android.sh) |
| **CMake / `qt_add_library`** | Layering enforced by the linker: `domain ← services ← ui`, each a separate target. | [`CMakeLists.txt`](../CMakeLists.txt) |

## Two things this map is honest about

**Qt Charts is deprecated since Qt 6.10, and it cannot share a process with Qt Graphs.**
Qt Charts is what the Widgets interface uses today, and it is also why the project is
GPL-3.0-or-later: it is offered under a commercial licence or GPLv3 with **no LGPL
option**, so a distributed binary linking it must be GPLv3-compatible. Qt Graphs — Qt's
recommended module for new work, and likewise GPLv3-or-commercial — supplies the
`CustomSeries` candlestick chart.

An earlier version of this page said the Qt Graphs backend could "be built beside the
shipping Qt Charts one rather than replacing it". **That was wrong**, and the correction is
worth recording because the failure mode is invisible. The two modules declare seventeen
classes with identical names in the same namespace — `QValueAxis`, `QAbstractAxis`,
`QLineSeries`, `QBarSeries`, `QPieSeries`, `QXYSeries` and eleven more — so
`qmltyperegistrar` sees two `::QValueAxis` and registers neither. At run time every Qt
Graphs QML type then fails to resolve (`ValueAxis is not a type`), the chart component is
unavailable, and the whole cockpit renders as a blank white rectangle with no error
anywhere. Measured 2026-08-07 with a two-target probe: identical source, the only
difference being whether `Qt6::Charts` was on the link line; instantiation order changes
nothing, the link alone decides it. Hence two executables over one shared QML module —
`TradingApp` (Charts) and `TradingCockpit` (Graphs) — and a `Loader` that degrades the
chart to a stated reason in the process that cannot draw it.

**Qt Quick is used, but not for anything that moves money.** The declarative cockpit
presents evidence — market cards, the nine-read confluence meter, the candlestick chart —
over the same `trading_domain` / `trading_services` libraries as the Widgets interface,
which is where the layering claim stops being an assertion: the second front end was built
without touching the domain. Amount, leverage, SL/TP entry and the REQ-N-005 double-press
stay in Qt Widgets, the surface that has been GUI-tested, object-name-gated and audited. A
second place where money can move would double the surface needing that scrutiny while
halving the attention paid to each.

## Where the Qt *tools* are covered

The four licensed Qt quality tools have their own case studies, each written as
problem → configuration → finding → correction → measurable result:

- [Squish for Qt](case-studies/squish.md) — GUI tests that cannot reach real money
- [Squish Coco](case-studies/coco.md) — MC/DC from 77% to ~88%
- [Qt Test Center](case-studies/test-center.md) — results that outlive the working tree
- [Axivion Suite](case-studies/axivion.md) — architecture as code, checked every run
