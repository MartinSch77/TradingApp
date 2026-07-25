#ifndef TRADINGAPP_ECONOMICCALENDAR_H
#define TRADINGAPP_ECONOMICCALENDAR_H

#include "domain/Models.h"

#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QTimer;

// Fetches upcoming macro events relevant to the selected instrument (rate decisions,
// CPI, employment, GDP, …) from a free, no-key economic-calendar feed, scoped to the
// regions whose data tends to move that instrument, and re-publishes the filtered,
// upcoming, time-sorted list. Call setInstrument() to re-scope it on an instrument
// switch; unknown instruments fall back to the US (the dominant global macro driver).
class EconomicCalendar : public QObject
{
    Q_OBJECT
public:
    explicit EconomicCalendar(QObject *parent = nullptr);

    void start();  // fetch now, then refresh periodically

    // Re-scope the calendar to the macro regions relevant to `symbol`; refreshes
    // immediately if the region set changed and start() has already run.
    void setInstrument(const QString &symbol);

signals:
    void eventsUpdated(const QList<EconomicEvent> &events);
    void log(const QString &message, bool isError);

private:
    void refresh();
    // Arm a one-shot refresh for the moment the soonest upcoming event is due, so
    // the list (and the "next event" it feeds) rolls over as each event passes.
    void scheduleNextEventRefresh(const QList<EconomicEvent> &events);

    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;          // periodic full refresh
    QTimer *m_nextEventTimer = nullptr;  // one-shot, fires when the next event is due
    QString m_countries = QStringLiteral("US");  // calendar regions for the current instrument
    bool m_started = false;              // start() has run — setInstrument may refresh now
};

#endif // TRADINGAPP_ECONOMICCALENDAR_H
