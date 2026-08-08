// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CockpitModel.h"

#include <QDateTime>
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

QList<CockpitCard> referenceCards(const QHash<QString, QList<double>> &byTicker,
                                  const QHash<QString, QList<double>> &bySymbol)
{
    // Each card says WHICH book it comes from, at the point of use. The futures are app
    // SYMBOLS and the volatility/yield reads are Yahoo TICKERS; reading either from the
    // other book yields nothing, silently, forever.
    return {
        cardFromSeries(QStringLiteral("SPX500"), bySymbol.value(QStringLiteral("SP.24-7"))),
        cardFromSeries(QStringLiteral("NSDQ100"),
                       bySymbol.value(QStringLiteral("NSDQ100.24-7"))),
        cardFromSeries(QStringLiteral("VIX"), byTicker.value(QStringLiteral("^VIX"))),
        cardFromSeries(QStringLiteral("US10Y"), byTicker.value(QStringLiteral("^TNX"))),
    };
}

QVariantMap positionToVariant(const Position &position, double markPrice)
{
    QVariantMap out;
    out[QStringLiteral("id")] = position.positionId;
    out[QStringLiteral("symbol")] = position.symbol;
    out[QStringLiteral("side")] = position.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");
    // The side as a SIGN too, so direction survives without colour discrimination — the same
    // second channel the market cards carry.
    out[QStringLiteral("dir")] = position.isBuy ? 1 : -1;
    out[QStringLiteral("amount")] = QStringLiteral("%1").arg(position.amount, 0, 'f', 2);
    out[QStringLiteral("leverage")] = QStringLiteral("x%1").arg(position.leverage);
    out[QStringLiteral("openRate")] = QStringLiteral("%1").arg(position.openRate, 0, 'f', 2);
    // An unknown mark is an em dash, never 0.00 and never the entry price dressed up as the
    // current one — the absent-is-not-zero rule this whole view is built on.
    const bool markable = (markPrice > 0.0) && (position.openRate > 0.0);
    out[QStringLiteral("markRate")] =
        markable ? QStringLiteral("%1").arg(markPrice, 0, 'f', 2) : QStringLiteral("—");
    return out;
}

QString ticketBlockedReason(bool hasCredentials, bool marketOpen, double amount,
                            double minAmount, double maxAmount)
{
    // Most fundamental first. A reader shown three obstacles at once fixes the wrong one,
    // and the ones below are not even reachable while the one above holds.
    if (!hasCredentials) {
        return QObject::tr("SIMULATION — no account configured, no order can be placed");
    }
    if (!marketOpen) {
        return QObject::tr("market closed for this instrument");
    }
    if (amount <= 0.0) {
        return QObject::tr("enter an amount");
    }
    // An unknown bound (0) disables its own check rather than inventing one — the same rule
    // OrderRequestValidator follows for every fact it was not given.
    if ((minAmount > 0.0) && (amount < minAmount)) {
        return QObject::tr("below the %1 minimum").arg(minAmount, 0, 'f', 2);
    }
    if ((maxAmount > 0.0) && (amount > maxAmount)) {
        return QObject::tr("above the %1 maximum").arg(maxAmount, 0, 'f', 2);
    }
    return {};   // no obstacle: the ticket may be used
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
    // The blocked sentence comes from the same function that computes it later, so a
    // freshly built model and a re-evaluated one cannot say different things about the
    // same state. The defaults are the safe ones: no account, market shut.
    m_ticketBlocked =
        ticketBlockedReason(m_hasCredentials, m_marketOpen, m_amount, m_minAmount, m_maxAmount);
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

void CockpitModel::setTradeContext(bool hasCredentials, bool marketOpen, double minAmount,
                                   double maxAmount)
{
    m_hasCredentials = hasCredentials;
    m_marketOpen = marketOpen;
    m_minAmount = minAmount;
    m_maxAmount = maxAmount;
    m_ticketBlocked =
        ticketBlockedReason(m_hasCredentials, m_marketOpen, m_amount, m_minAmount, m_maxAmount);
    // Anything armed is abandoned when the ground shifts under it. A confirmation given
    // while the market was open must not survive into a market that has closed.
    if (!m_ticketBlocked.isEmpty()) {
        m_gate = trading::ConfirmGate{};
        m_ticketPrompt.clear();
    }
    Q_EMIT changed();
}

void CockpitModel::setInstruments(const QStringList &symbols)
{
    m_instruments = symbols;
    Q_EMIT changed();
}

void CockpitModel::selectInstrument(const QString &symbol)
{
    if (symbol.isEmpty() || (symbol == m_instrument)) {
        return;
    }
    // Disarm BEFORE asking for the switch. An arming that survived an instrument change
    // would send the confirmed order against a different market.
    cancelArm();
    Q_EMIT instrumentRequested(symbol);
}

void CockpitModel::setTicket(double amount, qint32 leverage, double stopLoss,
                             double takeProfit)
{
    m_amount = amount;
    m_leverage = leverage;
    m_stopLoss = stopLoss;
    m_takeProfit = takeProfit;
    m_ticketBlocked =
        ticketBlockedReason(m_hasCredentials, m_marketOpen, m_amount, m_minAmount, m_maxAmount);
    // CHANGING THE ORDER DISARMS IT. The user confirmed a specific order; editing the amount
    // or the leverage makes the armed action a different one, and letting the old arming
    // stand would send something nobody pressed twice for.
    m_gate = trading::ConfirmGate{};
    m_ticketPrompt.clear();
    Q_EMIT changed();
}

void CockpitModel::setPositions(const QList<Position> &positions, double markPrice)
{
    m_positions.clear();
    m_positions.reserve(positions.size());
    for (const Position &p : positions) {
        m_positions.append(positionToVariant(p, markPrice));
    }
    Q_EMIT changed();
}

void CockpitModel::press(bool buy)
{
    if (!m_ticketBlocked.isEmpty()) {
        // Refuse loudly rather than silently: a dead control is the failure this project
        // already measured with the quick keys, where a swallowed press "seemed dead".
        m_ticketPrompt = m_ticketBlocked;
        Q_EMIT changed();
        return;
    }
    // The action string names the WHOLE order. Two presses that would send different orders
    // must never combine into one confirmation, so amount and leverage are part of it.
    // The action names the WHOLE order — instrument, side, size, leverage AND both legs.
    // Anything left out could change between the two presses without disarming, which is
    // exactly the confirmation-harvesting this string exists to prevent.
    const QString action = QStringLiteral("%1 %2 %3 at x%4 · SL %5 · TP %6")
                               .arg(buy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                                    m_instrument)
                               .arg(m_amount, 0, 'f', 2)
                               .arg(m_leverage)
                               .arg(m_stopLoss, 0, 'f', 2)
                               .arg(m_takeProfit, 0, 'f', 2);
    const trading::ConfirmDecision decision = trading::confirmPress(
        m_gate, action, QDateTime::currentMSecsSinceEpoch(), trading::kConfirmWindowMs);
    m_gate = decision.next;
    m_ticketPrompt = decision.prompt;
    Q_EMIT changed();
    if (decision.commit) {
        Q_EMIT placeRequested(buy, m_amount, m_leverage, m_stopLoss, m_takeProfit);
    }
}

void CockpitModel::pressClose(const QString &positionId)
{
    // Keyed by the position id, so confirming a close of one trade can never close another
    // — pressing close on #1 then on #2 arms #2 rather than committing #1.
    const QString action = QStringLiteral("close position #%1").arg(positionId);
    const trading::ConfirmDecision decision = trading::confirmPress(
        m_gate, action, QDateTime::currentMSecsSinceEpoch(), trading::kConfirmWindowMs);
    m_gate = decision.next;
    m_ticketPrompt = decision.prompt;
    Q_EMIT changed();
    if (decision.commit) {
        Q_EMIT closeRequested(positionId);
    }
}

void CockpitModel::cancelArm()
{
    m_gate = trading::ConfirmGate{};
    m_ticketPrompt.clear();
    Q_EMIT changed();
}

} // namespace trading::ui
