// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The shared bound for spy waits and QTRY_* timeouts in the network-facing tests.
//
// 15 s is generous for a mock that answers in milliseconds: the margin absorbs CI
// load, nothing else. Under valgrind's memcheck the same tests run 20-50x slower,
// and on a loaded developer machine two of them (TS-CLI-005, TS-CLI-041) hit the
// 15 s on 2026-09-30 while the uninstrumented run passed in under a second. The
// sanitize stage therefore exports TRADINGAPP_TEST_WAIT_SCALE for its valgrind
// pass, and the bound scales with it; anywhere else the variable is unset and the
// bound is the literal 15 s the tests were written against.
#ifndef TRADINGAPP_TESTS_TESTWAIT_H
#define TRADINGAPP_TESTS_TESTWAIT_H

#include <QtGlobal>

namespace trading_test {

// noexcept: the bound initialises namespace-scope constants (cert-err58-cpp), and
// qEnvironmentVariableIntValue is itself noexcept.
inline qint32 scaledWaitMs(qint32 baseMs) noexcept
{
    const int scale = qEnvironmentVariableIntValue("TRADINGAPP_TEST_WAIT_SCALE");
    return (scale > 1) ? (baseMs * scale) : baseMs;
}

}   // namespace trading_test

#endif   // TRADINGAPP_TESTS_TESTWAIT_H
