// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CockpitModel.h"

#include <QVariantMap>

namespace trading::ui {

namespace {

// The three glyphs, in one place. Chosen so the states differ in SHAPE — filled, hollow,
// crossed — and remain distinguishable with no colour at all.
//
// QChar with an explicit code point, NOT QLatin1StringView. These are multi-byte in UTF-8,
// and QLatin1StringView treats each byte as its own Latin-1 character — so "●" arrived as
// three mojibake characters (U+00E2 U+0097 U+008F) and TS-COCKPIT-002 failed on it. All
// three are single BMP code points, so QChar is exact and encoding-independent.
constexpr QChar kAgrees(u'●');        // ● BLACK CIRCLE
constexpr QChar kDisagrees(u'○');     // ○ WHITE CIRCLE
constexpr QChar kUnmeasurable(u'✕');  // ✕ MULTIPLICATION X

ReadTick tickFor(const QString &label, const trading::Read &read, qint32 dir)
{
    ReadTick tick;
    tick.label = label;
    tick.detail = read.detail;
    if (!read.known) {
        // Unmeasurable FIRST, before any look at dir. A read that could not be taken has no
        // opinion, and its dir field is not evidence of one.
        tick.glyph = QString(kUnmeasurable);
        tick.state = QStringLiteral("unmeasurable");
        return tick;
    }
    // A measured-but-neutral read (dir 0) does not agree. Counting it as agreement would
    // inflate the fraction with reads that said nothing.
    const bool agrees = (dir != 0) && (read.dir == dir);
    tick.glyph = QString(agrees ? kAgrees : kDisagrees);
    tick.state = agrees ? QStringLiteral("agrees") : QStringLiteral("disagrees");
    return tick;
}

} // namespace

Freshness cockpitFreshness(qint64 ageMs, qint64 staleMs)
{
    if (ageMs < 0) {
        return Freshness::Absent;
    }
    return (ageMs < staleMs) ? Freshness::Live : Freshness::Lagging;
}

QString freshnessLabel(Freshness state, qint64 ageMs)
{
    switch (state) {
    case Freshness::Live:
        return QStringLiteral("live");
    case Freshness::Lagging: {
        // Minutes, because the measured lag on this feed is minutes rather than seconds and
        // "372000 ms old" tells a reader nothing.
        const qint64 minutes = ageMs / 60000;
        return (minutes >= 1) ? QStringLiteral("%1m old").arg(minutes)
                              : QStringLiteral("%1s old").arg(ageMs / 1000);
    }
    case Freshness::Absent:
        break;
    }
    // An em dash, not "0.00" and not the last known value.
    return QStringLiteral("—");
}

QList<ReadTick> cockpitTicks(const trading::IndexReads &reads, qint32 dir)
{
    // The order is the order the window and the model prompt use, so a reader comparing the
    // two is comparing the same list.
    return {
        tickFor(QStringLiteral("futures lead"), reads.futuresLead, dir),
        tickFor(QStringLiteral("futures momentum"), reads.futuresMomentum, dir),
        tickFor(QStringLiteral("volatility direction"), reads.volatility, dir),
        tickFor(QStringLiteral("US 10-year"), reads.yields, dir),
        tickFor(QStringLiteral("yield curve"), reads.curve, dir),
        tickFor(QStringLiteral("participation"), reads.participation, dir),
        tickFor(QStringLiteral("above own VWAP"), reads.aboveVwap, dir),
        tickFor(QStringLiteral("up/down volume"), reads.upDownVolume, dir),
        tickFor(QStringLiteral("opening range"), reads.structure, dir),
    };
}

QString cockpitAgreementText(const QList<ReadTick> &ticks)
{
    qint32 agreeing = 0;
    qint32 unmeasurable = 0;
    for (const ReadTick &tick : ticks) {
        if (tick.state == QStringLiteral("agrees")) {
            ++agreeing;
        } else if (tick.state == QStringLiteral("unmeasurable")) {
            ++unmeasurable;
        }
    }
    const auto total = static_cast<qint32>(ticks.size());
    QString text = QStringLiteral("%1 of %2 agree").arg(agreeing).arg(total);
    if (unmeasurable > 0) {
        // Always appended when nonzero. "6 of 9" and "6 of 9 with 3 unmeasurable" are
        // different facts and REQ-F-035 forbids collapsing them.
        text += QStringLiteral(" · %1 unmeasurable").arg(unmeasurable);
    }
    return text;
}

QString cockpitProbabilityText(qint32 samples, qint32 minSamples, double hitRate)
{
    if (samples < minSamples) {
        // No number at all. Naming the shortfall is the honest answer and the same one
        // paperLiveReadiness gives for real money.
        return QStringLiteral("UNCALIBRATED — %1 of %2 samples").arg(samples).arg(minSamples);
    }
    // A measured hit rate IS a frequency, so a percentage is legitimate here and only here.
    return QStringLiteral("P(up) %1%  (measured over %2 samples)")
        .arg(hitRate * 100.0, 0, 'f', 0)
        .arg(samples);
}

QString cockpitEvidenceText(const trading::LeadSignal &signal)
{
    const QString side = (signal.dir > 0) ? QStringLiteral("LONG")
                         : (signal.dir < 0) ? QStringLiteral("SHORT")
                                            : QStringLiteral("NO SIDE");
    // "Evidence", on a 0..100 scale, with no percent sign anywhere. The word "confidence"
    // is avoided deliberately: it reads as a probability and this number is not one.
    return QStringLiteral("%1 · evidence %2 of 100 · %3")
        .arg(side)
        .arg(signal.strength, 0, 'f', 0)
        .arg(trading::leadGradeWord(signal.grade));
}

QVariantMap cardToVariant(const CockpitCard &card)
{
    QVariantMap out;
    out[QStringLiteral("symbol")] = card.symbol;
    out[QStringLiteral("freshness")] = static_cast<qint32>(card.freshness);
    out[QStringLiteral("freshnessLabel")] = freshnessLabel(card.freshness, card.ageMs);
    const bool absent = (card.freshness == Freshness::Absent);
    // An absent value crosses as an em dash rather than as a number, so no QML binding can
    // render a missing price as 0.00 by accident.
    out[QStringLiteral("price")] = absent ? QStringLiteral("—")
                                          : QStringLiteral("%1").arg(card.price, 0, 'f', 2);
    out[QStringLiteral("changePct")] = absent
                                           ? QStringLiteral("—")
                                           : QStringLiteral("%1%2%")
                                                 .arg(card.changePct >= 0.0
                                                          ? QStringLiteral("+")
                                                          : QString())
                                                 .arg(card.changePct, 0, 'f', 2);
    // The SIGN as its own field: the second channel beside colour, so direction survives
    // without colour discrimination.
    out[QStringLiteral("dir")] = absent ? 0 : (card.changePct >= 0.0 ? 1 : -1);
    return out;
}

CockpitCard cardFromSeries(const QString &label, const QList<double> &series)
{
    CockpitCard card;
    card.symbol = label;
    if ((series.size() < 2) || (series.constFirst() <= 0.0)) {
        // Absent, not zero: no series means no reading, and the card renders an em dash.
        // A single point is also absent — a change needs two.
        card.freshness = Freshness::Absent;
        card.ageMs = -1;
        return card;
    }
    card.price = series.constLast();
    card.changePct = ((series.constLast() - series.constFirst()) / series.constFirst()) * 100.0;
    // The reference sweep runs on its own timer and this project does not record a per-ticker
    // timestamp, so a series we hold is reported as live rather than given an invented age.
    // Inventing one would be the exact failure the freshness field exists to prevent.
    card.ageMs = 0;
    card.freshness = Freshness::Live;
    return card;
}

QList<CockpitCard> referenceCards(const QHash<QString, QList<double>> &referenceSeries)
{
    // Label, then the Yahoo ticker it is keyed by. The two differ on purpose: SP.24-7 is this
    // application's symbol for the S&P future and is not a ticker anyone would recognise on a
    // card.
    return {
        cardFromSeries(QStringLiteral("SPX500"),
                       referenceSeries.value(QStringLiteral("SP.24-7"))),
        cardFromSeries(QStringLiteral("NSDQ100"),
                       referenceSeries.value(QStringLiteral("NSDQ100.24-7"))),
        cardFromSeries(QStringLiteral("VIX"), referenceSeries.value(QStringLiteral("^VIX"))),
        cardFromSeries(QStringLiteral("US10Y"), referenceSeries.value(QStringLiteral("^TNX"))),
    };
}

QVariantMap candleToVariant(const trading::Candle &candle)
{
    QVariantMap out;
    out[QStringLiteral("open")] = candle.open;
    out[QStringLiteral("high")] = candle.high;
    out[QStringLiteral("low")] = candle.low;
    out[QStringLiteral("close")] = candle.close;
    out[QStringLiteral("up")] = candle.up();
    return out;
}

CockpitModel::CockpitModel(QObject *parent) : QObject(parent)
{
    setCandles({});   // start in the stated "no bars yet" state rather than at a blank axis
}

void CockpitModel::setCandles(const QList<trading::Candle> &candles)
{
    // The tail only. See recentCandles: this is what keeps the hollow-versus-solid body wide
    // enough to READ, which is the chart's colour-blind guarantee rather than a preference.
    const QList<trading::Candle> drawn = trading::recentCandles(candles, kMaxDrawnCandles);

    m_candles.clear();
    m_candles.reserve(drawn.size());
    for (const trading::Candle &candle : drawn) {
        m_candles.append(candleToVariant(candle));
    }

    // SAID, not silently truncated. A chart showing two hours of a six-hour session while
    // labelled only "1-minute candles" is a claim about the session that is not true.
    m_candleSpan = (drawn.size() < candles.size())
                       ? tr("last %1 of %2 one-minute bars")
                             .arg(drawn.size())
                             .arg(candles.size())
                       : tr("%n one-minute bar(s)", nullptr, static_cast<int>(drawn.size()));

    // Fitted to the candles actually DRAWN, not to the whole series: an axis covering bars
    // that are off-screen leaves the visible ones squashed into a band in the middle.
    const std::optional<trading::CandleRange> range = trading::candleRange(drawn);
    if (!range.has_value()) {
        // SAID, not implied. An empty axis and a flat market look identical on screen, and
        // the difference is whether the reader is looking at the market or at a broken feed.
        m_candleMin = 0.0;
        m_candleMax = 0.0;
        m_candleNote = tr("no bars — waiting for the first intraday series");
        m_candleSpan.clear();
        Q_EMIT changed();
        return;
    }
    // 4% of the session span on each side. The axis is fitted to the WICKS (candleRange
    // reads low/high, never the bodies), so the extremes stay inside the frame instead of
    // sitting on it.
    const trading::CandleRange padded = trading::paddedRange(*range, 0.04);
    m_candleMin = padded.low;
    m_candleMax = padded.high;
    m_candleNote.clear();
    Q_EMIT changed();
}

void CockpitModel::setCards(const QList<CockpitCard> &cards)
{
    m_cards.clear();
    m_cards.reserve(cards.size());
    for (const CockpitCard &card : cards) {
        m_cards.append(cardToVariant(card));
    }
    Q_EMIT changed();
}

void CockpitModel::setSignal(const QString &instrument, const trading::LeadSignal &signal,
                             const trading::IndexReads &reads)
{
    m_instrument = instrument;
    // `read` rather than `ticks`: a local named `ticks` shadows the ticks() accessor, which
    // cppcheck reports as shadowFunction — the same finding a local named `net` produced in
    // PaperTrader. Harmless here, but the gate is zero and the rename costs nothing.
    const QList<ReadTick> readTicks = cockpitTicks(reads, signal.dir);
    m_ticks.clear();
    m_ticks.reserve(readTicks.size());
    for (const ReadTick &tick : readTicks) {
        QVariantMap entry;
        entry[QStringLiteral("glyph")] = tick.glyph;
        entry[QStringLiteral("label")] = tick.label;
        entry[QStringLiteral("state")] = tick.state;
        entry[QStringLiteral("detail")] = tick.detail;
        m_ticks.append(entry);
    }
    m_agreement = cockpitAgreementText(readTicks);
    m_evidence = cockpitEvidenceText(signal);
    Q_EMIT changed();
}

void CockpitModel::setCalibration(qint32 samples, qint32 minSamples, double hitRate)
{
    m_probability = cockpitProbabilityText(samples, minSamples, hitRate);
    Q_EMIT changed();
}

void CockpitModel::setSimulation(bool on)
{
    m_simulation = on;
    Q_EMIT changed();
}

} // namespace trading::ui
