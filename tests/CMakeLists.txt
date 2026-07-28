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
tradingapp_add_test(tst_indicators tst_indicators.cpp)
tradingapp_add_test(tst_forecasting tst_forecasting.cpp)
tradingapp_add_test(tst_signalensemble tst_signalensemble.cpp)
tradingapp_add_test(tst_decisionengine tst_decisionengine.cpp)
tradingapp_add_test(tst_tradeplan tst_tradeplan.cpp)
tradingapp_add_test(tst_positionmath tst_positionmath.cpp)
tradingapp_add_test(tst_eventinsight tst_eventinsight.cpp)
tradingapp_add_test(tst_benchmarks tst_benchmarks.cpp)

# --- integration tests (services layer) ------------------------------------
tradingapp_add_test(tst_config tst_config.cpp)
tradingapp_add_test(tst_simulationengine tst_simulationengine.cpp)
tradingapp_add_test(tst_jsonhttp tst_jsonhttp.cpp MockHttpServer.cpp MockHttpServer.h)
tradingapp_add_test(tst_etoroclient tst_etoroclient.cpp MockHttpServer.cpp MockHttpServer.h)

# --- unit test over the ui table model (headless: model only, no widgets) ---
# The ui layer is compiled into the app executable, not a library, so the test
# compiles PositionsModel directly; Widgets is needed for its thin delegate.
tradingapp_add_test(tst_positionsmodel tst_positionsmodel.cpp
    ../src/ui/PositionsModel.cpp ../src/ui/PositionsModel.h)
target_link_libraries(tst_positionsmodel PRIVATE Qt6::Widgets)
