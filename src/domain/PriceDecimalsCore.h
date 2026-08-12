// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_PRICEDECIMALSCORE_H
#define TRADINGAPP_DOMAIN_PRICEDECIMALSCORE_H

#include <cstdint>

// CBMC 5.95's C++ front-end cannot parse the [[attribute]] bracket syntax AT ALL,
// in any --cppNN mode (measured: even a bare `[[nodiscard]] int foo();` is a parse
// error) — tools/cbmc_check.sh compiles this header with -DTRADINGAPP_CBMC_PROOF to
// strip it, since this is the one header CBMC needs to parse at all. Every normal
// build leaves the attribute in place.
#ifdef TRADINGAPP_CBMC_PROOF
#define TRADINGAPP_NODISCARD
#else
#define TRADINGAPP_NODISCARD [[nodiscard]]
#endif

// The pure magnitude-bucketing algorithm behind PositionMath::priceDecimals,
// pulled into its own header with NO Qt dependency (not even qint32) so it can be
// verified with CBMC (tooling backlog item 7, optional): CBMC has no model for
// Qt's headers, and PositionMath.h/.cpp pull in QString/QHash/QStringList for
// their OTHER functions, which would make the whole translation unit unprovable.
// PositionMath::priceDecimals is a one-line wrapper around this function — there
// is exactly one authoritative definition of the algorithm, just relocated.
namespace trading {

// Sensible number of decimals for a price, by magnitude: indices in the
// thousands need 2, while low-priced instruments (e.g. forex ~1.08) need more.
// Total over every double, including NaN/Inf/subnormals (see cbmc/priceDecimals_
// proof.cpp): every comparison against a NaN magnitude is false, so it falls
// through to the same 5 an ordinary sub-1.0 price gets — never undefined
// behaviour, and always one of {2, 3, 4, 5}.
TRADINGAPP_NODISCARD std::int32_t priceDecimalsCore(double price) noexcept;

} // namespace trading

#undef TRADINGAPP_NODISCARD

#endif // TRADINGAPP_DOMAIN_PRICEDECIMALSCORE_H
