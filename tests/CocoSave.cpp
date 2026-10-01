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
// directory like the automatic writer, which is where coverage.sh runs each test. The call
// is registered with std::atexit from a namespace-scope initialiser, so it runs during
// exit() in every build whether or not Coco's own handler fires; when both run, the second
// record is a union of the first — a coverage database answers "was it executed", not "how
// often".
//
// Two ways to reach the function, because tests/ is EXCLUDED from instrumentation
// (`--cs-exclude-path`) and it is not documented whether CoverageScanner still defines
// `__COVERAGESCANNER__` for a file it does not instrument:
//   * under the macro, the call the Coco manual shows (CoverageScanner declares it);
//   * otherwise a WEAK reference on Linux (ELF) — resolved by Coco's runtime when the binary
//     is instrumented, null (and skipped) in every ordinary build, which keeps this file a
//     no-op in build/, build-san/, the MC/DC tree and CI.
// Linux only, deliberately: Mach-O's linker rejects a weak UNDEFINED function ("Undefined
// symbols for architecture arm64: ___coveragescanner_save" — the build-macos CI job, first
// push of this file), and MSVC has no weak references at all. Neither platform runs Coco
// here (tools/coverage.sh is the Linux path; tools/coverage.ps1 keeps the automatic writer
// and says the Windows set of silent suites is unverified).
//
// Validation belongs to the first licensed run after this landed: the "produced no
// execution report" notes from tools/coverage.sh must stop appearing for the eight suites.

#ifdef __COVERAGESCANNER__

#include <cstdlib>

namespace {
void saveCocoReport()
{
    __coveragescanner_save();
}
// A POD flag, not an object with a destructor: clazy's non-pod-global-static is part of the
// static-analysis gate, and std::atexit (noexcept) runs the save at exit just the same.
bool registerCocoSave() noexcept
{
    return std::atexit(saveCocoReport) == 0;
}
[[maybe_unused]] const bool cocoSaveRegistered = registerCocoSave();
}   // namespace

#elif defined(__GNUC__) && defined(__linux__)

#include <QtGlobal>

#include <cstdlib>
#include <iostream>

#include <unistd.h>

// Coco's runtime symbols, if the binary carries them. The reserved-identifier spelling is
// the library's own, not ours to choose — hence the three suppressions around this block.
// `__coveragescanner_filename` is the report path Coco itself intends to write; with
// TRADINGAPP_COCO_TRACE set, the hook says where that is and whether the file exists after
// the save — the diagnostic for a binary whose report never appears (2026-10-02: the hook
// was linked into tst_candles, Coco's save symbol was defined there, the test exited 0,
// and still no .csexe — this trace is how that is taken apart on a licensed machine).
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" void __coveragescanner_save(void) __attribute__((weak));
extern "C" const char *__coveragescanner_filename(void) __attribute__((weak));

namespace {
void saveCocoReport()
{
    if (__coveragescanner_save == nullptr) {
        return;
    }
    const bool trace = qEnvironmentVariableIsSet("TRADINGAPP_COCO_TRACE");
    const char *name =
        (__coveragescanner_filename != nullptr) ? __coveragescanner_filename() : nullptr;
    const char *shown = (name != nullptr) ? name : "(no filename from the runtime)";
    if (trace) {
        std::cerr << "coco: saving execution report to " << shown << '\n';
    }
    __coveragescanner_save();
    if (trace) {
        const bool present = (name != nullptr) && (::access(name, F_OK) == 0);
        std::cerr << "coco: after save, " << shown << (present ? " exists" : " does NOT exist")
                  << '\n';
    }
}
// A POD flag, not an object with a destructor (see the other branch).
bool registerCocoSave() noexcept
{
    return std::atexit(saveCocoReport) == 0;
}
[[maybe_unused]] const bool cocoSaveRegistered = registerCocoSave();
}   // namespace
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

#endif
