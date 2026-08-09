# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Test suite: unit tests over the pure domain layer plus integration tests over
# the services (config layering, simulation engine, HTTP plumbing and the eToro
# client against an in-process mock server). Every test function carries the
# traceability tags (//! @tstid TS-… @design DES-… plus the plain-comment
# StrictDoc marker // @relation(REQ-…, scope=function)) that tools/trace_report.py
# joins with docs/requirements.md, docs/design.md, docs/test_spec.md and the
# JUnit results in test-results/.

find_package(Qt6 REQUIRED COMPONENTS Test)

# One test executable per area; JUnit output for the traceability report is
# produced by tools/run_tests.sh (ctest itself only records pass/fail).
function(tradingapp_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE trading_services Qt6::Test)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME ${name} COMMAND ${name})
    # IDE-only (multi-config generators): collect the test projects in a folder
    # and put the Qt DLLs on PATH so a test can be debugged straight from the
    # generated Visual Studio solution. Ignored by Ninja/Makefiles.
    if(CMAKE_CONFIGURATION_TYPES)
        set_target_properties(${name} PROPERTIES
            FOLDER "Tests"
            VS_DEBUGGER_ENVIRONMENT "PATH=${QT6_INSTALL_PREFIX}/bin;%PATH%")
    endif()
endfunction()

# --- unit tests (domain layer only) ----------------------------------------
tradingapp_add_test(tst_models tst_models.cpp)
tradingapp_add_test(tst_money tst_money.cpp)
tradingapp_add_test(tst_indicators tst_indicators.cpp)
tradingapp_add_test(tst_forecasting tst_forecasting.cpp)
tradingapp_add_test(tst_signalensemble tst_signalensemble.cpp)
tradingapp_add_test(tst_decisionengine tst_decisionengine.cpp)
tradingapp_add_test(tst_tradeplan tst_tradeplan.cpp)
tradingapp_add_test(tst_tradescript tst_tradescript.cpp)

# Crowd Sentiment & AI subsystem — Phase 1 data foundation (REQ-F-039). The store test drives an
# in-memory SQLite database; trading_services links Qt6::Sql, so it is available transitively.
tradingapp_add_test(tst_crowdobservation tst_crowdobservation.cpp)
tradingapp_add_test(tst_mockcrowdprovider tst_mockcrowdprovider.cpp)
tradingapp_add_test(tst_crowdstore tst_crowdstore.cpp)
# TS-SCRIPT-008 parses the reference script that ships in examples/ — the file
# users copy has to keep matching the parser that reads it.
target_compile_definitions(tst_tradescript PRIVATE
    TRADINGAPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
tradingapp_add_test(tst_papertrader tst_papertrader.cpp)
tradingapp_add_test(tst_botnet tst_botnet.cpp)
tradingapp_add_test(tst_indexconfluence tst_indexconfluence.cpp)
tradingapp_add_test(tst_leadsignal tst_leadsignal.cpp)
tradingapp_add_test(tst_predictionledger tst_predictionledger.cpp)
tradingapp_add_test(tst_decisionlog tst_decisionlog.cpp)
tradingapp_add_test(tst_candles tst_candles.cpp)
tradingapp_add_test(tst_confirmgate tst_confirmgate.cpp)
# The bot-console front end's text, compiled directly (the console layer is not a library),
# with NO terminal dependency — every line it shows is checkable headless.
tradingapp_add_test(tst_botconsoleview tst_botconsoleview.cpp
    ../src/console/BotConsoleView.cpp ../src/console/BotConsoleView.h)
target_include_directories(tst_botconsoleview PRIVATE ../src)
# The one test that runs the TRAINER (tools/train_bot_net.py) and reads its output
# back: the C++ and the Python halves of REQ-F-033 have to agree on the model file,
# and only an end-to-end round trip can show that they do.
target_compile_definitions(tst_botnet PRIVATE
    TRADINGAPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
tradingapp_add_test(tst_positionmath tst_positionmath.cpp)
tradingapp_add_test(tst_eventinsight tst_eventinsight.cpp)
tradingapp_add_test(tst_benchmarks tst_benchmarks.cpp)

# --- integration tests (services layer) ------------------------------------
tradingapp_add_test(tst_config tst_config.cpp)
tradingapp_add_test(tst_ordergateway tst_ordergateway.cpp)
tradingapp_add_test(tst_simulationengine tst_simulationengine.cpp)
tradingapp_add_test(tst_jsonhttp tst_jsonhttp.cpp MockHttpServer.cpp MockHttpServer.h)
tradingapp_add_test(tst_etoroclient tst_etoroclient.cpp MockHttpServer.cpp MockHttpServer.h)
tradingapp_add_test(tst_etoroclientlive tst_etoroclientlive.cpp MockHttpServer.cpp MockHttpServer.h)
tradingapp_add_test(tst_marketfeeds tst_marketfeeds.cpp MockHttpServer.cpp MockHttpServer.h)
tradingapp_add_test(tst_economiccalendar tst_economiccalendar.cpp MockHttpServer.cpp MockHttpServer.h)
tradingapp_add_test(tst_aiadvisor tst_aiadvisor.cpp MockHttpServer.cpp MockHttpServer.h)
tradingapp_add_test(tst_ollamaadvisor tst_ollamaadvisor.cpp MockHttpServer.cpp MockHttpServer.h)

# --- unit test over the ui table model (headless: model only, no widgets) ---
# The ui layer is compiled into the app executable, not a library, so the test
# compiles PositionsModel directly; Widgets is needed for its thin delegate.
tradingapp_add_test(tst_positionsmodel tst_positionsmodel.cpp
    ../src/ui/PositionsModel.cpp ../src/ui/PositionsModel.h)
target_link_libraries(tst_positionsmodel PRIVATE Qt6::Widgets)

tradingapp_add_test(tst_leadgauge tst_leadgauge.cpp
    ../src/ui/LeadGauge.cpp ../src/ui/LeadGauge.h)
target_link_libraries(tst_leadgauge PRIVATE Qt6::Widgets)

# --- the declarative cockpit's data, tested WITHOUT rendering (REQ-F-038) -----
# Same reason as above: the ui layer is not a library, so the view-model is compiled
# directly. Deliberately NO Quick/QuickWidgets dependency — the whole design point is
# that every claim the QML makes is checkable headless, with no window and no GPU.
tradingapp_add_test(tst_cockpitmodel tst_cockpitmodel.cpp
    ../src/ui/CockpitModel.cpp ../src/ui/CockpitModel.h)
