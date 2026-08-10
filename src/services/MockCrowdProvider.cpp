// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/MockCrowdProvider.h"

#include <QRandomGenerator>
#include <QTimeZone>

#include <numeric>

namespace trading::crowd {

namespace {

// A DETERMINISTIC seed from the instrument and the UTC day, computed without qHash — Qt6's
// qHash mixes a per-process random seed, which would make the "mock" different on every run.
// This little rolling hash depends only on its inputs, so the same day yields the same data
// (reproducibility the tests rely on). It is a stand-in for real data, not a security PRNG —
// the deterministic-PRNG-for-reproducibility choice SonarCloud flags and this repo accepts.
quint32 seedFor(const QString &instrument, const QDate &day)
{
    return std::accumulate(instrument.cbegin(), instrument.cend(),
                           static_cast<quint32>(day.toJulianDay()),
                           [](quint32 seed, QChar ch) { return (seed * 31U) + ch.unicode(); });
}

} // namespace

MockCrowdProvider::MockCrowdProvider(bool configured) : m_configured(configured) {}

QString MockCrowdProvider::name() const
{
    return QStringLiteral("mock");
}

Source MockCrowdProvider::category() const
{
    return Source::Market;   // it spans families; labelled Market as the catch-all
}

bool MockCrowdProvider::isConfigured() const
{
    return m_configured;
}

ProviderResult MockCrowdProvider::fetch(const QString &instrument, const QDateTime &now)
{
    ProviderResult result;
    if (!m_configured) {
        // The recoverable no-credentials state a real provider has — absent, not an error.
        result.available = false;
        result.note = QStringLiteral("mock provider is switched off (no data)");
        return result;
    }

    const QDateTime nowUtc = now.toUTC();
    QRandomGenerator gen(seedFor(instrument, nowUtc.date()));
    const auto uniform = [&gen](double lo, double hi) {
        return lo + (gen.generateDouble() * (hi - lo));
    };
    const auto make = [&instrument](Source src, const QString &series, double value,
                                    const QString &unit, const QDateTime &eventT,
                                    const QDateTime &receivedT) {
        Observation obs;
        obs.instrument = instrument;
        obs.source = src;
        obs.sourceName = QStringLiteral("mock");
        obs.seriesId = series;
        obs.eventTime = eventT.toUTC();
        obs.receivedTime = receivedT.toUTC();
        obs.value = value;
        obs.unit = unit;
        obs.valid = true;
        return obs;
    };

    // Live families: event == received == now.
    result.observations.append(make(Source::Volatility, QStringLiteral("VIX"),
                                    uniform(13.0, 24.0), QStringLiteral("index"), nowUtc, nowUtc));
    result.observations.append(make(Source::RetailPositioning, QStringLiteral("IG-PCT-LONG"),
                                    uniform(35.0, 70.0), QStringLiteral("percent"), nowUtc,
                                    nowUtc));
    result.observations.append(make(Source::Options, QStringLiteral("PUT-CALL"),
                                    uniform(0.7, 1.3), QStringLiteral("ratio"), nowUtc, nowUtc));
    result.observations.append(make(Source::Social, QStringLiteral("NET-SENTIMENT"),
                                    uniform(-0.4, 0.4), QStringLiteral("ratio"), nowUtc, nowUtc));

    // Institutional COT with the REAL publication lag: the datum is ABOUT the most recent Tuesday
    // but became KNOWN the following Friday (~CFTC 3:30pm ET release). receivedTime is therefore
    // ~3 days after eventTime, which is exactly what a leakage-safe consumer must reason in.
    const QDate today = nowUtc.date();
    const int daysSinceFriday = (today.dayOfWeek() - static_cast<int>(Qt::Friday) + 7) % 7;
    const QDate pubFriday = today.addDays(-daysSinceFriday);
    const QDateTime received(pubFriday, QTime(20, 30), QTimeZone::UTC);
    const QDateTime eventTuesday(pubFriday.addDays(-3), QTime(20, 0), QTimeZone::UTC);
    result.observations.append(make(Source::InstitutionalPositioning,
                                    QStringLiteral("COT-ASSET-MGR-NET"), uniform(-50000.0, 50000.0),
                                    QStringLiteral("contracts"), eventTuesday, received));
    return result;
}

} // namespace trading::crowd
