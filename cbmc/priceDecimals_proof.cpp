// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// CBMC bounded-model-checking proof (tooling backlog item 7, optional) for
// trading::priceDecimalsCore (src/domain/PriceDecimalsCore.cpp) — the ONE function
// in this codebase pulled out specifically because it has NO Qt dependency at all,
// which is what makes it provable: CBMC ships no model for Qt's headers, and every
// other pure-algorithmic candidate considered (PositionMath's other functions,
// domain/Indicators, domain/ConfirmGate) still needs QString/QList/QHash somewhere
// in the same translation unit. This is a genuine, if narrow, proof — not a
// stand-in for the fuzzing/mutation-testing coverage the rest of this project's
// tooling backlog focuses on, which reach much more of the Qt-heavy trading logic.
//
// Property proved: for EVERY possible double bit pattern (including NaN, +-Inf,
// subnormals — the input is fully unconstrained), priceDecimalsCore returns
// exactly one of {2, 3, 4, 5}. No precondition is assumed, because none is
// needed: every comparison against a NaN magnitude is false by IEEE 754
// definition, so a NaN price falls through to the same 5 an ordinary sub-1.0
// price gets, never undefined behaviour.
//
// Run via tools/cbmc_check.sh (Linux only, ./setup.sh cbmc installs the tool).

#include "domain/PriceDecimalsCore.h"

extern "C" double nondet_double(); // NOLINT — CBMC's documented nondet-value idiom

int main()
{
    const double price = nondet_double();
    const std::int32_t decimals = trading::priceDecimalsCore(price);
    __CPROVER_assert((decimals == 2) || (decimals == 3) || (decimals == 4)
                         || (decimals == 5),
                     "priceDecimalsCore always returns one of {2, 3, 4, 5}");
    return 0;
}
