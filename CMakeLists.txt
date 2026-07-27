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
    VERSION 1.0.0
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

# Qt 6.10.2 lives under ~/Qt on this machine; allow overriding via -DCMAKE_PREFIX_PATH.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Charts Concurrent)

qt_standard_project_setup()

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
)
target_include_directories(trading_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(trading_domain PUBLIC Qt6::Core)

qt_add_library(trading_services STATIC
    src/services/AiAdvisor.cpp
    src/services/AiAdvisor.h
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
    src/ui/ScreenerDialog.h
    src/ui/ChartView.cpp
    src/ui/ChartView.h
)

target_link_libraries(TradingApp PRIVATE
    trading_services
    Qt6::Gui
    Qt6::Widgets
    Qt6::Charts
    Qt6::Concurrent
)

# Put the binary directly in the build dir root for convenience.
set_target_properties(TradingApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

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
    # Optional: pin the Android SDK levels. Without these, androiddeployqt uses
    # the kit defaults; adjust to match the SDK you have installed.
    set_target_properties(TradingApp PROPERTIES
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 35
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

    # Generates a CMake script, run at install time, that collects and copies the
    # Qt runtime dependencies. NO_UNSUPPORTED_PLATFORM_ERROR makes it a no-op
    # where no deploy tool exists (e.g. plain Linux) instead of erroring.
    qt_generate_deploy_app_script(
        TARGET TradingApp
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${deploy_script})
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
