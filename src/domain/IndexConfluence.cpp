#include "domain/IndexConfluence.h"

#include "domain/DecisionEngine.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace trading {

namespace {

// The eight names that carry most of the Nasdaq-100's weight. A fixed list is
// deliberate: it documents what the participation read covers, and it changes on the
// order of years rather than of scans.
QStringList heavyweightNames()
{
    return {QStringLiteral("MSFT"), QStringLiteral("NVDA"), QStringLiteral("AAPL"),
            QStringLiteral("AMZN"), QStringLiteral("GOOGL"), QStringLiteral("META"),
            QStringLiteral("AVGO"), QStringLiteral("TSLA")};
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
Read participationRead(const QHash<QString, QList<double>> &series)
{
    Read out;
    const QPair<qint32, qint32> up = upCount(series, heavyweightNames());
    if (up.second < 4) {
        return out;   // fewer than half the names read is not a participation read
    }
    out.known = true;
    const double share = static_cast<double>(up.first) / static_cast<double>(up.second);
    out.dir = (share >= 0.625) ? 1 : ((share <= 0.375) ? -1 : 0);
    out.detail = QStringLiteral("%1 of %2 heavyweights up").arg(up.first).arg(up.second);
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
    return heavyweightNames();
}

QStringList referenceTickers()
{
    QStringList out{QStringLiteral("^VIX"), QStringLiteral("^VXN"), QStringLiteral("^TNX")};
    out += nasdaqHeavyweights();
    return out;
}

IndexReads indexReads(const QString &symbol, const QHash<QString, QList<double>> &referenceSeries,
                      const QList<double> &ownSeries)
{
    IndexReads out;
    out.futuresLead = futuresLeadRead(referenceSeries);
    out.volatility = volatilityRead(symbol, referenceSeries);
    out.yields = yieldRead(referenceSeries);
    out.participation = participationRead(referenceSeries);
    out.structure = structureRead(ownSeries);
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
