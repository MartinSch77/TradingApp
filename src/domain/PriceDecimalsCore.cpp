// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/PriceDecimalsCore.h"

namespace trading {

std::int32_t priceDecimalsCore(double price) noexcept
{
    // A manual absolute value rather than std::abs/<cmath>: this file is kept
    // dependency-free on purpose (see the header) so CBMC can parse it — <cmath>
    // drags in glibc's extended-float declarations (_Float32/_Float64), which
    // CBMC 5.95's C++ front-end cannot parse at all. Behaviourally identical to
    // std::abs for every double this function cares about, NaN included (a NaN
    // comparison is always false, so `a` stays NaN either way and every branch
    // below falls through to the same 5 a std::abs(NaN) path would reach).
    const double a = (price < 0.0) ? -price : price;
    if (a >= 100.0) {
        return 2;
    }
    if (a >= 10.0) {
        return 3;
    }
    if (a >= 1.0) {
        return 4;
    }
    return 5;
}

} // namespace trading
