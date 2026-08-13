// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/IndexConfluence.h"

#include "domain/DecisionEngine.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>

namespace trading {

namespace {

// The ten names that carry most of the Nasdaq-100's weight, and the ten that carry
// most of the S&P 500's. Two lists rather than one shared list, because the tail is
// where the two indices actually differ: Netflix and Costco move the Nasdaq and
// barely register in the S&P's weighting, while Berkshire and JPMorgan move the S&P
// and are not in the Nasdaq-100 at all. Reading the wrong tail is reading a
// different index.
//
// Fixed lists are deliberate: they document exactly what the participation read
// covers, and index weightings change on the order of years, not of scans. The eight
// shared names at the top are shared because both indices really are carried by the
// same megacaps — that overlap is the reason a heavyweight read says something about
// both at once.
QStringList nasdaqHeavyweightNames()
{
    return {QStringLiteral("NVDA"),  QStringLiteral("MSFT"), QStringLiteral("AAPL"),
            QStringLiteral("AMZN"),  QStringLiteral("AVGO"), QStringLiteral("META"),
            QStringLiteral("GOOGL"), QStringLiteral("TSLA"), QStringLiteral("NFLX"),
            QStringLiteral("COST")};
}

QStringList spHeavyweightNames()
{
    // BRK-B is the Yahoo spelling of Berkshire Hathaway class B (BRK.B elsewhere).
    return {QStringLiteral("NVDA"),  QStringLiteral("MSFT"), QStringLiteral("AAPL"),
            QStringLiteral("AMZN"),  QStringLiteral("META"), QStringLiteral("AVGO"),
            QStringLiteral("GOOGL"), QStringLiteral("TSLA"), QStringLiteral("BRK-B"),
            QStringLiteral("JPM")};
}

// Which list applies to a symbol. A Nasdaq instrument is read by the Nasdaq-100
// heavyweights; everything else — the S&P instruments, and any other symbol that
// borrows the broad-market read — by the S&P 500's.
bool isNasdaqSymbol(const QString &symbol)
{
    return symbol.startsWith(QStringLiteral("NSDQ"), Qt::CaseInsensitive)
        || symbol.startsWith(QStringLiteral("NQ"), Qt::CaseInsensitive);
}

// Approximate index weight (share of the index, in percent) of a top-ten constituent.
// A STATIC snapshot, the same standing the ticker lists have: real weightings drift on
// the order of quarters, not scans, and this app does not fetch live weights. It is the
// honest counterpart of the breadth caveat — enough to weight NVDA's move above COST's
// by roughly the right ratio, not a claim to the index's exact construction. Only relative
// magnitudes matter, because the cap-weighted change renormalises over the READABLE names.
// A function-local static (not a file-scope QHash) so the QString constructors run lazily
// on first use, where an exception can be handled (CERT-ERR58-CPP), like instrumentSpec().
double heavyweightWeight(const QString &symbol, const QString &ticker)
{
    static const QHash<QString, double> kNasdaq = {
        {QStringLiteral("NVDA"), 8.9},  {QStringLiteral("MSFT"), 7.8},
        {QStringLiteral("AAPL"), 7.5},  {QStringLiteral("AMZN"), 5.5},
        {QStringLiteral("AVGO"), 4.8},  {QStringLiteral("META"), 4.6},
        {QStringLiteral("GOOGL"), 5.1}, {QStringLiteral("TSLA"), 3.0},
        {QStringLiteral("NFLX"), 2.5},  {QStringLiteral("COST"), 2.5}};
    static const QHash<QString, double> kSp = {
        {QStringLiteral("NVDA"), 7.0},  {QStringLiteral("MSFT"), 6.5},
        {QStringLiteral("AAPL"), 6.5},  {QStringLiteral("AMZN"), 3.8},
        {QStringLiteral("META"), 2.6},  {QStringLiteral("AVGO"), 2.4},
        {QStringLiteral("GOOGL"), 3.9}, {QStringLiteral("TSLA"), 1.6},
        {QStringLiteral("BRK-B"), 1.7}, {QStringLiteral("JPM"), 1.4}};
    return (isNasdaqSymbol(symbol) ? kNasdaq : kSp).value(ticker, 0.0);
}

// The two futures the cash indices follow, as this app's own instrument symbols
// (their Yahoo tickers are NQ=F and ES=F — see the instrument catalog). Functions
// rather than file-scope QStrings: a static QString's constructor can throw where
// nothing can catch it (CERT-ERR58-CPP).
QString nasdaqFutureSymbol()
{
    return QStringLiteral("NSDQ100.24-7");
}

QString spFutureSymbol()
{
    return QStringLiteral("SP.24-7");
}

// Session change of a series in percent, or nothing when it cannot be measured.
std::optional<double> sessionChangePct(const QList<double> &series)
{
    if ((series.size() < 2) || (series.constFirst() <= 0.0)) {
        return std::nullopt;
    }
    return ((series.constLast() - series.constFirst()) / series.constFirst()) * 100.0;
}

// How many of the given series are up on the session, and how many could be read.
QPair<qint32, qint32> upCount(const QHash<QString, QList<double>> &series,
                              const QStringList &tickers)
{
    qint32 up = 0;
    qint32 measured = 0;
    for (const QString &ticker : tickers) {
        const std::optional<double> change = sessionChangePct(series.value(ticker));
        if (!change.has_value()) {
            continue;
        }
        ++measured;
        if (*change > 0.0) {
            ++up;
        }
    }
    return {up, measured};
}

// The futures that lead the cash market: Nasdaq against S&P this session. Technology
// leading supports a long on a technology index; lagging does the opposite.
Read futuresLeadRead(const QHash<QString, QList<double>> &series)
{
    Read out;
    const double lead =
        relativeStrength(series.value(nasdaqFutureSymbol()), series.value(spFutureSymbol()));
    if (qFuzzyIsNull(lead)) {
        return out;
    }
    out.known = true;
    out.dir = (lead > 0.0) ? 1 : -1;
    out.detail = QStringLiteral("Nasdaq vs S&P %1%2%")
                     .arg((lead > 0.0) ? QStringLiteral("+") : QString())
                     .arg(lead, 0, 'f', 2);
    return out;
}

// Expected volatility, by its DIRECTION rather than its level: falling volatility
// supports risk, rising volatility is traders buying protection. The Nasdaq has its
// own index (^VXN); everything else is judged by ^VIX.
Read volatilityRead(const QString &symbol, const QHash<QString, QList<double>> &series)
{
    Read out;
    const bool nasdaq = symbol.startsWith(QStringLiteral("NSDQ"), Qt::CaseInsensitive);
    const QString ticker = nasdaq ? QStringLiteral("^VXN") : QStringLiteral("^VIX");
    const std::optional<double> change = sessionChangePct(series.value(ticker));
    if (!change.has_value()) {
        return out;
    }
    out.known = true;
    out.dir = (*change < 0.0) ? 1 : ((*change > 0.0) ? -1 : 0);
    out.detail = QStringLiteral("%1 %2%3%")
                     .arg(ticker, (*change > 0.0) ? QStringLiteral("+") : QString())
                     .arg(*change, 0, 'f', 2);
    return out;
}

// The US 10-year yield: rising yields press on growth shares, so a rising yield
// argues against a long. The relationship is not a law — it is one vote.
Read yieldRead(const QHash<QString, QList<double>> &series)
{
    Read out;
    const std::optional<double> change =
        sessionChangePct(series.value(QStringLiteral("^TNX")));
    if (!change.has_value()) {
        return out;
    }
    out.known = true;
    out.dir = (*change > 0.0) ? -1 : ((*change < 0.0) ? 1 : 0);
    out.detail = QStringLiteral("US 10y %1%2%")
                     .arg((*change > 0.0) ? QStringLiteral("+") : QString())
                     .arg(*change, 0, 'f', 2);
    return out;
}

// How many of the heavyweights are actually up. NOT breadth in the professional
// sense — that needs per-constituent data this app does not fetch — but it answers
// the question breadth is asked for: is the index carried by everything, or by one
// or two names?
Read participationRead(const QString &symbol, const QHash<QString, QList<double>> &series)
{
    Read out;
    const QStringList names = indexHeavyweights(symbol);
    const QPair<qint32, qint32> up = upCount(series, names);
    // Fewer than half the names readable is not a participation read — with two of ten
    // fetched, "both up" says nothing about whether the index is being carried.
    if (up.second < static_cast<qint32>(names.size() / 2)) {
        return out;
    }
    out.known = true;
    const double share = static_cast<double>(up.first) / static_cast<double>(up.second);
    // Scale-free thresholds: roughly two thirds up is broad participation, roughly two
    // thirds down is broad distribution, anything between is a split field.
    out.dir = (share >= 0.625) ? 1 : ((share <= 0.375) ? -1 : 0);
    out.detail = QStringLiteral("%1 of %2 %3 heavyweights up")
                     .arg(up.first)
                     .arg(up.second)
                     .arg(isNasdaqSymbol(symbol) ? QStringLiteral("Nasdaq-100")
                                                 : QStringLiteral("S&P 500"));
    return out;
}

// The short end of the curve against the long end — the policy read, and the one that
// matters most to a technology index. A front end rising faster than the long end is
// the market pricing tighter policy, which presses on the multiple of everything that
// earns its money later. Independent of the 10-year read: that one measures the level
// of long rates, this one measures the SHAPE, and the two regularly disagree.
//
// The 2-year yield future is the honest instrument for the front end; the 13-week bill
// is the fallback, because it is the one Yahoo serves most reliably. The read names
// which one it actually got — a read whose source is ambiguous is not a read.
Read curveRead(const QHash<QString, QList<double>> &series)
{
    Read out;
    const std::optional<double> longEnd = sessionChangePct(series.value(QStringLiteral("^TNX")));
    QString frontName = QStringLiteral("US 2y");
    std::optional<double> frontEnd = sessionChangePct(series.value(QStringLiteral("2YY=F")));
    if (!frontEnd.has_value()) {
        frontName = QStringLiteral("US 13w");
        frontEnd = sessionChangePct(series.value(QStringLiteral("^IRX")));
    }
    if (!frontEnd.has_value() || !longEnd.has_value()) {
        return out;
    }
    out.known = true;
    // Only a MEANINGFUL divergence is evidence. Both ends drifting a few hundredths
    // together is the curve holding still, and calling that a policy signal would turn
    // noise into a vote.
    const double divergence = *frontEnd - *longEnd;
    constexpr double kMeaningfulDivergencePct = 0.5;
    if (divergence > kMeaningfulDivergencePct) {
        out.dir = -1;    // front end leading higher: tightening pressure
    } else if (divergence < -kMeaningfulDivergencePct) {
        out.dir = 1;     // front end leading lower: easing pressure
    }
    out.detail = QStringLiteral("%1 %2%3% vs 10y %4%5%")
                     .arg(frontName, (*frontEnd > 0.0) ? QStringLiteral("+") : QString())
                     .arg(*frontEnd, 0, 'f', 2)
                     .arg((*longEnd > 0.0) ? QStringLiteral("+") : QString())
                     .arg(*longEnd, 0, 'f', 2);
    return out;
}

// The leading future's own push, read over three horizons at once: the last minute,
// the last five and the last fifteen.
//
// Deliberately ONE read rather than three. A 1-minute return, a 5-minute return and a
// 15-minute return computed from the same series are one piece of evidence wearing
// three hats — exactly what REQ-F-035 exists to refuse — so what counts here is
// whether they AGREE. Three horizons pointing the same way is a push with staying
// power; a 1-minute pop against a 15-minute slide is noise, and is reported as the
// neutral it is.
Read futuresMomentumRead(const QString &symbol, const QHash<QString, QList<double>> &bySymbol)
{
    Read out;
    const QString future = isNasdaqSymbol(symbol) ? nasdaqFutureSymbol() : spFutureSymbol();
    const QList<double> series = bySymbol.value(future);
    constexpr qsizetype kLongestHorizon = 15;
    if (series.size() <= kLongestHorizon) {
        return out;   // not enough session to read the longest horizon
    }
    const double last = series.constLast();
    const auto returnOver = [&series, last](qsizetype bars) {
        const double then = series.at(series.size() - 1 - bars);
        return (then > 0.0) ? (((last - then) / then) * 100.0) : 0.0;
    };
    const double oneMin = returnOver(1);
    const double fiveMin = returnOver(5);
    const double fifteenMin = returnOver(kLongestHorizon);
    out.known = true;
    if ((oneMin > 0.0) && (fiveMin > 0.0) && (fifteenMin > 0.0)) {
        out.dir = 1;
    } else if ((oneMin < 0.0) && (fiveMin < 0.0) && (fifteenMin < 0.0)) {
        out.dir = -1;
    }
    out.detail = QStringLiteral("%1 1m %2% · 5m %3% · 15m %4%%5")
                     .arg(future)
                     .arg(oneMin, 0, 'f', 2)
                     .arg(fiveMin, 0, 'f', 2)
                     .arg(fifteenMin, 0, 'f', 2)
                     .arg((out.dir == 0) ? QStringLiteral(" — horizons disagree") : QString());
    return out;
}

// How many of the heavyweights trade above their OWN session VWAP.
//
// This is the closest this app can honestly get to the breadth measure a professional
// desk actually watches. It is not the advance/decline line and not the share of all
// 100 or 500 constituents — it is ten names — but unlike the plain up-count it knows
// WHERE in the session the buying happened: a name up on the day yet below its VWAP
// has been distributed into all morning, and the count alone cannot see that.
Read aboveVwapRead(const QString &symbol, const QHash<QString, VolumeSeries> &volume)
{
    Read out;
    const QStringList names = indexHeavyweights(symbol);
    qint32 above = 0;
    qint32 measured = 0;
    for (const QString &ticker : names) {
        const VolumeSeries bars = volume.value(ticker);
        const std::optional<double> vwap = bars.vwap();
        if (!vwap.has_value()) {
            continue;
        }
        ++measured;
        if (bars.closes.constLast() > *vwap) {
            ++above;
        }
    }
    if (measured < static_cast<qint32>(names.size() / 2)) {
        return out;   // same rule as the participation read: half the field or it says nothing
    }
    out.known = true;
    const double share = static_cast<double>(above) / static_cast<double>(measured);
    out.dir = (share >= 0.625) ? 1 : ((share <= 0.375) ? -1 : 0);
    out.detail = QStringLiteral("%1 of %2 above own VWAP (stand-in for breadth)")
                     .arg(above)
                     .arg(measured);
    return out;
}

// Where the VOLUME is, rather than how many names moved: the session volume behind the
// up names against the session volume behind the down names.
//
// Independent of the up-count on purpose. Six names up and four down is a positive
// field by count, but if the four carry twice the volume the move is being sold into.
// This is up/down volume at ten-name resolution — the honest fraction of the real
// measure, which needs every constituent.
Read upDownVolumeRead(const QString &symbol, const QHash<QString, VolumeSeries> &volume)
{
    Read out;
    const QStringList names = indexHeavyweights(symbol);
    double upVolume = 0.0;
    double downVolume = 0.0;
    qint32 measured = 0;
    for (const QString &ticker : names) {
        const VolumeSeries bars = volume.value(ticker);
        const std::optional<double> change = sessionChangePct(bars.closes);
        const std::optional<double> total = bars.totalVolume();
        if (!change.has_value() || !total.has_value()) {
            continue;
        }
        ++measured;
        if (*change > 0.0) {
            upVolume += *total;
        } else if (*change < 0.0) {
            downVolume += *total;
        }
    }
    const double traded = upVolume + downVolume;
    if ((measured < static_cast<qint32>(names.size() / 2)) || (traded <= 0.0)) {
        return out;
    }
    out.known = true;
    const double upShare = upVolume / traded;
    // A wider band than the up-count uses: volume is far more skewed than a count
    // (one megacap can out-trade three of its neighbours), so it takes a clearer
    // imbalance before this is evidence rather than the shape of the index.
    out.dir = (upShare >= 0.65) ? 1 : ((upShare <= 0.35) ? -1 : 0);
    out.detail = QStringLiteral("%1% of heavyweight volume behind the up names, %2 read")
                     .arg(upShare * 100.0, 0, 'f', 0)
                     .arg(measured);
    return out;
}

// Price structure: the opening range, where the session itself has already declared
// a direction.
Read structureRead(const QList<double> &ownSeries)
{
    Read out;
    const OpeningRange range = openingRange(ownSeries);
    if (!range.valid) {
        return out;
    }
    out.known = true;
    out.dir = range.breakDir;
    out.detail = (range.breakDir == 0)
                     ? QStringLiteral("inside the opening range")
                     : QStringLiteral("broke the opening range %1 by %2% of width")
                           .arg((range.breakDir > 0) ? QStringLiteral("up")
                                                     : QStringLiteral("down"))
                           .arg(range.breakPct, 0, 'f', 0);
    return out;
}

} // namespace

QStringList nasdaqHeavyweights()
{
    return nasdaqHeavyweightNames();
}

QStringList spHeavyweights()
{
    return spHeavyweightNames();
}

QStringList indexHeavyweights(const QString &symbol)
{
    return isNasdaqSymbol(symbol) ? nasdaqHeavyweightNames() : spHeavyweightNames();
}

std::optional<double> VolumeSeries::vwap() const
{
    // The two lists are filled together, so a size mismatch means something upstream
    // parsed them independently — refuse rather than average across a shift.
    if (closes.isEmpty() || (closes.size() != volumes.size())) {
        return std::nullopt;
    }
    double turnover = 0.0;
    double traded = 0.0;
    for (qsizetype i = 0; i < closes.size(); ++i) {
        const double volume = volumes.at(i);
        if ((closes.at(i) <= 0.0) || (volume <= 0.0)) {
            continue;
        }
        turnover += closes.at(i) * volume;
        traded += volume;
    }
    if (traded <= 0.0) {
        return std::nullopt;   // an index ticker: bars but no volume behind them
    }
    return turnover / traded;
}

std::optional<double> VolumeSeries::totalVolume() const
{
    if (volumes.isEmpty()) {
        return std::nullopt;
    }
    // Negative or zero entries are skipped rather than subtracted: a feed that reports
    // no trade for a minute is not negative turnover.
    const double traded =
        std::accumulate(volumes.cbegin(), volumes.cend(), 0.0, [](double sum, double volume) {
            return (volume > 0.0) ? (sum + volume) : sum;
        });
    return (traded > 0.0) ? std::optional<double>{traded} : std::nullopt;
}

TermStructure termStructure(const QHash<QString, QList<double>> &referenceSeries)
{
    const auto lastOf = [&referenceSeries](const QString &ticker) -> std::optional<double> {
        const QList<double> series = referenceSeries.value(ticker);
        if (series.isEmpty() || (series.constLast() <= 0.0)) {
            return std::nullopt;
        }
        return series.constLast();
    };
    TermStructure out;
    const std::optional<double> nearTerm = lastOf(QStringLiteral("^VIX9D"));
    // Three-month is the preferred far leg; thirty-day is the fallback, because a
    // 9-day above a 30-day is already the inversion this read is looking for.
    QString farName = QStringLiteral("^VIX3M");
    std::optional<double> farTerm = lastOf(farName);
    if (!farTerm.has_value()) {
        farName = QStringLiteral("^VIX");
        farTerm = lastOf(farName);
    }
    if (!nearTerm.has_value() || !farTerm.has_value()) {
        out.detail = QStringLiteral("term structure: not measurable");
        return out;
    }
    out.known = true;
    out.nearFarRatio = *nearTerm / *farTerm;
    out.inverted = out.nearFarRatio > 1.0;
    out.detail = out.inverted
                     ? QStringLiteral("term structure INVERTED: ^VIX9D %1 above %2 %3 — "
                                      "protection is being bought for right now")
                           .arg(*nearTerm, 0, 'f', 1)
                           .arg(farName)
                           .arg(*farTerm, 0, 'f', 1)
                     : QStringLiteral("term structure normal: ^VIX9D %1 below %2 %3")
                           .arg(*nearTerm, 0, 'f', 1)
                           .arg(farName)
                           .arg(*farTerm, 0, 'f', 1);
    return out;
}

QStringList referenceTickers()
{
    QStringList out{QStringLiteral("^VIX"), QStringLiteral("^VXN"), QStringLiteral("^TNX"),
                    // The volatility term structure (REQ-F-035): the near leg and the
                    // far leg, so an inverted curve can be SEEN rather than inferred
                    // from the level of one number.
                    QStringLiteral("^VIX9D"), QStringLiteral("^VIX3M"),
                    // The short end of the yield curve. Both are listed because the
                    // 2-year yield future is the right instrument and the 13-week bill
                    // is the one that is always served; whichever arrives is used, and
                    // the read names it.
                    QStringLiteral("2YY=F"), QStringLiteral("^IRX")};
    // The union of both heavyweight lists, in a stable order and without duplicates:
    // the sweep fetches every name once, and each index's read picks its own subset.
    for (const QString &name : nasdaqHeavyweightNames() + spHeavyweightNames()) {
        if (!out.contains(name)) {
            out.append(name);
        }
    }
    return out;
}

ReadInputs readInputsFor(const QString &symbol,
                         const QHash<QString, QList<double>> &reference,
                         const QHash<QString, VolumeSeries> &volume,
                         const QHash<QString, QList<double>> &bySymbol)
{
    ReadInputs out;
    out.reference = reference;
    out.volume = volume;
    out.bySymbol = bySymbol;
    out.ownSeries = bySymbol.value(symbol);
    return out;
}

IndexReads indexReads(const QString &symbol, const ReadInputs &in)
{
    IndexReads out;
    // The two futures reads take the SYMBOL book — the futures proxies are this app's
    // own instruments, not Yahoo tickers, and reading them out of the ticker book is
    // the defect ReadInputs exists to prevent.
    out.futuresLead = futuresLeadRead(in.bySymbol);
    out.futuresMomentum = futuresMomentumRead(symbol, in.bySymbol);
    out.volatility = volatilityRead(symbol, in.reference);
    out.yields = yieldRead(in.reference);
    out.curve = curveRead(in.reference);
    out.participation = participationRead(symbol, in.reference);
    out.aboveVwap = aboveVwapRead(symbol, in.volume);
    out.upDownVolume = upDownVolumeRead(symbol, in.volume);
    out.structure = structureRead(in.ownSeries);
    return out;
}

// A signed percent in the window's convention: a leading "+" only for positive numbers
// (the "-" comes from the number itself), a trailing "%".
namespace {
QString signedPct(double value)
{
    return QStringLiteral("%1%2%")
        .arg((value > 0.0) ? QStringLiteral("+") : QString())
        .arg(value, 0, 'f', 2);
}
} // namespace

QString HeavyweightPulse::headline() const
{
    if (isEmpty()) {
        return QStringLiteral("%1: no constituent prices yet").arg(indexName);
    }
    return QStringLiteral("%1: %2 of %3 up · average %4 · cap-wt %5 · leader %6 %7 · laggard %8 %9")
        .arg(indexName)
        .arg(up)
        .arg(measured)
        .arg(signedPct(averageChangePct))
        .arg(signedPct(capWeightedChangePct))
        .arg(leader, signedPct(leaderChangePct))
        .arg(laggard, signedPct(laggardChangePct));
}

QString HeavyweightPulse::leadIndicator() const
{
    if (isEmpty()) {
        return QStringLiteral("%1 top-10: no prices yet").arg(indexName);
    }
    // The SIGN of the cap-weighted move is the summarised direction; arrow AND sign carry it.
    const bool upTogether = capWeightedChangePct > 0.0;
    return QStringLiteral("%1 top-10 %2 %3 (%4/%5 up)")
        .arg(indexName)
        .arg(upTogether ? QChar(0x25B2) : QChar(0x25BC))   // ▲ / ▼
        .arg(signedPct(capWeightedChangePct))
        .arg(up)
        .arg(measured);
}

HeavyweightPulse heavyweightPulse(const QString &symbol,
                                  const QHash<QString, QList<double>> &series)
{
    HeavyweightPulse out;
    out.indexName = isNasdaqSymbol(symbol) ? QStringLiteral("Nasdaq-100")
                                           : QStringLiteral("S&P 500");
    double sum = 0.0;
    double weightedSum = 0.0;   // Σ weight · change over the READABLE names…
    double totalWeight = 0.0;   // …divided by this, so absent names do not distort it.
    bool haveExtremes = false;
    for (const QString &ticker : indexHeavyweights(symbol)) {
        HeavyweightRow row;
        row.ticker = ticker;
        const std::optional<double> change = sessionChangePct(series.value(ticker));
        if (change.has_value()) {
            row.known = true;
            row.changePct = *change;
            ++out.measured;
            if (*change > 0.0) {
                ++out.up;
            }
            sum += *change;
            const double weight = heavyweightWeight(symbol, ticker);
            weightedSum += weight * *change;
            totalWeight += weight;
            if (!haveExtremes || (*change > out.leaderChangePct)) {
                out.leader = ticker;
                out.leaderChangePct = *change;
            }
            if (!haveExtremes || (*change < out.laggardChangePct)) {
                out.laggard = ticker;
                out.laggardChangePct = *change;
            }
            haveExtremes = true;
        }
        out.rows.append(row);
    }
    if (out.measured > 0) {
        out.averageChangePct = sum / static_cast<double>(out.measured);
    }
    // Renormalise over the readable weight so a missing megacap does not drag the number
    // toward zero. Falls back to the equal-weight average only if no readable name carried
    // a weight (cannot happen for the current lists, but keeps the number defined).
    out.capWeightedChangePct = (totalWeight > 0.0) ? (weightedSum / totalWeight)
                                                   : out.averageChangePct;
    return out;
}

Confluence confluenceFor(const IndexReads &reads, qint32 dir)
{
    Confluence out;
    if (dir == 0) {
        return out;   // no side to agree with
    }
    const QList<QPair<QString, Read>> all{
        {QStringLiteral("futures lead"), reads.futuresLead},
        {QStringLiteral("futures momentum"), reads.futuresMomentum},
        {QStringLiteral("volatility"), reads.volatility},
        {QStringLiteral("yields"), reads.yields},
        {QStringLiteral("curve"), reads.curve},
        {QStringLiteral("participation"), reads.participation},
        {QStringLiteral("above VWAP"), reads.aboveVwap},
        {QStringLiteral("up/down volume"), reads.upDownVolume},
        {QStringLiteral("structure"), reads.structure},
    };
    for (const auto &[name, read] : all) {
        if (!read.known) {
            ++out.unknown;
            out.reasons << QStringLiteral("%1 unknown").arg(name);
            continue;
        }
        if (read.dir == 0) {
            // Measured and neutral is neither support nor contradiction; it counts as
            // neither, and says so.
            out.reasons << QStringLiteral("%1 neutral (%2)").arg(name, read.detail);
            continue;
        }
        if (read.dir == dir) {
            ++out.met;
            out.reasons << QStringLiteral("%1 agrees (%2)").arg(name, read.detail);
        } else {
            ++out.against;
            out.reasons << QStringLiteral("%1 disagrees (%2)").arg(name, read.detail);
        }
    }
    return out;
}

} // namespace trading
