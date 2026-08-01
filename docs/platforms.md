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

## Windows (MSVC or MinGW)

Install a Qt 6 kit (MSVC or MinGW) via the Qt Online Installer — or run
`.\setup.ps1`, which provisions the whole toolchain — then:

    .\build_all.ps1 build test              # app + tests, kit auto-detected
    cmake --install build --prefix dist     # runs windeployqt automatically
    cd build && cpack                       # ZIP; `cpack -G NSIS` = installer

By hand, without the wrapper:

    cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.9.2/msvc2022_64
    cmake --build build
    ctest --test-dir build                  # the suite is platform-neutral

Windows is **verified, not merely portable-by-inspection**: MSVC 19.44 builds
the app and all 12 test executables without warnings, all 12 pass, and the
window comes up. HTTPS uses the Schannel TLS backend that
`qt_generate_deploy_app_script` deploys; there are no POSIX-only calls and file
paths go through Qt.

The complete quality pipeline runs on Windows too — every `*.sh` entry point has
a PowerShell counterpart. See @ref windows for the script-by-script mapping,
the tool substitutions (MSVC `/analyze` for `g++ -fanalyzer`, OpenCppCoverage for
gcov/lcov, ASan+UBSan in two trees for the single GCC sanitizer build), the two
independent MC/DC measurements (Squish Coco and clang-cl/llvm-cov), and the three
things that genuinely have no Windows counterpart (clazy, ThreadSanitizer,
valgrind).

CI: the `build-windows` job in `.github/workflows/ci.yml` runs on
`windows-latest` and executes `.\build_all.ps1 build test trace` (plus a
report-only MSVC `/analyze` + clang-tidy step), providing the automated
Windows evidence for REQ-N-001.

## Android

Provisioned and built by script — no hand-assembled toolchain:

    ./setup.sh android                       # SDK + NDK + system image + Qt kits (~6 GB)
    tools/build_android.sh                   # APK -> downloads/ (x86_64, for an emulator)
    tools/build_android.sh --abi android_arm64_v8a   # …and for real devices
    tools/build_android.sh --run             # build, boot an emulator, screenshot
    ./build_all.sh android                   # the same as an extra pipeline stage

`.\setup.ps1 android`, `tools\build_android.ps1` and `tools\run_android.ps1` are the
Windows counterparts. Both build scripts exit **3 = skipped** when a prerequisite is
missing, so naming the stage on a desktop-only machine reports `skipped` rather than
failing.

### Facts measured on 2026-08-01 (Qt 6.11.1, NDK 27.2.12479018, API 35)

Three things were wrong in the previously never-built Android path, all now fixed:

1. **`add_android_openssl_libraries` was undefined.** `FetchContent_MakeAvailable`
   only `add_subdirectory()`s KDAB's android_openssl; the helper that appends the
   prebuilt `libcrypto_3.so`/`libssl_3.so` to `QT_ANDROID_EXTRA_LIBS` lives in
   `android_openssl.cmake`, which has to be `include()`d explicitly. Without it the
   configure step dies with *Unknown CMake command*.
2. **compileSdk had to move to 36.** Qt 6.11's Android bindings depend on
   `androidx.core:core:1.17.0`, which refuses to be consumed by a project compiling
   against less than API 36 (`checkDebugAarMetadata FAILED`). `QT_ANDROID_COMPILE_SDK_VERSION 36`
   with `QT_ANDROID_TARGET_SDK_VERSION` left at 35: compileSdk only decides which
   Java APIs may be referenced, targetSdk is the runtime-behaviour opt-in.
3. **No INTERNET permission.** Qt's default `AndroidManifest.xml` template declares
   none, and Qt 6.11 has no CMake property for permissions — so the app, which is
   nothing but an HTTPS REST client, could only ever have run its simulated feed.
   `packaging/android/AndroidManifest.xml` (Qt's own template plus INTERNET and
   ACCESS_NETWORK_STATE) is now pointed at by `QT_ANDROID_PACKAGE_SOURCE_DIR`.
   *Diff that file against the template of any Qt version you upgrade to* — Qt adds
   attributes there between minor releases.

The NDK version is **not** a free choice: Qt is built against exactly one, recorded in
the kit's own `modules/*.json` as `ndk_version`. Both build scripts read it from there
instead of hardcoding, and warn when the installed NDK differs — a mismatch links an
APK that fails to load at runtime, which is far more expensive to diagnose than a
warning.

Resulting artefacts: `TradingApp-1.0.0-x86_64-debug.apk` (22 MB) and
`TradingApp-1.0.0-arm64_v8a-debug.apk` (21 MB), each bundling Qt Widgets, Qt Charts
and OpenSSL 3. Tests stay desktop-only (`tests/` is skipped for Android builds), so
`android` is a packaging target, not a verification one.

### Emulator inside WSL2

The emulator needs `/dev/kvm`, and WSL2 only exposes it with nested virtualisation:

    # %USERPROFILE%\.wslconfig
    [wsl2]
    nestedVirtualization=true      # then: wsl --shutdown

That was already on here. The remaining catch is access, not existence: the device is
`root:kvm 0660`, so the user must be in the `kvm` group —

    sudo usermod -aG kvm $USER     # then start a NEW login shell

`tools/run_android.sh` checks readability *and* writability and reports `skipped` with
that exact command rather than starting an emulator that would fall back to software
emulation and appear to hang: a cold Android 15 x86_64 boot under TCG takes tens of
minutes versus well under a minute with KVM.

The runner is headless on purpose (`-no-window -gpu swiftshader_indirect`) and pulls
its evidence with `adb exec-out screencap`: no display needed, reproducible, and
usable as a CI artefact. WSLg can show the emulator window instead if you drop
`-no-window`.

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
