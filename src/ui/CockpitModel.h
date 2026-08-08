// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_COCKPITMODEL_H
#define TRADINGAPP_UI_COCKPITMODEL_H

#include "domain/Candles.h"
#include "domain/ConfirmGate.h"
#include "domain/IndexConfluence.h"
#include "domain/Models.h"
#include "domain/LeadSignal.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

class QTimer;

// What the declarative cockpit shows, prepared in C++ so it can be TESTED (REQ-F-038).
//
// The QML is deliberately dumb: it binds to the properties below and computes nothing. That
// is not a style preference — a view makes claims about the system, and a screenshot proves
// only that something was drawn, never that it was right. Everything here is reachable from
// a unit test with no rendering, no window and no GPU.
//
// The free functions come first and carry the actual rules. They are free rather than
// members because each one is a pure question with a pure answer, and a test should not have
// to build a QObject to ask it.
//
// THREE RULES THIS FILE EXISTS TO ENFORCE, each inherited from a requirement the cockpit is
// not allowed to soften:
//
//  1. An unmeasurable read is UNMEASURABLE (REQ-F-035). Not 0, not neutral. A "6 of 9" that
//     silently includes three absent feeds is a lie, so the count of unknowns travels
//     beside the count of agreements everywhere.
//  2. A 0..100 strength is EVIDENCE, never a probability (REQ-F-037). Where no measured
//     probability exists the answer is UNCALIBRATED with its sample count and NO number.
//  3. Every feed-derived value carries its FRESHNESS. A stale price rendered like a live one
//     misleads silently and continuously, which is worse than a gap — this project measured
//     the eToro rates row running 6-12 minutes behind while the candle close was exact.
namespace trading::ui {

// How current a number is. Absent is its own state and must never collapse into a value:
// the last known reading, styled as current, is the failure this enum prevents.
enum class Freshness : quint8 { Live, Lagging, Absent };

// Live below staleMs, Lagging beyond it, Absent when the age is unknown (< 0) — the same
// convention EtoroClient uses for a quote that never arrived.
[[nodiscard]] Freshness cockpitFreshness(qint64 ageMs, qint64 staleMs);

// "live", "6m old", "—". The age is spelled out rather than implied by colour, so the state
// survives a screenshot, a colour-blind reader and a black-and-white print.
[[nodiscard]] QString freshnessLabel(Freshness state, qint64 ageMs);

// One read of the nine, as a GLYPH first and a colour second.
//
// The glyph is load-bearing: the confluence meter's three states are agree / disagree /
// unmeasurable, and encoding them by colour alone fails exactly the readers the domain's
// green-red convention already fails. Measured with the palette validator: the status
// good and critical steps sit at CVD dE 4.1 under deuteranopia — indistinguishable.
struct ReadTick {
    QString glyph;    // "●" agrees · "○" disagrees · "✕" unmeasurable
    QString label;    // the read's name
    QString state;    // "agrees" | "disagrees" | "unmeasurable" — text, never colour alone
    QString detail;   // the number behind it
};

// The nine reads as ticks, judged against `dir` (the side the signal is for). A read that
// could not be measured is `unmeasurable` whatever its dir field happens to hold.
[[nodiscard]] QList<ReadTick> cockpitTicks(const trading::IndexReads &reads, qint32 dir);

// "6 of 9 agree · 3 unmeasurable" — both facts, always. Never just the fraction.
[[nodiscard]] QString cockpitAgreementText(const QList<ReadTick> &ticks);

// The probability line, and the one place the UNCALIBRATED discipline is enforced for the
// view: below `minSamples` resolved samples there is NO number, only the count and what is
// still needed. Above it, the MEASURED hit rate — which is a frequency, so it may be a
// percentage.
[[nodiscard]] QString cockpitProbabilityText(qint32 samples, qint32 minSamples,
                                             double hitRate);

// The evidence line. Says "evidence", never "confidence" and never a percent sign, because
// the number is a weighted sum of indicators rather than a claim about frequency.
[[nodiscard]] QString cockpitEvidenceText(const trading::LeadSignal &signal);

// One market card. `changePct` is only meaningful when freshness is not Absent.
struct CockpitCard {
    QString symbol;
    double price = 0.0;
    double changePct = 0.0;
    Freshness freshness = Freshness::Absent;
    qint64 ageMs = -1;
};

// The card as QML sees it. Freshness crosses as its own field AND as a label, so a binding
// cannot accidentally render Absent as 0.00.
[[nodiscard]] QVariantMap cardToVariant(const CockpitCard &card);

// One market card built from a reference series, and the ONE place that judgement is made.
//
// Both front ends call this rather than each computing "last versus first, unless the series
// is too short". Two copies of a rule are two answers waiting to diverge, and the answer here
// decides whether a card shows a number or an em dash — the difference between reporting a
// market and reporting a missing feed.
[[nodiscard]] CockpitCard cardFromSeries(const QString &label, const QList<double> &series);

// The four references a trader watches beside the instrument, in a fixed order. Fixed order
// because a card that moves between refreshes is unreadable.
//
// TWO BOOKS, AND THEY ARE KEYED DIFFERENTLY — the same trap ReadInputs exists to make
// unrepresentable, and this function fell straight into it. VIX and the 10-year are Yahoo
// TICKERS (^VIX, ^TNX) and live in `byTicker`. The index futures are this application's own
// SYMBOLS (SP.24-7, NSDQ100.24-7) and live in `bySymbol`. Looking the futures up in the
// ticker book — which is what this did — misses every time, so the SPX500 and NSDQ100 cards
// showed an em dash permanently in BOTH front ends while the feed was working perfectly.
// Two named parameters instead of one, so the mistake is visible at the call site.
[[nodiscard]] QList<CockpitCard> referenceCards(
    const QHash<QString, QList<double>> &byTicker,
    const QHash<QString, QList<double>> &bySymbol);

// One candle as Qt Graphs' CustomSeries sees it — a QVariantMap the delegate reads by name.
//
// `up` is computed HERE rather than in the delegate as `close >= open`. The QML would get
// the same answer today, but the tie rule (flat bar counts as up) is a decision this project
// made once and pinned with a test; recomputing it in a binding is how two answers to one
// question start to exist. The series itself adds `index`.
[[nodiscard]] QVariantMap candleToVariant(const trading::Candle &candle);

// One open position as QML sees it. Every number is formatted HERE: a binding that formats
// is a binding that can format differently from the Widgets table showing the same trade.
[[nodiscard]] QVariantMap positionToVariant(const Position &position, double markPrice);

// The trade ticket's state as QML sees it (REQ-F-038, REQ-N-005).
//
// The QML owns NONE of this. A declarative surface is the wrong place for a money-moving
// state machine: a binding that re-evaluates at the wrong moment would arm or disarm the
// gate, and no test could see it happen. So the ticket is a C++ object, the QML calls one
// method on it, and every rule below is checked headless.
struct TicketState {
    QString prompt;        // what the user must do next; empty when nothing is armed
    QString blocked;       // why trading is impossible at all; empty when it is possible
    bool armed = false;
};

// Whether an order may even be attempted, and the sentence saying why not.
//
// Order matters: the most fundamental obstacle is reported first, because a reader given
// three reasons fixes the wrong one. No credentials outranks a closed market, which
// outranks an unusable amount.
[[nodiscard]] QString ticketBlockedReason(bool hasCredentials, bool marketOpen, double amount,
                                          double minAmount, double maxAmount);

// The last-N-weeks closed-trade summary the user asked to see in the cockpit — the same
// question the Widgets closed-trades window answers, as one line: net kept, trade count, win
// rate and the fees paid to get it. Pure and tested; `nowMs`/`weeks` bound the window so a
// year-old book does not inflate a "last 13 weeks" figure.
[[nodiscard]] QString closedHistoryText(const QList<ClosedTrade> &closed, qint64 nowMs,
                                        qint32 weeks);

// The recent closed trades as rows for the cockpit, newest first, within the same window.
// Net carries its sign; an absent close time drops the row rather than dating it to the epoch.
[[nodiscard]] QVariantList closedHistoryRows(const QList<ClosedTrade> &closed, qint64 nowMs,
                                             qint32 weeks, qint32 limit);

// The UPCOMING economic events, soonest first — the releases that move the indices the bot
// watches. Past events are dropped (the calendar is about what is ahead), and impact travels
// as text so a High-impact release is not distinguished by colour alone.
[[nodiscard]] QVariantList calendarRows(const QList<EconomicEvent> &events, qint64 nowMs,
                                        qint32 limit);

// The view-model the QML binds to. Assembly only — every rule lives in the functions above.
class CockpitModel : public QObject
{
    Q_OBJECT;
    Q_PROPERTY(QVariantList cards READ cards NOTIFY changed)
    Q_PROPERTY(QVariantList ticks READ ticks NOTIFY changed)
    Q_PROPERTY(QString agreement READ agreement NOTIFY changed)
    Q_PROPERTY(QString evidence READ evidence NOTIFY changed)
    Q_PROPERTY(QString probability READ probability NOTIFY changed)
    Q_PROPERTY(QString instrument READ instrument NOTIFY changed)
    Q_PROPERTY(QString openingHours READ openingHours NOTIFY changed)
    Q_PROPERTY(bool simulation READ simulation NOTIFY changed)
    // The price chart. `candleNote` is non-empty exactly when there is nothing to draw, so
    // the QML has one thing to test and cannot render an empty axis as a quiet market.
    Q_PROPERTY(QVariantList candles READ candles NOTIFY changed)
    Q_PROPERTY(double candleMin READ candleMin NOTIFY changed)
    Q_PROPERTY(double candleMax READ candleMax NOTIFY changed)
    Q_PROPERTY(QString candleNote READ candleNote NOTIFY changed)
    Q_PROPERTY(QString candleSpan READ candleSpan NOTIFY changed)
    Q_PROPERTY(QString ticketPrompt READ ticketPrompt NOTIFY changed)
    Q_PROPERTY(QString ticketBlocked READ ticketBlocked NOTIFY changed)
    Q_PROPERTY(bool ticketArmed READ ticketArmed NOTIFY changed)
    Q_PROPERTY(double amount READ amount NOTIFY changed)
    Q_PROPERTY(int leverage READ leverage NOTIFY changed)
    Q_PROPERTY(QVariantList positions READ positions NOTIFY changed)
    Q_PROPERTY(QStringList instruments READ instruments NOTIFY changed)
    Q_PROPERTY(QString closedHistory READ closedHistory NOTIFY changed)
    Q_PROPERTY(QVariantList closedRows READ closedRows NOTIFY changed)
    Q_PROPERTY(QVariantList events READ events NOTIFY changed)
    Q_PROPERTY(double stopLoss READ stopLoss NOTIFY changed)
    Q_PROPERTY(double takeProfit READ takeProfit NOTIFY changed)

public:
    explicit CockpitModel(QObject *parent = nullptr);

    void setCards(const QList<CockpitCard> &cards);
    // The bars to draw. Already filtered by `candlesFrom` — this only prepares them for the
    // delegate and fits the axis to the WICKS.
    void setCandles(const QList<trading::Candle> &candles);
    void setSignal(const QString &instrument, const trading::LeadSignal &signal,
                   const trading::IndexReads &reads);
    void setCalibration(qint32 samples, qint32 minSamples, double hitRate);
    // SIMULATION is not decoration: the banner is how a reader knows no order can reach an
    // account, so the flag is part of the model rather than a QML constant.
    void setSimulation(bool on);

    [[nodiscard]] QVariantList cards() const { return m_cards; }
    [[nodiscard]] QVariantList ticks() const { return m_ticks; }
    [[nodiscard]] QString agreement() const { return m_agreement; }
    [[nodiscard]] QString evidence() const { return m_evidence; }
    [[nodiscard]] QString probability() const { return m_probability; }
    [[nodiscard]] QString instrument() const { return m_instrument; }
    // The selected instrument's exchange opening hours, as a display line (crypto = 24/7).
    [[nodiscard]] QString openingHours() const { return m_openingHours; }
    [[nodiscard]] bool simulation() const { return m_simulation; }
    [[nodiscard]] QVariantList candles() const { return m_candles; }
    [[nodiscard]] double candleMin() const { return m_candleMin; }
    [[nodiscard]] double candleMax() const { return m_candleMax; }
    [[nodiscard]] QString candleNote() const { return m_candleNote; }
    [[nodiscard]] QString candleSpan() const { return m_candleSpan; }

    // --- the trade ticket (REQ-F-038, REQ-N-005) ---------------------------------------
    // Everything the account imposes on an order, pushed in by the host. The model never
    // fetches: it is a view-model, and a view-model that reaches for data is a second,
    // untested copy of the service layer.
    void setTradeContext(bool hasCredentials, bool marketOpen, double minAmount,
                         double maxAmount);
    // The whole ticket in ONE call, SL/TP included. One setter rather than five, because
    // every field is part of the armed action: a call that changed only the stop would have
    // to disarm too, and five setters is five chances to forget that.
    void setTicket(double amount, qint32 leverage, double stopLoss = 0.0,
                   double takeProfit = 0.0);
    // The instruments that can be selected, and which one is shown.
    void setInstruments(const QStringList &symbols);
    // The closed-trade history over `weeks` (the user's 13-week view) and the economic
    // calendar. Both read-only evidence, both fed by the host from the services it already
    // drives.
    void setClosedHistory(const QList<ClosedTrade> &closed, qint32 weeks);
    void setEvents(const QList<EconomicEvent> &events);
    // The open book. Already filtered by the host — a position the broker has confirmed
    // closed must not be here, for the same reason it leaves the Widgets table at once.
    void setPositions(const QList<Position> &positions, double markPrice);

    [[nodiscard]] QString ticketPrompt() const { return m_ticketPrompt; }
    [[nodiscard]] QString ticketBlocked() const { return m_ticketBlocked; }
    [[nodiscard]] bool ticketArmed() const { return !m_gate.action.isEmpty(); }
    [[nodiscard]] double amount() const { return m_amount; }
    [[nodiscard]] qint32 leverage() const { return m_leverage; }
    [[nodiscard]] QVariantList positions() const { return m_positions; }
    [[nodiscard]] QStringList instruments() const { return m_instruments; }
    [[nodiscard]] double stopLoss() const { return m_stopLoss; }
    [[nodiscard]] double takeProfit() const { return m_takeProfit; }
    [[nodiscard]] QString closedHistory() const { return m_closedHistory; }
    [[nodiscard]] QVariantList closedRows() const { return m_closedRows; }
    [[nodiscard]] QVariantList events() const { return m_events; }

    // Switching instrument DISARMS, for the same reason editing the amount does: the order
    // that was confirmed is not the order that would now be sent.
    Q_INVOKABLE void selectInstrument(const QString &symbol);

    // ONE press of buy or sell. Returns nothing and places nothing by itself: on a valid
    // second press within the window it emits placeRequested, and the HOST sends the order.
    // The model deliberately has no broker of its own — the thing that can move money stays
    // in the composition root, where it is one object and one place to audit.
    Q_INVOKABLE void press(bool buy);
    // Abandon whatever is armed. Bound to Escape and to leaving the ticket.
    Q_INVOKABLE void cancelArm();
    // The same gate in front of closing a position, keyed by its id so confirming a close
    // of one trade can never close another.
    Q_INVOKABLE void pressClose(const QString &positionId);

Q_SIGNALS:
    void changed();
    // A human confirmed this, twice, just now. The host still validates it
    // (OrderRequestValidator, REQ-N-009) before anything is sent — this signal means
    // "authorised", never "correct".
    void placeRequested(bool buy, double amount, qint32 leverage, double stopLoss,
                        double takeProfit);
    // The user picked a different instrument; the host refetches and re-pushes.
    void instrumentRequested(const QString &symbol);
    void closeRequested(const QString &positionId);

private:
    // Starts/stops the arm-expiry timer to match the gate: running while armed, stopped once
    // the gate commits, cancels or clears. Kept in one place so press and pressClose agree.
    void syncArmTimer();

    QVariantList m_cards;
    QVariantList m_ticks;
    QString m_agreement;
    QString m_evidence;
    QString m_probability;
    QString m_instrument;
    QString m_openingHours;
    bool m_simulation = true;   // safe default: claim simulation until told otherwise
    QVariantList m_candles;
    double m_candleMin = 0.0;
    double m_candleMax = 0.0;
    // Non-empty until real bars arrive, so a cockpit opened before the first fetch says so
    // rather than presenting an empty frame.
    QString m_candleNote;
    QString m_candleSpan;

    // The REQ-N-005 gate, shared with the Widgets window through trading::confirmPress.
    trading::ConfirmGate m_gate;
    QString m_ticketPrompt;
    // Clears the armed flash when the confirm window passes with no second press, so the
    // armed state never LINGERS looking actionable (a further press would only re-arm). This
    // is what makes a manual "disarm" control unnecessary.
    QTimer *m_armTimer = nullptr;
    // Deliberately NOT given a literal default here. It is computed in the constructor by
    // ticketBlockedReason from the safe defaults below, so the sentence a blocked ticket
    // shows has exactly ONE source. A hand-written default was a second copy of that rule
    // and immediately disagreed with it — caught by TS-COCKPIT-008.
    QString m_ticketBlocked;
    double m_amount = 0.0;
    qint32 m_leverage = 1;
    bool m_hasCredentials = false;   // safe default: no account until told otherwise
    bool m_marketOpen = false;
    double m_minAmount = 0.0;
    double m_maxAmount = 0.0;
    QVariantList m_positions;
    QStringList m_instruments;
    double m_stopLoss = 0.0;      // 0 = no stop leg
    double m_takeProfit = 0.0;    // 0 = no target leg
    QString m_closedHistory;
    QVariantList m_closedRows;
    QVariantList m_events;
    // The lookback for the closed-trade summary; 13 weeks is the window the user asked for
    // and the Widgets closed-trades view already offers.
    static constexpr qint32 kHistoryWeeks = 13;

    // How many candles are drawn. 120 across a ~940-pixel plot leaves each body about five
    // pixels wide, which is where the hollow interior becomes visible inside its one-pixel
    // border; a full 339-bar session leaves three, and every candle then reads as solid.
    static constexpr qsizetype kMaxDrawnCandles = 120;
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_COCKPITMODEL_H
