#include "domain/Models.h"

// Out-of-line member definitions. These used to live inline in Models.h, but
// header-inline (comdat) functions are compiled into several translation
// units, and the coverage instrumentation can emit records with divergent
// structural hashes for them (e.g. plain TU vs. automoc TU). The linker keeps
// one copy of the function, the stale coverage records survive, and llvm-cov
// reports "functions have mismatched data". Defining them in exactly one TU
// keeps the coverage data unambiguous.

bool InstrumentFees::isValid() const
{
    return (buyOvernight != 0.0) || (sellOvernight != 0.0) || (buyWeekend != 0.0)
           || (sellWeekend != 0.0);
}

bool Quote::isValid() const
{
    return bid > 0.0;
}

double Quote::closeRate(bool isBuy) const
{
    // A long is closed by SELLING (at the bid), a short by BUYING (at the ask) —
    // the same side eToro marks its own unrealised P/L at. Fall back to whichever
    // side is known when the row carried only one.
    const double want = isBuy ? bid : ask;
    return (want > 0.0) ? want : (isBuy ? ask : bid);
}

double Quote::conversion(bool isBuy) const
{
    const double want = isBuy ? conversionBid : conversionAsk;
    return (want > 0.0) ? want : 1.0;
}

double Quote::spread() const
{
    return ((bid > 0.0) && (ask > bid)) ? (ask - bid) : 0.0;
}

qint64 Quote::ageMs(const QDateTime &now) const
{
    return asOf.isValid() ? asOf.msecsTo(now) : -1;
}
