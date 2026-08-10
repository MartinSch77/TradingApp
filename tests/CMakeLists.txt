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
# Phase 2 — transparent Crowd Score (REQ-F-040).
tradingapp_add_test(tst_rollingzscore tst_rollingzscore.cpp)
tradingapp_add_test(tst_crowdscore tst_crowdscore.cpp)
tradingapp_add_test(tst_crowdscorebuilder tst_crowdscorebuilder.cpp)
tradingapp_add_test(tst_crowdscoremodel tst_crowdscoremodel.cpp
    ../src/ui/CrowdScoreModel.cpp ../src/ui/CrowdScoreModel.h)
# Phase 3 — real network-backed providers (REQ-F-039), driven against the in-process mock HTTP
# server: valid parse, malformed/empty, network error, 429 rate-limit retry, no-key unavailable.
tradingapp_add_test(tst_crowdproviders tst_crowdproviders.cpp MockHttpServer.cpp MockHttpServer.h)
# Phase 4 — the OFFLINE training pipeline (REQ-F-041): the C++ CrowdStore writes real SQLite
# fixtures, the Python tools (tools/ml/) read them, and the leakage/label/split/refusal rules
# are asserted on the files that come back — the same cross-language contract discipline as
# TS-NET-004. The model-fitting test skips without the optional `./setup.sh ml` environment.
# CrowdFixtures carries the shared pipeline invocation/fixtures for the Phase 4 + 5 tests.
tradingapp_add_test(tst_crowdml tst_crowdml.cpp CrowdFixtures.cpp CrowdFixtures.h)
target_compile_definitions(tst_crowdml PRIVATE
    TRADINGAPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# Phase 5 — the OPTIONAL in-process inference (REQ-F-042): the pure metadata/assembly rules
# run everywhere; the round-trip test loads the very files the pipeline exports and skips
# without the runtime or the training environment — the licence-bound-stage discipline.
tradingapp_add_test(tst_crowdinference tst_crowdinference.cpp)
tradingapp_add_test(tst_crowdmodel tst_crowdmodel.cpp CrowdFixtures.cpp CrowdFixtures.h)
target_compile_definitions(tst_crowdmodel PRIVATE
    TRADINGAPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# Phase 7 — the collection loop (REQ-F-043), driven through ingest() with the deterministic
# mock's batches; no network, refreshNow() is never called here.
tradingapp_add_test(tst_crowdcollector tst_crowdcollector.cpp CrowdFixtures.cpp CrowdFixtures.h)
target_compile_definitions(tst_crowdcollector PRIVATE
    TRADINGAPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# Phase 6 — local text sentiment (REQ-F-044): tokenizer rules everywhere; the runner and the
# collector hook against the tiny bert-shaped fixture (make_finbert_fixture.py), never the
# real 400 MB model.
tradingapp_add_test(tst_finbert tst_finbert.cpp CrowdFixtures.cpp CrowdFixtures.h)
target_compile_definitions(tst_finbert PRIVATE
    TRADINGAPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
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
# The advise console's report (REQ-F-047), same discipline: compiled directly, no terminal.
tradingapp_add_test(tst_adviseview tst_adviseview.cpp
    ../src/console/AdviseView.cpp ../src/console/AdviseView.h)
target_include_directories(tst_adviseview PRIVATE ../src)
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
