# Platform support

@page platforms Building for Linux, Windows, Android, iOS
@tableofcontents

One CMake project covers all four targets (REQ-N-001, DES-BLD-CMAKE). The
code base contains no platform-specific API usage outside `main.cpp`'s WSL
display-platform selection (guarded by environment checks) — everything else
is portable Qt.

## Linux (reference platform)

    cmake -S . -B build -DCMAKE_PREFIX_PATH=~/Qt/6.10.2/gcc_64
    cmake --build build && ./build/TradingApp
    cd build && ctest                       # test suite

## Windows

Install a Qt 6 kit (MSVC or MinGW) via the Qt Online Installer, then:

    cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64
    cmake --build build --config Release
    ctest --test-dir build -C Release       # the suite is platform-neutral
    cmake --install build --prefix dist     # runs windeployqt automatically
    cd build && cpack                       # ZIP; `cpack -G NSIS` = installer

Portability notes verified by inspection: HTTPS uses the Schannel TLS backend
that `qt_generate_deploy_app_script` deploys; no POSIX-only calls; file paths
go through Qt. CI recommendation: a `windows-latest` job running configure +
build + ctest is the missing automated evidence (tracked as the REQ-N-001 gap
in the traceability matrix).

## Android

Use the Android kit's wrapper so the toolchain/ABI are set up correctly:

    ~/Qt/6.10.2/android_arm64_v8a/bin/qt-cmake -S . -B build-android
    cmake --build build-android             # androiddeployqt builds the APK

Specifics already handled in `CMakeLists.txt`: OpenSSL libraries are fetched
and bundled (eToro is HTTPS-only), min/target SDK pinned (28/35), and the
config-file search includes the app-data dir (bundle a `config.json` +
`apiKeyEtoro.json` via `QT_ANDROID_PACKAGE_SOURCE_DIR` or use env-var
injection). Tests are desktop-only (`tests/` is skipped for Android builds).

## iOS / iPhone

Building for iOS requires a macOS host with Xcode (Apple's toolchain licence
does not permit cross-building from Linux/Windows):

    ~/Qt/6.10.2/ios/bin/qt-cmake -S . -B build-ios -GXcode
    open build-ios/TradingApp.xcodeproj     # sign with your team, deploy

The project guards iOS in CMake (`if(NOT ANDROID AND NOT IOS)` around
desktop deployment/tests); Qt Charts/Widgets run on iOS. Two practical
caveats: Apple's review guidelines are restrictive about third-party trading
apps (distribution realistically means TestFlight/private), and secrets must
be provisioned per-device — the same layered `Config` mechanism works with a
bundled non-secret `config.json` plus keys entered at runtime or shipped via
MDM. This target is *buildable-by-design but unverified here* (no macOS host
on the reference machine) — listed as part of the REQ-N-001 gap.
