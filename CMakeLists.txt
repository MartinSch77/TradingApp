# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.2)

# cmake_minimum_required(4.2) turns every policy up to 4.2 NEW, including
# CMP0156 (link-line library de-duplication, new in CMake 3.30). Qt forces
# CMP0156 back to OLD for non-Apple platforms regardless, and warns once per
# qt_add_library/qt_add_executable call while doing so — around 20 screens of
# warnings for this project. Setting it to OLD here matches the behaviour Qt
# imposes anyway, so the build is unchanged and the warning has nothing to
# report. (Passing -DQT_FORCE_CMP0156_TO_VALUE=OLD does NOT help: only the
# value NEW silences that check, and NEW would change how Qt links.)
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 OLD)
endif()

project(TradingApp
    VERSION 1.0.4
    DESCRIPTION "Qt desktop app to trade instruments via the official eToro API"
    LANGUAGES CXX)

# C++23 everywhere by default. Overridable from the command line for the one
# consumer that cannot parse it: Squish Coco's instrumenting front end supports
# up to C++20, so tools/coverage.ps1 configures its build tree with
# -DCMAKE_CXX_STANDARD=20 (see docs/windows.md). Everything else — including
# every build whose result ships or is analyzed — stays on 23.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt 6.11.1 lives under ~/Qt on this machine; allow overriding via -DCMAKE_PREFIX_PATH.
# Quick/Qml/QuickWidgets carry the declarative cockpit (REQ-F-038). QuickWidgets is what
# lets it be ONE PAGE of this Widgets application rather than a second executable — the same
# domain and service libraries behind two presentations, which is the point of the exercise.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Charts Concurrent
                                    Qml Quick QuickWidgets QuickControls2)
# Qt Graphs supplies CustomSeries (6.11), which draws the cockpit's candlesticks — but it is
# found, NOT linked, and that is load-bearing.
#
# Qt Charts and Qt Graphs declare SEVENTEEN classes with the same names in the same namespace
# (QValueAxis, QAbstractAxis, QLineSeries, QBarSeries, QPieSeries, QXYSeries and eleven more).
# Linking both into one target makes qmltyperegistrar see two ::QValueAxis and fail to
# register either — it warns "Multiple C++ types called QAbstractAxis found!" at BUILD time
# and then, at RUN time, the QtGraphs QML types do not resolve: "ValueAxis is not a type",
# PriceChart is unavailable, and the whole cockpit renders as a blank white rectangle with no
# other symptom. Measured 2026-08-07; the blank window is what made it visible.
#
# Nothing here uses the Qt Graphs C++ API — only its QML types — so the module is loaded as a
# QML plugin at run time from the Qt installation's own qml directory, and the deploy tools
# pick it up from the `import QtGraphs` in PriceChart.qml. Do not "tidy" Qt6::Graphs onto the
# link line: it builds, and it silently empties the cockpit.
find_package(Qt6 QUIET COMPONENTS Graphs)
if(NOT Qt6Graphs_FOUND)
    message(WARNING "Qt Graphs not found — the cockpit's candlestick chart will not render. "
                    "Install it with: aqt/maintenancetool -m qtgraphs")
endif()

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# Compiler diagnostics: the compiler is the first static analyzer, so it runs
# with a full warning set instead of the default one. The list is what this
# codebase is clean against (measured 2026-07-29: 20 -Wreorder and 4 -Wshadow
# hits, all fixed), and it deliberately includes the warnings that catch the
# classic Qt mistakes — -Woverloaded-virtual for a mistyped override,
# -Wsuggest-override, -Wnon-virtual-dtor.
#
# Deliberately NOT enabled:
#   -Wpedantic / -Wextra-semi   report the `Q_OBJECT;` semicolon as an extra
#                               ';' (37 hits). That semicolon is required here:
#                               it is the tree-sitter parse anchor the
#                               traceability tooling keys on (see CLAUDE.md).
#   -Wold-style-cast, -Wuseless-cast   Qt's own macros and QVariant round-trips
#                               generate both by construction.
#   -Wconversion                the int/qsizetype mixing that clang-tidy's
#                               *-narrowing-conversions already governs.
#
# TRADINGAPP_WARNINGS_AS_ERRORS turns the set fatal. Off by default so a newer
# compiler's brand-new warning cannot break someone's local build; the quality
# pipeline (build_all.sh / build_all.ps1) turns it on.
option(TRADINGAPP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(MSVC)
    # /utf-8 is NOT optional here: these sources are UTF-8 without a BOM (.gitattributes
    # keeps the BOM for .ps1 only) and carry ~250 non-ASCII string literals — the em
    # dashes and "—"/"⇅"/"×" the UI shows. Without it MSVC decodes the source with the
    # machine's ANSI code page, so every one of those literals reaches QStringLiteral
    # as mojibake ("—" → "â€""). It compiles and links silently: on a Western code page
    # each UTF-8 byte maps to SOME CP1252 character, so not even C4819 fires — the only
    # symptom is the running Windows UI showing the wrong glyphs.
    add_compile_options(/utf-8 /W4 /permissive-)
    if(TRADINGAPP_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(
        -Wall -Wextra
        -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wsuggest-override
        -Wimplicit-fallthrough -Wmisleading-indentation
        -Wformat=2 -Wdouble-promotion -Wcast-qual)
    # GCC-only spellings. clang rejects an unknown -W… outright, and the flags
    # reach it through the compile database (clang-tidy, the Clang Static
    # Analyzer, the clang sanitizer builds) — so they must be GCC-gated, not
    # merely "supported by the compiler that builds".
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wduplicated-cond -Wduplicated-branches -Wlogical-op)
    endif()
    if(TRADINGAPP_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Layered architecture, enforced by the linker:
#   domain   -> pure trading logic (Qt Core only; no I/O, no UI)
#   services -> integration: eToro REST, web feeds, calendar, config (adds Network)
#   app/ui   -> Qt Widgets/Charts presentation + the composition root (main.cpp)
# Each lower layer cannot see the ones above it, so the dependency direction
# stays one-way even as the code grows.
# ---------------------------------------------------------------------------
qt_add_library(trading_domain STATIC
    src/domain/Models.cpp
    src/domain/Models.h
    src/domain/Money.cpp
    src/domain/Money.h
    src/domain/OrderRequestValidator.cpp
    src/domain/OrderRequestValidator.h
    src/domain/InstrumentCatalog.cpp
    src/domain/InstrumentCatalog.h
    src/domain/Indicators.cpp
    src/domain/Indicators.h
    src/domain/Forecasting.cpp
    src/domain/Forecasting.h
    src/domain/SignalEnsemble.cpp
    src/domain/SignalEnsemble.h
    src/domain/PositionMath.cpp
    src/domain/PositionMath.h
    src/domain/EventInsight.cpp
    src/domain/EventInsight.h
    src/domain/DecisionEngine.cpp
    src/domain/DecisionEngine.h
    src/domain/TradePlan.cpp
    src/domain/TradePlan.h
    src/domain/TradeScript.cpp
    src/domain/TradeScript.h
    src/domain/BotNet.cpp
    src/domain/BotNet.h
    src/domain/LeadSignal.cpp
    src/domain/LeadSignal.h
    src/domain/Candles.cpp
    src/domain/Candles.h
    src/domain/ConfirmGate.cpp
    src/domain/ConfirmGate.h
    src/domain/IndexConfluence.cpp
    src/domain/IndexConfluence.h
    src/domain/PredictionLedger.cpp
    src/domain/PredictionLedger.h
    src/domain/DecisionLog.cpp
    src/domain/DecisionLog.h
    src/domain/PaperTrader.cpp
    src/domain/PaperTrader.h
)
target_include_directories(trading_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(trading_domain PUBLIC Qt6::Core)

qt_add_library(trading_services STATIC
    src/services/AiAdvisor.cpp
    src/services/AiAdvisor.h
    src/services/OrderGateway.cpp
    src/services/OrderGateway.h
    src/services/Config.cpp
    src/services/Config.h
    src/services/EconomicCalendar.cpp
    src/services/EconomicCalendar.h
    src/services/EtoroClient.cpp
    src/services/EtoroClient.h
    src/services/JsonHttp.cpp
    src/services/JsonHttp.h
    src/services/MarketFeeds.cpp
    src/services/MarketFeeds.h
    src/services/OllamaAdvisor.cpp
    src/services/OllamaAdvisor.h
    src/services/SimulationEngine.cpp
    src/services/SimulationEngine.h
)
target_link_libraries(trading_services PUBLIC
    trading_domain
    Qt6::Network
)

qt_add_executable(TradingApp
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/Palette.h
    src/ui/PositionsModel.cpp
    src/ui/PositionsModel.h
    src/ui/TradeGauge.cpp
    src/ui/TradeGauge.h
    src/ui/PriceChart.cpp
    src/ui/PriceChart.h
    src/ui/ScreenerDialog.cpp
    src/ui/TradeScriptPanel.cpp
    src/ui/TradeScriptPanel.h
    src/ui/HeavyweightsPanel.cpp
    src/ui/HeavyweightsPanel.h
    src/ui/LeadGauge.cpp
    src/ui/LeadGauge.h
    src/ui/BotSimRunner.cpp
    src/ui/BotSimRunner.h
    src/ui/BotSimPanel.cpp
    src/ui/BotSimPanel.h
    src/ui/ScreenerDialog.h
    src/ui/ChartView.cpp
    src/ui/ChartView.h
    src/ui/CockpitPanel.cpp
    src/ui/CockpitPanel.h
)

# The cockpit's QML as a proper module rather than loose files, so the types resolve by URI
# (TradingApp.Cockpit) and qmllint can check them against that module. Theme.qml is a
# `pragma Singleton`, which qt_add_qml_module registers for us.
# QTP0001 NEW puts the module under the standard ':/qt/qml/' resource prefix, which is the
# URL CockpitPanel loads. Left unset, Qt warns and the prefix is the legacy one.
qt_policy(SET QTP0001 NEW)

# OUTPUT_DIRECTORY is NOT optional here, and the reason is a genuine collision: this target
# writes its executable to the build ROOT (RUNTIME_OUTPUT_DIRECTORY below), so the file
# `build/TradingApp` already exists — and the module's default output would be the DIRECTORY
# `build/TradingApp/Cockpit/`. CMake fails with "Not a directory" while configuring. Putting
# the module under build/qml/ sidesteps the clash between an executable and a module sharing
# one name.
# QT_RESOURCE_ALIAS per file, because qt_add_qml_module keeps the SOURCE subdirectory in the
# resource alias: without these the files land at
# qrc:/qt/qml/TradingApp/Cockpit/src/quick/qml/Main.qml, and loading the documented
# .../Cockpit/Main.qml fails with "No such file or directory" — measured, and caught by
# CockpitPanel::ready() rather than by an empty window.
set_source_files_properties(src/quick/qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(src/quick/qml/MarketCard.qml
    PROPERTIES QT_RESOURCE_ALIAS MarketCard.qml)
set_source_files_properties(src/quick/qml/ConfluenceMeter.qml
    PROPERTIES QT_RESOURCE_ALIAS ConfluenceMeter.qml)
set_source_files_properties(src/quick/qml/PriceChart.qml
    PROPERTIES QT_RESOURCE_ALIAS PriceChart.qml)
set_source_files_properties(src/quick/qml/CockpitWindow.qml
    PROPERTIES QT_RESOURCE_ALIAS CockpitWindow.qml)
set_source_files_properties(src/quick/qml/TradeTicket.qml
    PROPERTIES QT_RESOURCE_ALIAS TradeTicket.qml)
set_source_files_properties(src/quick/qml/OpenPositions.qml
    PROPERTIES QT_RESOURCE_ALIAS OpenPositions.qml)
set_source_files_properties(src/quick/qml/HistoryCalendar.qml
    PROPERTIES QT_RESOURCE_ALIAS HistoryCalendar.qml)
# QT_QML_SINGLETON_TYPE is REQUIRED as well as the file's own `pragma Singleton`: CMake does
# not parse the QML, so without this the generated qmldir says "Theme 1.0 Theme.qml" instead
# of "singleton Theme 1.0 Theme.qml". The failure is quiet and misleading — every Theme.*
# colour resolves to an invalid value, so the cockpit renders black-on-white with no error
# at all, and a function on the singleton raises "is not a function". Measured.
set_source_files_properties(src/quick/qml/Theme.qml PROPERTIES
    QT_RESOURCE_ALIAS Theme.qml
    QT_QML_SINGLETON_TYPE TRUE)

# The module is its OWN library rather than part of the executable, so BOTH front ends can
# use it: the QQuickWidget panel inside TradingApp, and the standalone TradingCockpit built
# below. It deliberately links Quick and NOT Qt Graphs — see the find_package comment at the
# top; a Graphs link here would propagate into TradingApp, which links Qt Charts, and empty
# the cockpit.
qt_add_library(trading_cockpit STATIC
    src/ui/CockpitModel.cpp
    src/ui/CockpitModel.h
)
target_include_directories(trading_cockpit PUBLIC src)
target_link_libraries(trading_cockpit PUBLIC trading_domain Qt6::Quick
    # Controls supplies the ticket's SpinBox and Button. Widgets-free on purpose:
    # this library is linked by the Qt Quick binary too.
    Qt6::QuickControls2)

qt_add_qml_module(trading_cockpit
    URI TradingApp.Cockpit
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml/TradingApp/Cockpit
    QML_FILES
        src/quick/qml/Main.qml
        src/quick/qml/MarketCard.qml
        src/quick/qml/ConfluenceMeter.qml
        src/quick/qml/PriceChart.qml
        src/quick/qml/CockpitWindow.qml
        src/quick/qml/TradeTicket.qml
        src/quick/qml/OpenPositions.qml
        src/quick/qml/HistoryCalendar.qml
        src/quick/qml/Theme.qml
)

target_link_libraries(TradingApp PRIVATE
    trading_services
    Qt6::Gui
    Qt6::Widgets
    Qt6::Charts
    Qt6::Concurrent
    # The declarative cockpit (REQ-F-038). QuickWidgets hosts it inside the existing
    # QMainWindow, so this stays one executable over one set of tested libraries.
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickWidgets
    trading_cockpit
)

# Put the binary directly in the build dir root for convenience.
set_target_properties(TradingApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
# The cockpit's QML module is a static library now, shared by both front ends, and a STATIC
# qt_add_qml_module puts its type registration in a SEPARATE plugin target. Linking only the
# library leaves the plugin's initialiser as an unreferenced symbol in an archive, the linker
# drops it, and the module is "not found" at run time — with the qmldir sitting right there in
# the resources, which makes the error read like a path problem when it is a linking one.
# qt_import_qml_plugins() does not help: it scans the TARGET's own QML files, and both
# executables have none. The plugin is therefore named directly.
target_link_libraries(TradingApp PRIVATE trading_cockpitplugin)

# ---------------------------------------------------------------------------
# TradingCockpit — the SECOND front end (REQ-F-038): Qt Quick end to end, over the same
# domain and service libraries and the same CockpitModel and QML as the panel embedded in
# TradingApp. It exists as its own process for a measured reason, not for tidiness: Qt Charts
# and Qt Graphs declare seventeen identically-named classes, so a binary that links both
# cannot resolve a single Qt Graphs QML type, and the cockpit's candlestick chart needs
# Qt Graphs while the Widgets UI is built on Qt Charts. See the find_package comment above.
#
# It links trading_services for the read-only feeds and never constructs an EtoroClient, so
# there is structurally no route to an order endpoint from this binary.
#
# Built only when Qt Graphs is present; the rest of the project does not depend on it.
# ---------------------------------------------------------------------------
if(Qt6Graphs_FOUND)
    qt_add_executable(TradingCockpit src/quick/cockpit_main.cpp)
    target_compile_definitions(TradingCockpit PRIVATE
        TRADINGAPP_VERSION="${PROJECT_VERSION}")
    target_link_libraries(TradingCockpit PRIVATE
        trading_services
        trading_cockpit
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
    )
    set_target_properties(TradingCockpit PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    # See the note on TradingApp above: the static QML module's plugin must be named.
    target_link_libraries(TradingCockpit PRIVATE trading_cockpitplugin)
endif()

# ---------------------------------------------------------------------------
# TradingBot — the CONSOLE front end for examining the paper-trading bot (REQ-F-029).
# Qt Core + services only, no widgets and no QML: it links the SAME BotSimRunner the GUI
# apps drive (the reason that class was split out of BotSimPanel), so it measures the same
# bot rather than a copy. A static ANSI TUI; no ncurses, so setup.sh gains no dependency.
# ---------------------------------------------------------------------------
qt_add_executable(TradingBot
    src/console/bot_console_main.cpp
    src/console/BotConsoleView.cpp
    src/console/BotConsoleView.h
    src/ui/BotSimRunner.cpp
    src/ui/BotSimRunner.h
)
target_include_directories(TradingBot PRIVATE src)
target_link_libraries(TradingBot PRIVATE trading_services Qt6::Core Qt6::Concurrent)
set_target_properties(TradingBot PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Visual Studio solution ergonomics
# ---------------------------------------------------------------------------
# The .sln is generated (tools/make_vs_solution.ps1 or the "visual-studio"
# CMake preset), never committed — CMakeLists.txt stays the single description
# of the build. These two properties only affect the multi-config IDE
# generators and are ignored by Ninja/Makefiles.
if(CMAKE_CONFIGURATION_TYPES)
    # F5 should launch the app, not ALL_BUILD.
    if(NOT TRADINGAPP_VS_STARTUP_PROJECT)
        set(TRADINGAPP_VS_STARTUP_PROJECT TradingApp)
    endif()
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTY VS_STARTUP_PROJECT "${TRADINGAPP_VS_STARTUP_PROJECT}")

    # Debugging from the IDE needs the Qt DLLs on PATH; without this the app
    # starts with a "Qt6Cored.dll was not found" dialog.
    set_target_properties(TradingApp PROPERTIES
        VS_DEBUGGER_ENVIRONMENT "PATH=${QT6_INSTALL_PREFIX}/bin;%PATH%"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    # Group the generated moc/rcc/uic targets so Solution Explorer stays readable.
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY AUTOGEN_TARGETS_FOLDER "Qt autogen")
endif()

# ---------------------------------------------------------------------------
# Tests (desktop only; see docs/test_spec.md and tools/run_tests.sh)
# ---------------------------------------------------------------------------
# include(CTest) defines the standard BUILD_TESTING option (default ON), so
# `cmake -DBUILD_TESTING=OFF` skips the suite (e.g. for deployment builds).
if(NOT ANDROID AND NOT IOS)
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()

# ---------------------------------------------------------------------------
# Android
# ---------------------------------------------------------------------------
# Only evaluated when configuring with an Android Qt kit (use the kit's
# qt-cmake wrapper). qt_add_executable() then builds the app as a shared library
# and androiddeployqt packages it into an APK automatically.
if(ANDROID)
    # Pin the SDK levels and name the package explicitly. android-35 is what
    # tools/build_android.sh installs; min 28 keeps Qt 6's own floor.
    #
    # QT_ANDROID_PACKAGE_SOURCE_DIR is NOT optional here: Qt 6.11 offers no CMake
    # property for manifest permissions, and this app is an HTTPS REST client — without
    # android.permission.INTERNET (declared in packaging/android/AndroidManifest.xml)
    # every request fails and only the simulation feed works. The file is Qt's own
    # template plus that permission; see the comment in it before bumping Qt.
    set_target_properties(TradingApp PROPERTIES
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 35
        # COMPILE sdk, deliberately ahead of the target: Qt 6.11's Android bindings
        # depend on androidx.core:core:1.17.0, which refuses to be consumed by a
        # project compiling against less than API 36 ("checkDebugAarMetadata FAILED").
        # compileSdk only decides which APIs the Java side may reference; targetSdk —
        # the runtime-behaviour opt-in — stays at 35 on purpose.
        QT_ANDROID_COMPILE_SDK_VERSION 36
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packaging/android"
        QT_ANDROID_APP_NAME "TradingApp"
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
    )

    # eToro is HTTPS-only, so QNetworkAccessManager needs OpenSSL at runtime.
    # The Android build of Qt does not ship OpenSSL inside the APK, so pull in
    # prebuilt libraries and add them to QT_ANDROID_EXTRA_LIBS. Replace this with
    # your own OpenSSL if you'd rather not fetch from the network at configure.
    include(FetchContent)
    FetchContent_Declare(android_openssl
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        URL https://github.com/KDAB/android_openssl/archive/refs/heads/master.zip
    )
    FetchContent_MakeAvailable(android_openssl)
    # MakeAvailable only add_subdirectory()s the package. The helper that appends the
    # prebuilt libcrypto/libssl to QT_ANDROID_EXTRA_LIBS lives in this file, so it has
    # to be included explicitly — without it the configure step dies with
    # "Unknown CMake command add_android_openssl_libraries".
    include("${android_openssl_SOURCE_DIR}/android_openssl.cmake")
    add_android_openssl_libraries(TradingApp)
endif()

# ---------------------------------------------------------------------------
# Desktop deployment (Windows / macOS / Linux)
# ---------------------------------------------------------------------------
# `cmake --install <build> --prefix <dir>` copies the binary plus its Qt runtime
# dependencies into a self-contained directory. On Windows this drives
# windeployqt (Qt DLLs, plugins, Schannel TLS backend); on macOS macdeployqt.
if(NOT ANDROID AND NOT IOS)
    install(TARGETS TradingApp
        BUNDLE  DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    # The second front end travels with the first when it was built. Same layers, same
    # view-model, read-only — but it is the ONLY binary that can draw the candlestick chart
    # (Qt Charts and Qt Graphs cannot share a process), so shipping one without the other
    # would ship a feature nobody can reach.
    if(TARGET TradingCockpit)
        install(TARGETS TradingCockpit
            BUNDLE  DESTINATION .
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

    # The licence texts travel INSIDE the artefact, not only beside it in the
    # release page. GPL-3.0-or-later requires the terms to reach whoever receives
    # the binary, and an AppImage or a portable ZIP is often passed on by itself.
    # Installed here rather than in each packager so the AppImage
    # (tools/package_appimage.sh) and the Windows portable ZIP
    # (tools/package_portable.ps1) — both of which run `cmake --install` — cannot
    # disagree about what they ship.
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/TradingApp
    )
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/LICENSES
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/TradingApp
    )

    # Generates a CMake script, run at install time, that collects and copies the
    # Qt runtime dependencies. NO_UNSUPPORTED_PLATFORM_ERROR makes it a no-op
    # where no deploy tool exists instead of erroring.
    #
    # TRADINGAPP_SKIP_QT_DEPLOY exists for the AppImage: linuxdeploy and its Qt
    # plugin do the bundling there (tools/package_appimage.sh), and Qt's generic
    # Linux deployment cannot be used for it anyway — it patches RUNPATH in place
    # and aborts with "The replacement path is too long for the RUNPATH entry" on
    # the plugins it just copied into an AppDir.
    option(TRADINGAPP_SKIP_QT_DEPLOY
        "Skip Qt's install-time runtime deployment (AppImage packaging bundles with linuxdeploy)" OFF)
    if(NOT TRADINGAPP_SKIP_QT_DEPLOY)
        qt_generate_deploy_app_script(
            TARGET TradingApp
            OUTPUT_SCRIPT deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
        )
        install(SCRIPT ${deploy_script})
    endif()
endif()

# ---------------------------------------------------------------------------
# Packaging (CPack)
# ---------------------------------------------------------------------------
# Running `cpack` from the build dir stages the install rules above — including
# the Qt deploy script — and wraps the self-contained result in an archive or
# installer. The defaults below always work; switch generator with `cpack -G`
# (e.g. NSIS, DragNDrop, DEB) using the metadata already set here. Desktop only.
if(NOT ANDROID AND NOT IOS)
    set(CPACK_PACKAGE_NAME "TradingApp")
    set(CPACK_PACKAGE_VENDOR "TradingApp")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "eToro Trader")
    set(CPACK_VERBATIM_VARIABLES ON)
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    # Start Menu / shortcut entry: "<exe-name-without-suffix>;<label>".
    set(CPACK_PACKAGE_EXECUTABLES "TradingApp;eToro Trader")

    if(WIN32)
        # ZIP always works; `cpack -G NSIS` builds an installer (needs makensis
        # on PATH). The NSIS_* metadata applies when that generator is selected.
        set(CPACK_GENERATOR "ZIP")
        set(CPACK_NSIS_DISPLAY_NAME "eToro Trader")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    elseif(APPLE)
        # ZIP by default; `cpack -G DragNDrop` builds a .dmg.
        set(CPACK_GENERATOR "ZIP")
    else()
        # TGZ by default; `cpack -G DEB` builds a .deb (Qt libs come from the
        # deploy step, not distro packages, so keep dependencies conservative).
        set(CPACK_GENERATOR "TGZ")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "TradingApp")
    endif()

    include(CPack)
endif()
