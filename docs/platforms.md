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
