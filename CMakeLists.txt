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
    VERSION 1.0.2
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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Charts Concurrent)

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
    src/domain/IndexConfluence.cpp
    src/domain/IndexConfluence.h
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
    src/ui/BotSimPanel.cpp
    src/ui/BotSimPanel.h
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
