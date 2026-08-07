// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_COCKPITMODEL_H
#define TRADINGAPP_UI_COCKPITMODEL_H

#include "domain/Candles.h"
#include "domain/IndexConfluence.h"
#include "domain/LeadSignal.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

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

// The four references a trader watches beside the instrument, in a fixed order, from a book
// keyed by Yahoo TICKER. Fixed because a card that moves between refreshes is unreadable.
[[nodiscard]] QList<CockpitCard> referenceCards(
    const QHash<QString, QList<double>> &referenceSeries);

// One candle as Qt Graphs' CustomSeries sees it — a QVariantMap the delegate reads by name.
//
// `up` is computed HERE rather than in the delegate as `close >= open`. The QML would get
// the same answer today, but the tie rule (flat bar counts as up) is a decision this project
// made once and pinned with a test; recomputing it in a binding is how two answers to one
// question start to exist. The series itself adds `index`.
[[nodiscard]] QVariantMap candleToVariant(const trading::Candle &candle);

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
    Q_PROPERTY(bool simulation READ simulation NOTIFY changed)
    // The price chart. `candleNote` is non-empty exactly when there is nothing to draw, so
    // the QML has one thing to test and cannot render an empty axis as a quiet market.
    Q_PROPERTY(QVariantList candles READ candles NOTIFY changed)
    Q_PROPERTY(double candleMin READ candleMin NOTIFY changed)
    Q_PROPERTY(double candleMax READ candleMax NOTIFY changed)
    Q_PROPERTY(QString candleNote READ candleNote NOTIFY changed)
    Q_PROPERTY(QString candleSpan READ candleSpan NOTIFY changed)

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
    [[nodiscard]] bool simulation() const { return m_simulation; }
    [[nodiscard]] QVariantList candles() const { return m_candles; }
    [[nodiscard]] double candleMin() const { return m_candleMin; }
    [[nodiscard]] double candleMax() const { return m_candleMax; }
    [[nodiscard]] QString candleNote() const { return m_candleNote; }
    [[nodiscard]] QString candleSpan() const { return m_candleSpan; }

Q_SIGNALS:
    void changed();

private:
    QVariantList m_cards;
    QVariantList m_ticks;
    QString m_agreement;
    QString m_evidence;
    QString m_probability;
    QString m_instrument;
    bool m_simulation = true;   // safe default: claim simulation until told otherwise
    QVariantList m_candles;
    double m_candleMin = 0.0;
    double m_candleMax = 0.0;
    // Non-empty until real bars arrive, so a cockpit opened before the first fetch says so
    // rather than presenting an empty frame.
    QString m_candleNote;
    QString m_candleSpan;

    // How many candles are drawn. 120 across a ~940-pixel plot leaves each body about five
    // pixels wide, which is where the hollow interior becomes visible inside its one-pixel
    // border; a full 339-bar session leaves three, and every candle then reads as solid.
    static constexpr qsizetype kMaxDrawnCandles = 120;
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_COCKPITMODEL_H
