// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_COCKPITMODEL_H
#define TRADINGAPP_UI_COCKPITMODEL_H

#include "domain/IndexConfluence.h"
#include "domain/LeadSignal.h"

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

public:
    explicit CockpitModel(QObject *parent = nullptr);

    void setCards(const QList<CockpitCard> &cards);
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
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_COCKPITMODEL_H
