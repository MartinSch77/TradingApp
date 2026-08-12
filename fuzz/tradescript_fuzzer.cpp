// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// libFuzzer harness for trading::parseTradeScript (REQ-F-028). The function reads a
// hand-edited text file straight from disk with no other validation, so it is exactly
// the "untrusted external text" case this project's own tooling priorities call out.
// QString::fromUtf8 never throws/crashes on ill-formed UTF-8 (invalid sequences become
// U+FFFD) — the point of this harness is finding a crash, hang or sanitizer report
// INSIDE the parser itself, not testing UTF-8 decoding.

#include "domain/TradeScript.h"

#include <QString>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size)
{
    const QString text =
        QString::fromUtf8(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    const trading::ScriptParseResult result = trading::parseTradeScript(text);
    // Every entry parseTradeScript accepts must carry the loop-bound invariants the
    // rest of the domain trusts without re-checking (amount required, snapped leverage
    // range) — asserting them here turns "the parser accepted something it shouldn't"
    // into a sanitizer-visible failure instead of a silent downstream bug.
    if (result.ok) {
        for (const trading::ScriptEntry &entry : result.entries) {
            if (entry.amount <= 0.0) {
                __builtin_trap();
            }
            if (entry.leverage < 1) {
                __builtin_trap();
            }
        }
    }
    return 0;
}
