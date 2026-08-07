// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/Money.h"

#include <QLocale>

#include <cmath>
#include <limits>

namespace trading {

namespace {

// One minor unit per hundredth of a major unit: both currencies here are 2-decimal.
// A currency with a different exponent (JPY) would need this to become a per-currency
// value, which is why it is a named constant rather than a scattered 100.
constexpr double kMinorUnitsPerMajor = 100.0;

// Rounding, defined once: half away from zero. std::round already does exactly that
// for both signs, which is why it is used rather than a hand-rolled comparison — but
// the intent is stated here because "round half away from zero" is a requirement
// (REQ-N-008) rather than an implementation detail.
[[nodiscard]] bool minorUnitsFor(double major, qint64 &out)
{
    if (!std::isfinite(major)) {
        return false;
    }
    const double scaled = std::round(major * kMinorUnitsPerMajor);
    // A double beyond the int64 range would be undefined behaviour to convert. An
    // amount that large is a defect upstream, so it becomes an invalid amount.
    constexpr double kLimit = 9.0e18;
    if (std::fabs(scaled) > kLimit) {
        return false;
    }
    out = static_cast<qint64>(scaled);
    return true;
}

} // namespace

QString currencyCode(Currency c)
{
    switch (c) {
    case Currency::Eur:
        return QStringLiteral("EUR");
    case Currency::Usd:
        return QStringLiteral("USD");
    case Currency::Invalid:
        break;
    }
    return QStringLiteral("---");
}

Currency currencyFromCode(const QString &code)
{
    const QString upper = code.trimmed().toUpper();
    if (upper == QStringLiteral("EUR")) {
        return Currency::Eur;
    }
    if (upper == QStringLiteral("USD")) {
        return Currency::Usd;
    }
    return Currency::Invalid;
}

Money Money::fromMinorUnits(qint64 minorUnits, Currency currency)
{
    if (currency == Currency::Invalid) {
        return {};
    }
    return {minorUnits, currency};
}

Money Money::fromDouble(double major, Currency currency)
{
    qint64 units = 0;
    if ((currency == Currency::Invalid) || !minorUnitsFor(major, units)) {
        return {};
    }
    return {units, currency};
}

Money Money::zero(Currency currency)
{
    return fromMinorUnits(0, currency);
}

double Money::toDouble() const
{
    if (!isValid()) {
        return 0.0;
    }
    return static_cast<double>(m_minorUnits) / kMinorUnitsPerMajor;
}

Money Money::operator+(const Money &other) const
{
    if (!isValid() || (m_currency != other.m_currency)) {
        return {};
    }
    return {m_minorUnits + other.m_minorUnits, m_currency};
}

Money Money::operator-(const Money &other) const
{
    if (!isValid() || (m_currency != other.m_currency)) {
        return {};
    }
    return {m_minorUnits - other.m_minorUnits, m_currency};
}

Money Money::operator-() const
{
    if (!isValid()) {
        return {};
    }
    return {-m_minorUnits, m_currency};
}

Money &Money::operator+=(const Money &other)
{
    *this = *this + other;
    return *this;
}

Money &Money::operator-=(const Money &other)
{
    *this = *this - other;
    return *this;
}

Money Money::scaledBy(double factor) const
{
    if (!isValid() || !std::isfinite(factor)) {
        return {};
    }
    qint64 units = 0;
    if (!minorUnitsFor((static_cast<double>(m_minorUnits) * factor) / kMinorUnitsPerMajor,
                       units)) {
        return {};
    }
    return {units, m_currency};
}

Money Money::timesInt(qint64 factor) const
{
    if (!isValid()) {
        return {};
    }
    return {m_minorUnits * factor, m_currency};
}

double Money::fractionOf(const Money &of) const
{
    if (!isValid() || (m_currency != of.m_currency) || (of.m_minorUnits == 0)) {
        return 0.0;
    }
    return static_cast<double>(m_minorUnits) / static_cast<double>(of.m_minorUnits);
}

std::partial_ordering Money::operator<=>(const Money &other) const
{
    if (!isValid() || (m_currency != other.m_currency)) {
        return std::partial_ordering::unordered;
    }
    if (m_minorUnits < other.m_minorUnits) {
        return std::partial_ordering::less;
    }
    if (m_minorUnits > other.m_minorUnits) {
        return std::partial_ordering::greater;
    }
    return std::partial_ordering::equivalent;
}

bool Money::operator==(const Money &other) const
{
    return (m_currency == other.m_currency) && (m_minorUnits == other.m_minorUnits);
}

QString Money::toString() const
{
    if (!isValid()) {
        return QStringLiteral("invalid amount");
    }
    // The locale's own grouping and decimal separator, because this string is shown to
    // a user; the value behind it stays the exact integer either way.
    return QStringLiteral("%1 %2")
        .arg(QLocale().toString(toDouble(), 'f', 2), currencyCode(m_currency));
}

} // namespace trading
