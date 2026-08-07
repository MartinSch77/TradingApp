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
| **Qt Network** | The eToro REST client (orders, portfolio, P/L, eligibility) and the public market feeds, both over one retrying JSON transport. | [`src/services/EtoroClient.cpp`](../src/services/EtoroClient.cpp), [`src/services/MarketFeeds.cpp`](../src/services/MarketFeeds.cpp), [`src/services/JsonHttp.cpp`](../src/services/JsonHttp.cpp) |
| **Qt Concurrent** | Monte-Carlo forecasting and plan building stay off the GUI thread; the bot's neural-net retrain runs on a `QFutureWatcher` so a scan never blocks the window. | [`src/ui/MainWindow.cpp`](../src/ui/MainWindow.cpp), [`src/ui/BotSimPanel.cpp`](../src/ui/BotSimPanel.cpp) |
| **Model/View** | The open-positions table is model/view and allocation-free per tick, with in-place SL/TP editing through a delegate. | [`src/ui/PositionsModel.cpp`](../src/ui/PositionsModel.cpp) |
| **Qt Test** — unit | Pure domain logic, one test function per behaviour, each carrying its requirement and design tags. | [`tests/tst_papertrader.cpp`](../tests/tst_papertrader.cpp), [`tests/tst_indexconfluence.cpp`](../tests/tst_indexconfluence.cpp) |
| **Qt Test** — integration | Services driven against a local `MockHttpServer`, so no test touches the real network. | [`tests/tst_etoroclientlive.cpp`](../tests/tst_etoroclientlive.cpp), [`tests/tst_marketfeeds.cpp`](../tests/tst_marketfeeds.cpp) |
| **Qt Test** — benchmarks | `QBENCHMARK` over the hot paths, so an optimisation can be shown rather than asserted. | [`tests/tst_benchmarks.cpp`](../tests/tst_benchmarks.cpp) |
| **Qt deployment** | AppImage via linuxdeploy (x86-64 and ARM64), Windows portable ZIP via windeployqt, Android APK, all from CMake install rules. | [`tools/package_appimage.sh`](../tools/package_appimage.sh), [`tools/package_portable.ps1`](../tools/package_portable.ps1), [`tools/build_android.sh`](../tools/build_android.sh) |
| **CMake / `qt_add_library`** | Layering enforced by the linker: `domain ← services ← ui`, each a separate target. | [`CMakeLists.txt`](../CMakeLists.txt) |

## Two things this map is honest about

**Qt Charts is deprecated since Qt 6.10.** It is what this application uses today, and
it is also the reason the project is GPL-3.0-or-later: Qt Charts is offered under a
commercial licence or GPLv3 with **no LGPL option**, so a distributed binary linking it
must be GPLv3-compatible. Qt Graphs is the module Qt recommends for new work, and it is
now installed here (`qtgraphs`, added to `setup.sh`/`setup.ps1` on 2026-08-07) so the
newer `QCustomSeries` backend can be built beside the shipping Qt Charts one rather than
replacing it. Qt Graphs is likewise GPLv3-or-commercial.

**Qt Quick is not used yet.** The interface is Qt Widgets throughout. A QML dashboard
over the same `trading_domain` / `trading_services` libraries — which is where the
layered architecture would pay off visibly, since both frontends would reuse the same
tested domain — is planned rather than present, and this map will say so until it is.

## Where the Qt *tools* are covered

The four licensed Qt quality tools have their own case studies, each written as
problem → configuration → finding → correction → measurable result:

- [Squish for Qt](case-studies/squish.md) — GUI tests that cannot reach real money
- [Squish Coco](case-studies/coco.md) — MC/DC from 77% to ~88%
- [Qt Test Center](case-studies/test-center.md) — results that outlive the working tree
- [Axivion Suite](case-studies/axivion.md) — architecture as code, checked every run
