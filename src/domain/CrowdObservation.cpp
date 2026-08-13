// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/CrowdObservation.h"

#include <QHash>

namespace trading::crowd {

namespace {

// The enum <-> string table in ONE place, so the two directions cannot drift. Values are
// persisted as these strings (see the header), so they are part of the on-disk schema: change
// a spelling only with a store migration.
const QHash<Source, QString> &sourceNames()
{
    static const QHash<Source, QString> kNames = {
        {Source::RetailPositioning, QStringLiteral("retail")},
        {Source::Options, QStringLiteral("options")},
        {Source::Volatility, QStringLiteral("volatility")},
        {Source::InstitutionalPositioning, QStringLiteral("institutional")},
        {Source::Social, QStringLiteral("social")},
        {Source::Macro, QStringLiteral("macro")},
        {Source::Market, QStringLiteral("market")},
    };
    return kNames;
}

} // namespace

QString Observation::dedupKey() const
{
    // ISO-8601 in UTC with milliseconds: two datums about the same instant compare equal to the
    // millisecond regardless of the local zone they arrived in.
    return QStringLiteral("%1|%2|%3|%4")
        .arg(sourceName, seriesId, instrument,
             eventTime.toUTC().toString(Qt::ISODateWithMs));
}

qint64 Observation::ageSeconds(const QDateTime &now) const
{
    if (!receivedTime.isValid()) {
        return -1;
    }
    return receivedTime.secsTo(now);
}

Freshness Observation::freshness(const QDateTime &now, qint64 staleAfterSec) const
{
    if (!valid || !receivedTime.isValid()) {
        return Freshness::Absent;
    }
    const qint64 age = ageSeconds(now);
    // A future timestamp (age < 0) is treated as Live rather than Stale: a small clock skew is
    // not "old data", and clamping it to Live is the safe reading.
    return (age <= staleAfterSec) ? Freshness::Live : Freshness::Stale;
}

bool Observation::hasQuality(Quality flag) const
{
    if (flag == Quality::Ok) {
        return quality == std::to_underlying(Quality::Ok);
    }
    return (quality & std::to_underlying(flag)) != 0U;
}

void Observation::setQuality(Quality flag)
{
    quality |= std::to_underlying(flag);
}

QString sourceToString(Source source)
{
    return sourceNames().value(source, QStringLiteral("market"));
}

Source sourceFromString(const QString &name)
{
    for (auto it = sourceNames().cbegin(); it != sourceNames().cend(); ++it) {
        if (it.value() == name) {
            return it.key();
        }
    }
    return Source::Market;
}

QString freshnessWord(Freshness freshness)
{
    switch (freshness) {
    case Freshness::Live:
        return QStringLiteral("live");
    case Freshness::Stale:
        return QStringLiteral("stale");
    case Freshness::Absent:
        break;
    }
    return QStringLiteral("absent");
}

} // namespace trading::crowd
