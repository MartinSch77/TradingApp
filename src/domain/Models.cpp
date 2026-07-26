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
