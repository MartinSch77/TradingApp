# Platform support

@page platforms Building for Linux (x86-64 and ARM64/Raspberry Pi), Windows, Android, iOS
@tableofcontents

One CMake project covers all targets (REQ-N-001, DES-BLD-CMAKE). The
code base contains no platform-specific API usage outside `main.cpp`'s WSL
display-platform selection (guarded by environment checks) — everything else
is portable Qt, and there is no architecture-specific code at all: no
intrinsics, no inline assembly, no assumptions about `char` signedness or type
sizes, which is why the ARM64 port below is a toolchain matter rather than a
code one.

Where the host architecture matters — the Qt kit directory name, the AppImage
architecture, the LLVM version — it is resolved in ONE place, `tools/common.sh`
(the Linux counterpart of `tools/common.ps1`). Run it to see what a machine
resolves to:

    tools/common.sh
    #   host arch      aarch64
    #   Qt kit dir     gcc_arm64
    #   aqt host       linux_arm64
    #   aqt arch       linux_gcc_arm64
    #   Qt prefix      /home/pi/Qt/6.11.1/gcc_arm64
    #   LLVM toolset   clang++-19 (MC/DC + TSan available)

## Linux (reference platform)

    cmake -S . -B build -DCMAKE_PREFIX_PATH=~/Qt/6.10.2/gcc_64
    cmake --build build && ./build/TradingApp
    cd build && ctest                       # test suite

## Raspberry Pi (Linux ARM64)

Supported: any 64-bit-capable Pi — Pi 4, Pi 5, Pi 400, Compute Module 4/5,
Zero 2 W — running a **64-bit Raspberry Pi OS Trixie (Debian 13) or newer**, or
Ubuntu 24.04+ for ARM. Nothing about the app is x86: the ARM64 port is entirely a
matter of pointing the toolchain at the right Qt kit, which the scripts now do
themselves.

Two OS-level constraints decide this, and both were measured rather than
assumed (2026-08-04, `objdump -T` over the aqt kits):

* **glibc ≥ 2.38.** Qt's official *aarch64* binaries reference `GLIBC_2.38` in
  `libQt6Gui` and `libQt6Network` — in 6.8.3, 6.9.3 and 6.11.1 alike, so there is
  no older, laxer ARM64 kit to fall back on. The *x86-64* build of the same Qt
  stops at `GLIBC_2.34`, which is why this floor is an ARM-only surprise.
  Raspberry Pi OS **Trixie** has glibc 2.41 and is fine; **Bookworm** has 2.36 —
  below Qt's own floor, and no build setting on this side can change that.
  Bookworm's distribution Qt (6.4.2) is in turn below the app's Qt ≥ 6.5 floor,
  so a Bookworm Pi has no supported route: upgrade the OS (or run Ubuntu
  24.04 for Pi, glibc 2.39).
* **64-bit userland.** Qt publishes no desktop binaries for 32-bit ARM, and the
  32-bit distribution's Qt is 6.4 — below the same floor. Check with `uname -m`:
  `aarch64` is the supported answer, `armv7l`/`armv6l` is not (reinstall the
  64-bit image; the hardware is fine).

### Route 1: the project toolchain (what CI verifies)

    ./setup.sh install          # same command as on x86-64
    ./build_all.sh build test trace
    ./build/TradingApp

`setup.sh` needs no Raspberry-Pi-specific flags. It resolves the host
architecture and installs Qt's official **Linux ARM64** kit —
`aqt install-qt linux_arm64 desktop <version> linux_gcc_arm64 -m qtcharts`,
which lands in `~/Qt/<version>/gcc_arm64` — and every entry point
(`build_all.sh`, `tools/coverage.sh`, `tools/sanitize.sh`,
`tools/package_appimage.sh`, `axivion/start_analysis.sh`) picks that kit up
without `QT_PREFIX`. Two other things it handles that a Pi specifically needs:

* **CMake ≥ 4.2**, which no Debian release ships (Trixie has 3.31) — it comes
  from pipx, and the aarch64 wheels exist.
* **Packages the distribution does not have.** `apt-get install` is
  all-or-nothing, so one unknown name used to abandon the whole provisioning
  step. The apt list is now filtered against `apt-cache` and the dropped names
  are printed. On a Pi that is typically `clang-18`/`llvm-18`/`clang-tools-18`:
  Raspberry Pi OS Trixie ships clang-19, and the version is resolved at run time
  (below), so nothing needs those exact names.

### Route 2: the distribution's Qt (no aqt download)

    sudo apt-get install qt6-base-dev qt6-charts-dev qt6-base-dev-tools
    ./build_all.sh build test           # finds the system Qt on its own

When no `~/Qt` kit exists for the architecture, `tools/common.sh` resolves an
EMPTY prefix on purpose: the scripts then configure without
`CMAKE_PREFIX_PATH` and CMake finds the distribution Qt 6. `build_all.sh` says
which of the two it used in its first line. This route sidesteps the glibc
question entirely — the distribution's Qt is built against the distribution's
glibc — but it substitutes a version question: Trixie's Qt 6.8 clears the app's
Qt ≥ 6.5 floor, Bookworm's 6.4.2 does not.

### Route 3: don't build at all

`TradingApp-<version>-aarch64.AppImage` from the
[latest release](https://github.com/MartinSch77/TradingApp/releases/latest) —
one file, Qt bundled, `chmod +x` and run. Built by the same
`tools/package_appimage.sh` as the x86-64 package (it fetches the aarch64
linuxdeploy tooling, pinned by tag AND sha256 per architecture) on an
`ubuntu-24.04-arm` runner — 24.04 and not 22.04 because of the GLIBC_2.38 floor
above, so the artifact needs glibc ≥ 2.39: Trixie and Ubuntu 24.04 for Pi, not
Bookworm. This is also the answer for the small boards — a Zero 2 W has 512 MB of
RAM and will not compile a C++23 Qt application in reasonable time or memory, but
it runs the AppImage.

Build times for scale (4 cores): expect tens of minutes for
`build_all.sh build test` on a Pi 5 and to want 4 GB of RAM or swap for a
parallel build; `-j2` (`JOBS` is `nproc`) is the lever if the OOM killer
appears.

### Running it on the Pi

The ARM64 kit ships the same platform plugins as the x86-64 one, so all three
Pi situations are covered:

| Situation | Platform plugin | Notes |
|---|---|---|
| Pi OS Desktop, Pi 5 (labwc/Wayland) | `xcb` via XWayland, or `wayland` | Default works; nothing to set |
| Pi OS Desktop, Pi 4 and older (X11) | `xcb` | Default works |
| Pi OS Lite, no desktop (KMS/DRM) | `QT_QPA_PLATFORM=eglfs` | Full-screen appliance mode; the user must be in the `video` and `render` groups |
| Over SSH, tests only | `QT_QPA_PLATFORM=offscreen` | What CI and `tools/run_tests.sh` use |

Performance: the app's own work is either network-bound (the polled REST calls)
or already off the GUI thread (Monte-Carlo and plan building via QtConcurrent,
REQ-N-006), and the positions table is allocation-free per tick — so a Pi 4/5
is adequate for what it does. Use a **Release** build for daily use
(`./build_all.sh release`, or the AppImage, which is one): the compute paths run
5–20× faster than the Debug build the pipeline measures. The domain benchmarks
(`tools/profile.sh`, `/perf-check`) run on a Pi unchanged, but no numbers from
Pi hardware are recorded here — the figures in @ref verification are x86-64.

### What the quality pipeline can and cannot do on ARM64

| Stage | On a Raspberry Pi |
|---|---|
| build, test, trace, docs, analysis (cppcheck, clang-tidy, CSA, clazy, `g++ -fanalyzer`, lizard, PMD CPD, codespell) | run normally |
| coverage — gcov/lcov line+branch | runs normally |
| coverage — MC/DC, and sanitize — TSan | need clang ≥ 18; the version is **resolved**, not hardcoded (`llvm_suffix` in `tools/common.sh`), and both report `skipped` with the reason when the OS ships an older one. Trixie's clang-19 satisfies them |
| sanitize — ASan+UBSan, valgrind | run normally (valgrind supports aarch64); a missing valgrind now reports `skipped` instead of failing |
| axivion | **`skipped` always** — the Axivion Suite is published for x86-64 hosts only, which `start_analysis.sh` now says out loud. Run that stage on an x86-64 machine; the analysis is host-side, so nothing about it is Pi-specific |
| coverage — Squish Coco | not available (x86-64 Linux / Windows only), same `skipped` contract |

### Verification status

`.github/workflows/ci.yml` runs **`build-linux-arm64`** on GitHub's
`ubuntu-24.04-arm` runner: it installs the ARM64 Qt kit the way `setup.sh` does,
then runs `./build_all.sh build test trace` with **no** architecture arguments —
so a regression in kit resolution fails the job, warnings are errors, and every
test has to pass on aarch64. It then **starts the app** through the
`TRADINGAPP_SHOT` hook and uploads the PNGs, so the evidence includes the
aarch64 GUI rendering rather than only compiling.
`.github/workflows/release.yml` additionally builds the aarch64 AppImage and
smoke-starts it headless.

What that does **not** cover, and is therefore claimed by inspection only: the
Pi's own GPU/display stack (`eglfs` on V3D), thermals and SD-card I/O, and any
measured performance figure on Pi silicon.

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

## Building, per platform (moved from the README)

The full quality pipeline runs natively on **both Linux and Windows**, on
x86-64 and on ARM64 (Raspberry Pi included — the few stages whose tools are
x86-64-only report `skipped`, see [docs/platforms.md](../docs/platforms.md)). Each
`*.sh` entry point has a one-to-one PowerShell counterpart; see
[docs/windows.md](../docs/windows.md) for the complete tool mapping and the
Windows-specific notes.

On a naked Debian/Ubuntu Linux, `./setup.sh` installs every required tool
and dependency (compilers, CMake, Qt 6 incl. Charts via aqtinstall, the
clang-18/LLVM tooling, cppcheck/clazy/valgrind/lcov, lizard, PMD, Doxygen +
Java, StrictDoc/Doorstop) idempotently; `./setup.sh update` brings them to their
latest versions and `./setup.sh status` reports what is present. On Windows,
`.\setup.ps1` does the same through winget + pip + aqtinstall. License-bound
tools (Axivion Suite, Squish Coco) are detected and reported but must be
installed manually.

The repository has three top-level entry points:

```bash
./build_all.sh            # everything: app, tests, traceability, docs,
                          # coverage, static analysis, sanitizers, Axivion,
                          # and the PDF quality report
./build_all.sh app        # ONLY the TradingApp executable (build/TradingApp)
./build_all.sh build test # any subset of stages, in order
./build_all.sh --skip axivion  # everything except the (slow) Axivion analysis
./clean_all.sh [--deep]   # remove everything generated
```

```powershell
.\setup.ps1                     # provision/verify the Windows toolchain
.\build_all.ps1                 # same stages, same order
.\build_all.ps1 build test      # any subset of stages
.\build_all.ps1 -Skip axivion   # everything except the (slow) Axivion analysis
.\clean_all.ps1 [-Deep]         # remove everything generated
```

Stages: `build test trace docs coverage analysis sanitize axivion report`
(default: all, continuing past failing stages with a summary at the end); the
last one writes **`downloads/TradingApp-quality-report.pdf`** — one colour PDF
with the run's verdict, every test function and its result, the traceability
highlights per requirement, the analyzer findings, code metrics, coverage and
the sanitizer results (`tools/make_report.py`, shared by both platforms); `app`,
`release` and `android` (APK via androiddeployqt) are extra stages that are only run when named, and `build_all.ps1`
additionally offers `vs` and `deploy`. For a different single CMake target:
`cmake --build build --target <name>`.

**No licence, no problem.** Stage outcomes are `ok` / `skipped` / `FAILED`. A
stage needing a tool that is license-bound (Axivion Suite, Squish Coco) or
otherwise absent reports **`skipped`** with a message saying what to install, and
does *not* fail the run — so the whole pipeline goes green on a machine with only
the free toolchain. Everything **open source** that the pipeline needs is
installed for you by `./setup.sh` / `.\setup.ps1`; `setup.sh status` and
`setup.ps1 status` list what is present, what is license-bound, and what has no
counterpart on the platform.

`build_all.ps1` selects the Qt kit itself (newest kit containing Qt6Charts,
MSVC preferred) and imports the Visual Studio developer environment into the
session, so no "x64 Native Tools" prompt is required. Override the kit with
`$env:QT_PREFIX` or `-QtKit mingw_64`.

Requires Qt 6 with the **Widgets**, **Network**, and **Charts** modules
(developed against Qt 6.11.1), CMake ≥ 4.2 and a C++23-capable compiler
(GCC 13+, Clang 17+, MSVC 19.38+). The sources are
plain cross-platform Qt/C++ — the same code builds on Linux (x86-64 **and**
ARM64), Windows, and Android; only the Qt kit and the packaging step differ.

## Linux / macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
cmake --build build
./build/TradingApp
```

## Raspberry Pi (ARM64)

A Pi 4/5 with a **64-bit Raspberry Pi OS Trixie** (Debian 13) or newer builds and
runs it with the ordinary commands — the scripts resolve the ARM64 Qt kit
(`~/Qt/<ver>/gcc_arm64`) themselves, so nothing needs an extra flag:

```bash
./setup.sh install                  # installs Qt's Linux ARM64 kit + the toolchain
./build_all.sh build test trace
./build/TradingApp
```

`sudo apt-get install qt6-base-dev qt6-charts-dev` instead of the aqt kit also
works on Trixie, and `tools/package_appimage.sh` produces
`TradingApp-<version>-aarch64.AppImage`. Bookworm (Debian 12) has no supported
route: its glibc 2.36 is below what Qt's aarch64 binaries need (2.38) and its own
Qt is 6.4.2, below the app's floor — upgrade the OS.
Details — display server (`xcb` / `wayland` / `eglfs`), which pipeline stages
report `skipped` on ARM64 and why, and what CI verifies —
in [docs/platforms.md](../docs/platforms.md).

## Windows (MSVC)

Install the Qt 6 **msvc2022_64** kit (with the Charts module), Visual Studio 2022
(or its Build Tools), and CMake — or let `.\setup.ps1` do it. Then, from an
ordinary PowerShell prompt:

```powershell
.\build_all.ps1 app          # -> build\TradingApp.exe
.\build_all.ps1 build test   # app + tests + JUnit results
```

Or by hand, from a *Developer* command prompt:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64
cmake --build build
build\TradingApp.exe
```

The **MinGW** kit works too: `.\build_all.ps1 -QtKit mingw_64` (the matching
`C:\Qt\Tools\mingw*\bin` is put on PATH automatically).

### Visual Studio IDE

`CMakeLists.txt` is the single description of the build, so the `.sln` is
**generated**, not committed:

```powershell
.\tools\make_vs_solution.ps1 -Open   # -> build-vs\TradingApp.sln
.\build_all.ps1 vs                   # same thing, as a named stage
```

The solution contains `TradingApp`, `trading_domain`, `trading_services` and all
12 `tst_*` projects in Debug/Release/RelWithDebInfo; `TradingApp` is the startup
project, the Qt DLL directory is already on the debugger's PATH, and
*Test → Run All Tests* works. Re-run the script after adding or removing source
files. Two `.sln`-free alternatives, both driven by
[CMakePresets.json](../CMakePresets.json):

- **File → Open → Folder** on the repository root — Visual Studio offers the
  `windows-msvc-debug` and `visual-studio` presets directly.
- `cmake --preset windows-msvc-debug && cmake --build --preset windows-msvc-debug`
  from any shell. Linux has `linux-gcc-debug` / `linux-gcc-release` presets too.
  All presets take the Qt kit from `$QT_PREFIX`.

To make the built executable runnable on its own — Qt DLLs, the platform
plugin, the Schannel TLS backend and the compiler runtime copied next to it:

```powershell
.\build_all.ps1 deploy               # -> build\TradingApp.exe runs with nothing on PATH
.\tools\deploy_app.ps1 -IncludeTests # also make the tst_*.exe standalone
```

Qt 6 uses the **Schannel** TLS backend on Windows, so HTTPS to the eToro API
works with no OpenSSL install. For a distributable package rather than a
runnable build tree, see [Packaging](#packaging-desktop) below.

The Windows pipeline substitutes a few tools that do not exist there — MSVC
`/analyze` for `g++ -fanalyzer`, OpenCppCoverage for gcov/lcov, ASan (MSVC) plus
UBSan (clang-cl) for the combined GCC sanitizer build — and reports the genuine
gaps (clazy, TSan, valgrind) instead of hiding them. MC/DC coverage is measured
**twice**, by Squish Coco and by clang-cl/llvm-cov. Details, and the PowerShell
pitfalls worth knowing about, are in [docs/windows.md](../docs/windows.md).

## Android

Requires the Qt 6 **Android** kit (e.g. `android_arm64_v8a`), the Android SDK +
NDK, and a JDK. The simplest setup is to open the project in **Qt Creator** with
an Android kit selected, which fills in the SDK/NDK paths and toolchain. On the
command line, configure with the Android kit's `qt-cmake` wrapper:

```bash
~/Qt/6.11.1/android_arm64_v8a/bin/qt-cmake -S . -B build-android -G Ninja \
      -DQT_ANDROID_ABIS=arm64-v8a
cmake --build build-android --target apk     # produces the APK
```

`qt_add_executable()` builds the app as a shared library and `androiddeployqt`
packages it into an APK. The build **bundles OpenSSL** (fetched at configure
time — see [`CMakeLists.txt`](../CMakeLists.txt)) because Android's Qt does not ship
it and HTTPS to eToro would otherwise fail.

Caveats specific to Android:

- The UI is **Qt Widgets** — a desktop-style layout. It runs on a phone but is
  not touch-optimised.
- The APK is sandboxed and has no working directory or `ETORO_*` environment, so
  `config.json` / env-var configuration does not apply and the app starts in
  **SIMULATION** mode. To trade for real you would have to ship credentials into
  the app's data dir (bundle a `config.json` via `QT_ANDROID_PACKAGE_SOURCE_DIR`,
  or write to `AppConfigLocation`) — private builds only; never publish keys.

## Download a ready-made build

| Platform | Artifact | How to run it |
|---|---|---|
| Linux (x86-64) | `TradingApp-<version>-x86_64.AppImage` | `chmod +x` it and run — one file, no install, Qt bundled |
| Linux (ARM64, incl. Raspberry Pi 4/5) | `TradingApp-<version>-aarch64.AppImage` | same — needs a 64-bit OS with glibc ≥ 2.39: Raspberry Pi OS **Trixie** or Ubuntu 24.04 for Pi (Qt's own aarch64 binaries require 2.38, see [docs/platforms.md](../docs/platforms.md)) |
| Windows (x64) | `TradingApp-<version>-windows-x64.zip` | unzip anywhere and run `TradingApp.exe` — every DLL is inside, no Qt and no MSVC redistributable needed |
| Android (arm64-v8a) | `TradingApp-<version>-arm64-v8a.apk` | `adb install` it, or copy it to the phone and open it (allow installs from unknown sources). Built and signed by [`tools/build_android.sh`](../tools/build_android.sh) |
| macOS | *build from source* | `cmake --preset default && cmake --build build` with Qt 6.11 — the CI job `build-macos` keeps it working; no notarised `.dmg` is published (that needs an Apple Developer identity) |
| iOS / iPhone | *build from source, Xcode* | `~/Qt/<ver>/ios/bin/qt-cmake -S . -B build-ios -GXcode`, then sign with your own team in Xcode and deploy ([docs/platforms.md](../docs/platforms.md)). **No `.ipa` is published**: Apple only installs signed builds, signing needs a paid Apple Developer identity, and that identity cannot live in a public CI. The layout is also desktop-first — treat iOS as "it compiles and runs", not as a phone UI |
| every release | `TradingApp-quality-report.pdf` | the whole quality run in one PDF: requirements traceability, test results, all eight analyzers, code metrics, clone detection, coverage and the sanitizers |

All are attached to the [latest release](https://github.com/MartinSch77/TradingApp/releases/latest),
each with a `.sha256` next to it. Build them yourself into `downloads/`:

```bash
tools/package_appimage.sh          # Linux  -> downloads/TradingApp-<version>-<arch>.AppImage
                                   #           (arch = the host's: x86_64 or aarch64)
tools/build_android.sh --release   # Android -> downloads/TradingApp-<version>-arm64-v8a.apk
python3 tools/make_report.py       # the PDF -> downloads/TradingApp-quality-report.pdf
```
```powershell
.\tools\package_portable.ps1       # Windows -> downloads\TradingApp-<version>-windows-x64.zip
```

The PDF the release pipeline attaches marks the **Axivion** section "not run": the
Suite is licence-bound and x86-64-host-only, so no public runner can produce it. To
publish a report that includes it, run the full pipeline on a machine that has the
Suite and attach that PDF over the CI one:

```bash
./build_all.sh                     # all stages, Axivion included, then the report
gh release upload v<version> downloads/TradingApp-quality-report.pdf --clobber
```

`downloads/` is git-ignored — the artifacts belong to a release, not to the
history. Both scripts build their own Release tree, bundle the Qt runtime
(linuxdeploy + its Qt plugin on Linux, windeployqt on Windows) and print a
SHA-256; `.github/workflows/release.yml` runs these same two scripts on a `v*`
tag and attaches the results to the release. Without API keys the app starts in
SIMULATION mode, so a downloaded build is safe to try.

Two caveats worth knowing: the AppImage is built on Ubuntu 22.04, so it needs
glibc ≥ 2.35 (any distro from 2022 onwards), and it deliberately does **not**
bundle OpenSSL — Qt loads the system libssl for HTTPS, which keeps the download
out of the business of shipping a frozen TLS stack.

## Packaging (desktop)

`cmake --install build --prefix dist` produces a self-contained folder with the
binary and every Qt library/plugin it needs, ready to zip or hand to `cpack`. It
runs **windeployqt** on Windows and **macdeployqt** on macOS automatically
(`-DTRADINGAPP_SKIP_QT_DEPLOY=ON` turns that step off, which is what the AppImage
build does — linuxdeploy handles the bundling there).

**build linux / windows / macos** = the three platform jobs of
[ci.yml](../.github/workflows/ci.yml), reported separately because a GitHub badge
reports a workflow and not a job — and the defects this codebase has hit were
platform-specific (MSVC rejected code that GCC and clang accepted). Each job
publishes its own status, failures included, so a red badge names the platform.
The same run also covers traceability, the sanitizers and the static analysis.
**tests** = the Qt Test suite on its own
([tests.yml](../.github/workflows/tests.yml)), which also measures the **coverage**
number (line coverage of the domain + services layers, published as a badge
endpoint on the `badges` branch — no third-party coverage service involved).
**sonarcloud** is the SonarCloud quality gate for project
`MartinSch77_TradingApp` — note that it comes from SonarCloud's *automatic
analysis* of this public repository, not from
[sonarcloud.yml](../.github/workflows/sonarcloud.yml), which stays a no-op until the
`SONAR_TOKEN` secret exists. **coverity** is the Coverity Scan build status;
that analysis runs server-side on a weekly submission, so the badge trails the
other ones by design. **latest release** links the downloads below.
