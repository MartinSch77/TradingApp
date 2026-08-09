// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_CROWDSTORE_H
#define TRADINGAPP_SERVICES_CROWDSTORE_H

#include "domain/CrowdObservation.h"

#include <QDateTime>
#include <QList>
#include <QString>

// SQLite persistence for the Crowd Sentiment subsystem (REQ-F-039, Phase 1). It stores the RAW
// normalized observation layer ONLY. Features, labels, predictions and realized outcomes are
// deliberately SEPARATE tables, each added in the phase that needs it — never crammed into one
// wide table, which is what makes leakage and errors detectable later.
//
// Discipline this class enforces:
//   * every timestamp is an ISO-8601 UTC string on disk, parsed back to a UTC QDateTime;
//   * (source_name, series_id, instrument, event_time) is UNIQUE, so re-fetching the same datum
//     is an idempotent no-op (INSERT OR IGNORE) rather than a duplicate row;
//   * an open/SQL failure is reported through isOpen()/lastError(), never a crash — a missing
//     database is a recoverable condition, the same rule the providers follow.
//
// Pass ":memory:" as the path for a test. This is a SERVICES-layer class (it owns the QtSql
// connection); the domain Observation it stores stays free of any storage concern.
namespace trading::crowd {

class CrowdStore
{
public:
    // The schema version this build writes/expects. Bumped only alongside a migration.
    static constexpr qint32 kSchemaVersion = 1;

    explicit CrowdStore(const QString &path);
    ~CrowdStore();
    CrowdStore(const CrowdStore &) = delete;
    CrowdStore &operator=(const CrowdStore &) = delete;
    CrowdStore(CrowdStore &&) = delete;
    CrowdStore &operator=(CrowdStore &&) = delete;

    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    // Insert observations, IGNORING any whose dedup key already exists. Returns the number of
    // NEW rows actually written (duplicates are left untouched and counted out). An invalid
    // observation is skipped rather than stored as a zero.
    qint32 upsert(const QList<Observation> &observations);
    qint32 upsert(const Observation &observation);

    // Observations for `instrument` (empty = any instrument) whose receivedTime is at or after
    // `sinceUtc`, NEWEST received first. An invalid `sinceUtc` means "no lower bound". Ordering
    // by receivedTime — not eventTime — is deliberate: it answers "what did we KNOW by then".
    [[nodiscard]] QList<Observation> observationsReceivedSince(const QString &instrument,
                                                               const QDateTime &sinceUtc) const;
    // The single newest-by-receivedTime observation of a series, or an invalid Observation when
    // there is none.
    [[nodiscard]] Observation latest(const QString &instrument, Source source,
                                     const QString &seriesId) const;
    [[nodiscard]] qint64 count() const;

private:
    bool migrate();

    QString m_connectionName;
    bool m_open = false;
    QString m_lastError;
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_CROWDSTORE_H
