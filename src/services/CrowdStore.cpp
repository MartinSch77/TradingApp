// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CrowdStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QUuid>
#include <QVariant>

namespace trading::crowd {

namespace {

// The SELECT column order, used everywhere a row is read so the indices below cannot drift.
const char *const kColumns =
    "instrument, source, source_name, series_id, event_time, received_time, "
    "value, unit, valid, quality, schema_version";
enum Col : int {
    ColInstrument = 0,
    ColSource,
    ColSourceName,
    ColSeriesId,
    ColEventTime,
    ColReceivedTime,
    ColValue,
    ColUnit,
    ColValid,
    ColQuality,
    ColSchemaVersion,
};

QString toUtcIso(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime fromUtcIso(const QString &text)
{
    // Stored with a trailing Z, so fromString reads it back as UTC; force the zone regardless so
    // a consumer never has to guess the spec.
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    parsed.setTimeZone(QTimeZone::UTC);
    return parsed;
}

Observation rowToObservation(const QSqlQuery &query)
{
    Observation obs;
    obs.instrument = query.value(ColInstrument).toString();
    obs.source = sourceFromString(query.value(ColSource).toString());
    obs.sourceName = query.value(ColSourceName).toString();
    obs.seriesId = query.value(ColSeriesId).toString();
    obs.eventTime = fromUtcIso(query.value(ColEventTime).toString());
    obs.receivedTime = fromUtcIso(query.value(ColReceivedTime).toString());
    obs.value = query.value(ColValue).toDouble();
    obs.unit = query.value(ColUnit).toString();
    obs.valid = query.value(ColValid).toInt() != 0;
    obs.quality = query.value(ColQuality).toUInt();
    obs.schemaVersion = query.value(ColSchemaVersion).toInt();
    return obs;
}

} // namespace

CrowdStore::CrowdStore(const QString &path)
    : m_connectionName(QStringLiteral("crowd-") + QUuid::createUuid().toString(QUuid::Id128))
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(path);
    if (!db.open()) {
        m_lastError = db.lastError().text();
        return;
    }
    m_open = migrate();
}

CrowdStore::~CrowdStore()
{
    // Close and drop the named connection so a re-open (or a test opening a second store) does
    // not collide. The QSqlDatabase must go out of scope before removeDatabase, hence the block.
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, /*open=*/false);
        if (db.isValid() && db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool CrowdStore::migrate()
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS observations ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " instrument TEXT NOT NULL,"
            " source TEXT NOT NULL,"
            " source_name TEXT NOT NULL,"
            " series_id TEXT NOT NULL,"
            " event_time TEXT NOT NULL,"
            " received_time TEXT NOT NULL,"
            " value REAL NOT NULL,"
            " unit TEXT NOT NULL,"
            " valid INTEGER NOT NULL,"
            " quality INTEGER NOT NULL,"
            " schema_version INTEGER NOT NULL,"
            " UNIQUE(source_name, series_id, instrument, event_time))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_obs_instrument_received"
                       " ON observations(instrument, received_time)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_obs_source ON observations(source)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_meta ("
                       " key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            return false;
        }
    }
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO schema_meta(key, value) VALUES('crowd_schema_version', ?)"));
    query.addBindValue(QString::number(kSchemaVersion));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

qint32 CrowdStore::upsert(const Observation &observation)
{
    return upsert(QList<Observation>{observation});
}

qint32 CrowdStore::upsert(const QList<Observation> &observations)
{
    if (!m_open) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    db.transaction();
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO observations"
        " (instrument, source, source_name, series_id, event_time, received_time,"
        "  value, unit, valid, quality, schema_version)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    qint32 inserted = 0;
    for (const Observation &obs : observations) {
        if (!obs.valid) {
            continue;   // never store a missing datum as a zero row
        }
        query.addBindValue(obs.instrument);
        query.addBindValue(sourceToString(obs.source));
        query.addBindValue(obs.sourceName);
        query.addBindValue(obs.seriesId);
        query.addBindValue(toUtcIso(obs.eventTime));
        query.addBindValue(toUtcIso(obs.receivedTime));
        query.addBindValue(obs.value);
        query.addBindValue(obs.unit);
        query.addBindValue(obs.valid ? 1 : 0);
        query.addBindValue(static_cast<qint64>(obs.quality));
        query.addBindValue(obs.schemaVersion);
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            continue;
        }
        // INSERT OR IGNORE affects 1 row when it inserted, 0 when the unique key already existed.
        inserted += (query.numRowsAffected() > 0) ? 1 : 0;
    }
    db.commit();
    return inserted;
}

QList<Observation> CrowdStore::observationsReceivedSince(const QString &instrument,
                                                         const QDateTime &sinceUtc) const
{
    QList<Observation> out;
    if (!m_open) {
        return out;
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    QString sql = QStringLiteral("SELECT %1 FROM observations WHERE 1=1").arg(kColumns);
    if (!instrument.isEmpty()) {
        sql += QStringLiteral(" AND instrument = ?");
    }
    if (sinceUtc.isValid()) {
        sql += QStringLiteral(" AND received_time >= ?");
    }
    sql += QStringLiteral(" ORDER BY received_time DESC");
    query.prepare(sql);
    if (!instrument.isEmpty()) {
        query.addBindValue(instrument);
    }
    if (sinceUtc.isValid()) {
        query.addBindValue(toUtcIso(sinceUtc));
    }
    if (query.exec()) {
        while (query.next()) {
            out.append(rowToObservation(query));
        }
    }
    return out;
}

Observation CrowdStore::latest(const QString &instrument, Source source,
                               const QString &seriesId) const
{
    if (!m_open) {
        return {};
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("SELECT %1 FROM observations"
                                 " WHERE instrument = ? AND source = ? AND series_id = ?"
                                 " ORDER BY received_time DESC LIMIT 1")
                      .arg(kColumns));
    query.addBindValue(instrument);
    query.addBindValue(sourceToString(source));
    query.addBindValue(seriesId);
    if (query.exec() && query.next()) {
        return rowToObservation(query);
    }
    return {};
}

qint64 CrowdStore::count() const
{
    if (!m_open) {
        return 0;
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM observations")) && query.next()) {
        return query.value(0).toLongLong();
    }
    return 0;
}

} // namespace trading::crowd
