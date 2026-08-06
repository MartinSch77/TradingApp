#include "domain/IndexConfluence.h"

#include "domain/DecisionEngine.h"

#include <algorithm>
#include <cmath>
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

QStringList referenceTickers()
{
    QStringList out{QStringLiteral("^VIX"), QStringLiteral("^VXN"), QStringLiteral("^TNX")};
    // The union of both heavyweight lists, in a stable order and without duplicates:
    // the sweep fetches every name once, and each index's read picks its own subset.
    for (const QString &name : nasdaqHeavyweightNames() + spHeavyweightNames()) {
        if (!out.contains(name)) {
            out.append(name);
        }
    }
    return out;
}

IndexReads indexReads(const QString &symbol, const QHash<QString, QList<double>> &referenceSeries,
                      const QList<double> &ownSeries)
{
    IndexReads out;
    out.futuresLead = futuresLeadRead(referenceSeries);
    out.volatility = volatilityRead(symbol, referenceSeries);
    out.yields = yieldRead(referenceSeries);
    out.participation = participationRead(symbol, referenceSeries);
    out.structure = structureRead(ownSeries);
    return out;
}

QString HeavyweightPulse::headline() const
{
    if (isEmpty()) {
        return QStringLiteral("%1: no constituent prices yet").arg(indexName);
    }
    const QString direction = (averageChangePct > 0.0) ? QStringLiteral("+") : QString();
    return QStringLiteral("%1: %2 of %3 up · average %4%5% · leader %6 %7%8% · laggard %9 %10%")
        .arg(indexName)
        .arg(up)
        .arg(measured)
        .arg(direction)
        .arg(averageChangePct, 0, 'f', 2)
        .arg(leader, (leaderChangePct > 0.0) ? QStringLiteral("+") : QString())
        .arg(leaderChangePct, 0, 'f', 2)
        .arg(laggard)
        .arg(laggardChangePct, 0, 'f', 2);
}

HeavyweightPulse heavyweightPulse(const QString &symbol,
                                  const QHash<QString, QList<double>> &series)
{
    HeavyweightPulse out;
    out.indexName = isNasdaqSymbol(symbol) ? QStringLiteral("Nasdaq-100")
                                           : QStringLiteral("S&P 500");
    double sum = 0.0;
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
        {QStringLiteral("volatility"), reads.volatility},
        {QStringLiteral("yields"), reads.yields},
        {QStringLiteral("participation"), reads.participation},
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
