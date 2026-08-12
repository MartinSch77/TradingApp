// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// libFuzzer harness for trading::picksFrom (REQ-F-030, src/domain/OllamaResponseParser.cpp).
// The function reads a local LLM's raw free-text answer and defensively coerces it into
// trade picks — prose or ```json fences around the payload, a truncated generation missing
// its closing braces, a symbol-keyed map instead of the requested array. Every shape here
// has been observed from a real model (qwen2.5:1.5b) and is pinned by TS-OLLAMA-007: a
// mis-parse must be a silent no-trade, never a crash.

#include "domain/OllamaResponseParser.h"

#include <QString>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size)
{
    const QString text =
        QString::fromUtf8(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    const QList<AiDecision> picks = trading::picksFrom(text);
    // Every accepted pick must carry a non-empty symbol — the one invariant every
    // caller of picksFrom trusts without re-checking (PaperTrader's matchProposalSymbol
    // is the very next step). A pick with an empty symbol would be a defect in the
    // shape dispatcher, not a fact about the model's answer.
    for (const AiDecision &pick : picks) {
        if (pick.symbol.isEmpty()) {
            __builtin_trap();
        }
    }
    return 0;
}
