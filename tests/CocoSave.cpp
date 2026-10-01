// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Squish Coco: save the execution report EXPLICITLY at exit.
//
// Compiled into every test binary (tests/CMakeLists.txt, tradingapp_add_test). Coco's own
// atexit writer does not fire for a growing set of suites — four measured 2026-08-08
// (tst_models, tst_indicators, tst_candles, tst_confirmgate), eight on 2026-10-01 (those
// plus tst_architecture, tst_leadgauge, tst_rollingzscore, tst_swingpullbackstrategy) — all
// of which exit 0 and write no <name>.csexe at all, so their coverage was missing from the
// merged figure and tools/coverage.sh could only call that figure a FLOOR. Several of the
// affected suites are the ONLY tests of their domain file (Candles, ConfirmGate, LeadGauge,
// RollingZScore, SwingPullbackStrategyV1), which is exactly where the unit MC/DC number
// was undercounted.
//
// `__coveragescanner_save()` is the documented CoverageScanner library call for writing
// the report from the program itself; it writes <executable>.csexe into the working
// directory like the automatic writer, which is where coverage.sh runs each test. A static
// object's destructor runs during exit() in every build, so the save happens whether or not
// Coco's own handler fires; when both run, the second record is a union of the first — a
// coverage database answers "was it executed", not "how often".
//
// Two ways to reach the function, because tests/ is EXCLUDED from instrumentation
// (`--cs-exclude-path`) and it is not documented whether CoverageScanner still defines
// `__COVERAGESCANNER__` for a file it does not instrument:
//   * under the macro, the call the Coco manual shows (CoverageScanner declares it);
//   * otherwise a WEAK reference on GCC/clang — resolved by Coco's runtime when the binary
//     is instrumented, null (and skipped) in every ordinary build, which keeps this file a
//     no-op in build/, build-san/, the MC/DC tree and CI.
// MSVC has no weak references; the Windows Coco path keeps relying on the automatic writer
// (tools/coverage.ps1 says the Windows set of silent suites is unverified).
//
// Validation belongs to the first licensed run after this landed: the "produced no
// execution report" notes from tools/coverage.sh must stop appearing for the eight suites.

#ifdef __COVERAGESCANNER__

namespace {
struct CocoSaveAtExit {
    CocoSaveAtExit() = default;
    CocoSaveAtExit(const CocoSaveAtExit &) = delete;
    CocoSaveAtExit(CocoSaveAtExit &&) = delete;
    CocoSaveAtExit &operator=(const CocoSaveAtExit &) = delete;
    CocoSaveAtExit &operator=(CocoSaveAtExit &&) = delete;
    ~CocoSaveAtExit() { __coveragescanner_save(); }
};
const CocoSaveAtExit cocoSaveAtExit;
}   // namespace

#elif defined(__GNUC__) && !defined(_WIN32)

// Coco's runtime symbol, if the binary carries it. The reserved-identifier spelling is the
// library's own name, not ours to choose — hence the three suppressions around this block.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" void __coveragescanner_save(void) __attribute__((weak));

namespace {
struct CocoSaveAtExit {
    CocoSaveAtExit() = default;
    CocoSaveAtExit(const CocoSaveAtExit &) = delete;
    CocoSaveAtExit(CocoSaveAtExit &&) = delete;
    CocoSaveAtExit &operator=(const CocoSaveAtExit &) = delete;
    CocoSaveAtExit &operator=(CocoSaveAtExit &&) = delete;
    ~CocoSaveAtExit()
    {
        if (__coveragescanner_save != nullptr) {
            __coveragescanner_save();
        }
    }
};
const CocoSaveAtExit cocoSaveAtExit;
}   // namespace
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

#endif
