// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_MONEY_H
#define TRADINGAPP_DOMAIN_MONEY_H

#include <QString>

#include <compare>
#include <cstdint>

// An amount of money, counted rather than approximated (REQ-N-008).
//
// The problem this type exists for is not theoretical. 0.1 has no exact binary
// representation, so a stake accumulated from a hundred percentage-of-equity steps
// drifts; a comparison against a per-day cap then answers "just under" for an amount
// that is over, and the machine happily sends it. Every amount that can reach a
// broker — stake, stop-loss amount, take-profit amount, fee, per-order and per-day
// cap, booked result — is therefore an INTEGER number of minor units (cents) plus the
// currency those units belong to.
//
// Three design decisions are load-bearing:
//
//  * The lossy step is NAMED and happens once. A volatility-derived stake, a
//    percentage of equity and a quote are all doubles by nature; `Money::fromDouble`
//    is where that becomes an exact amount, it states its rounding, and everything
//    after it is integer arithmetic.
//  * Currencies do not silently combine. This app displays EUR and holds a USD
//    account, and `eur + usd` is a defect the compiler cannot see today. Mixing
//    yields an INVALID amount — one that reports itself as invalid rather than
//    presenting a plausible wrong number — and an invalid amount is refused by the
//    order validator instead of being sent.
//  * Rounding is defined, not inherited: half away from zero, both on conversion and
//    on scaling, so the same inputs give the same cents on every platform.
namespace trading {

// The currencies this app actually handles. `Invalid` is the result of an operation
// that had no single currency, and it is deliberately not spellable as a valid one.
enum class Currency : quint8 { Invalid = 0, Eur, Usd };

[[nodiscard]] QString currencyCode(Currency c);
// "eur"/"EUR" -> Currency::Eur. Anything else is Invalid — this is what reads a
// broker's `orderCurrency` field, so an unknown string must not become a guess.
[[nodiscard]] Currency currencyFromCode(const QString &code);

class Money
{
public:
    // The zero amount of no particular currency is INVALID on purpose: a default
    // constructed Money must not be usable as "0 EUR", or a forgotten assignment
    // becomes a silent free trade.
    Money() = default;

    // The exact constructor: minor units (cents) of a currency.
    [[nodiscard]] static Money fromMinorUnits(qint64 minorUnits, Currency currency);
    // The one lossy conversion, in one place. Rounds half away from zero; a
    // non-finite input yields an invalid amount rather than an arbitrary one.
    [[nodiscard]] static Money fromDouble(double major, Currency currency);
    // Convenience for literals in code and tests ("12.34 EUR" as 1234 cents).
    [[nodiscard]] static Money zero(Currency currency);

    [[nodiscard]] bool isValid() const { return m_currency != Currency::Invalid; }
    [[nodiscard]] Currency currency() const { return m_currency; }
    [[nodiscard]] qint64 minorUnits() const { return m_minorUnits; }
    // For display, for the arithmetic the broker's own API speaks in doubles, and for
    // the rate maths that is genuinely fractional. Never for accumulating money.
    [[nodiscard]] double toDouble() const;
    [[nodiscard]] bool isZero() const { return isValid() && (m_minorUnits == 0); }
    [[nodiscard]] bool isPositive() const { return isValid() && (m_minorUnits > 0); }
    [[nodiscard]] bool isNegative() const { return isValid() && (m_minorUnits < 0); }

    // Exact integer arithmetic within one currency; mixed currencies (or any invalid
    // operand) give an invalid result rather than a plausible wrong one.
    [[nodiscard]] Money operator+(const Money &other) const;
    [[nodiscard]] Money operator-(const Money &other) const;
    [[nodiscard]] Money operator-() const;
    Money &operator+=(const Money &other);
    Money &operator-=(const Money &other);

    // Scaling by a dimensionless factor — a risk fraction, a leverage, a share of
    // equity. Rounds half away from zero like fromDouble, so "1% of 1 234,56" is one
    // reproducible number and not two.
    [[nodiscard]] Money scaledBy(double factor) const;
    // Exact scaling by a whole number of times (units of a position, nights held).
    [[nodiscard]] Money timesInt(qint64 factor) const;
    // What fraction of `of` this amount is; 0 when `of` is zero or the currencies do
    // not match — the callers that ask this are comparing against caps, and a
    // silently wrong ratio there is worse than a refusal.
    [[nodiscard]] double fractionOf(const Money &of) const;

    // Comparison is only meaningful within a currency. Two amounts of different
    // currencies are UNORDERED (std::partial_ordering::unordered), which makes
    // "eur < usd" false and "eur >= usd" false alike — a cap check on the wrong
    // currency therefore cannot pass by accident.
    [[nodiscard]] std::partial_ordering operator<=>(const Money &other) const;
    [[nodiscard]] bool operator==(const Money &other) const;

    // "1 234,56 EUR" — the amount with its currency, always, because an amount
    // printed without its currency is exactly the ambiguity this type removes.
    [[nodiscard]] QString toString() const;

private:
    Money(qint64 minorUnits, Currency currency)
        : m_minorUnits(minorUnits)
        , m_currency(currency)
    {
    }

    qint64 m_minorUnits = 0;
    Currency m_currency = Currency::Invalid;
};

} // namespace trading

#endif // TRADINGAPP_DOMAIN_MONEY_H
