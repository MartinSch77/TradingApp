// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PortfolioReport.h"

#include "domain/PaperTrader.h"

#include <algorithm>
#include <numeric>

namespace trading::console {

namespace {

QString num(double v, int precision = 2)
{
    return QString::number(v, 'f', precision);
}

// Invested cash per correlation bucket — the concentration the recommendation reads.
QHash<QString, double> investedByBucket(const QList<HoldingSignal> &holdings)
{
    QHash<QString, double> by;
    for (const HoldingSignal &h : holdings) {
        by[trading::correlationGroup(h.position.symbol)] += h.position.amount;
    }
    return by;
}

double totalInvested(const QList<HoldingSignal> &holdings)
{
    return std::accumulate(
        holdings.cbegin(), holdings.cend(), 0.0,
        [](double acc, const HoldingSignal &h) { return acc + h.position.amount; });
}

// What to do with ONE holding, and why — from the signal's side vs the position's side and
// the bucket concentration. No signal is an explicit "hold — not analysable", never a guess.
QPair<QString, QString> recommendation(const HoldingSignal &h, double bucketShare)
{
    const qint32 held = h.position.isBuy ? 1 : -1;
    if (!h.haveSignal) {
        return {QStringLiteral("hold (no signal)"),
                h.note.isEmpty() ? QStringLiteral("no data feed for this instrument") : h.note};
    }
    const qint32 sig = h.row.dir;
    const QString conf = QStringLiteral("signal confidence %1").arg(h.row.confidence, 0, 'f', 0);
    if (sig == 0) {
        return {QStringLiteral("HOLD"), QStringLiteral("no directional signal (%1)").arg(conf)};
    }
    if (sig == held) {
        // The signal agrees with the position: add only if the bucket is not already crowded.
        const QString action = (bucketShare > 0.35) ? QStringLiteral("HOLD")
                                                     : QStringLiteral("HOLD / ADD");
        const QString crowded =
            (bucketShare > 0.35)
                ? QStringLiteral("; bucket already %1% of invested — no add")
                      .arg(bucketShare * 100.0, 0, 'f', 0)
                : QString();
        return {action, QStringLiteral("signal supports your %1 (%2)%3")
                            .arg(h.position.isBuy ? QStringLiteral("long")
                                                  : QStringLiteral("short"),
                                 conf, crowded)};
    }
    return {QStringLiteral("REDUCE / EXIT"),
            QStringLiteral("signal opposes your %1 (%2)")
                .arg(h.position.isBuy ? QStringLiteral("long") : QStringLiteral("short"), conf)};
}

// Signal-vs-position urgency: against < neutral/for < no-signal, so exits sort to the top.
int urgency(const HoldingSignal &h)
{
    if (!h.haveSignal) {
        return 2;
    }
    const qint32 held = h.position.isBuy ? 1 : -1;
    return ((h.row.dir != 0) && (h.row.dir != held)) ? 0 : 1;
}

Sheet holdingsSheet(const PortfolioReportInput &in)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Holdings");
    sheet.rows.append({QStringLiteral("instrument"), QStringLiteral("side"),
                       QStringLiteral("invested"), QStringLiteral("P/L"),
                       QStringLiteral("signal"), QStringLiteral("recommendation"),
                       QStringLiteral("why"), QStringLiteral("bucket")});
    const QHash<QString, double> buckets = investedByBucket(in.holdings);
    const double invested = totalInvested(in.holdings);
    QList<HoldingSignal> ordered = in.holdings;   // worst-first: exits/reductions at the top
    std::sort(ordered.begin(), ordered.end(),
              [](const HoldingSignal &a, const HoldingSignal &b) {
                  return urgency(a) < urgency(b);
              });
    for (const HoldingSignal &h : ordered) {
        const QString bucket = trading::correlationGroup(h.position.symbol);
        const double share = (invested > 0.0) ? (buckets.value(bucket) / invested) : 0.0;
        const QPair<QString, QString> rec = recommendation(h, share);
        const QString signal =
            h.haveSignal ? QStringLiteral("%1 (%2)")
                               .arg(h.row.dir > 0   ? QStringLiteral("BUY")
                                    : h.row.dir < 0 ? QStringLiteral("SELL")
                                                    : QStringLiteral("NEUTRAL"))
                               .arg(num(h.row.confidence, 0))
                         : QStringLiteral("no signal");
        // /portfolio carries no P/L; show it only when an API figure was overlaid, else n/a
        // rather than a misleading 0.00.
        const QString pl = h.position.profitFromApi ? num(h.position.profit)
                                                    : QStringLiteral("n/a");
        sheet.rows.append({h.position.symbol,
                           h.position.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                           num(h.position.amount), pl, signal, rec.first, rec.second, bucket});
    }
    return sheet;
}

Sheet evidenceSheet(const PortfolioReportInput &in)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Evidence");
    sheet.rows.append({QStringLiteral("instrument"), QStringLiteral("source"),
                       QStringLiteral("reading")});
    for (const HoldingSignal &h : in.holdings) {
        if (!h.haveSignal) {
            sheet.rows.append({h.position.symbol, QStringLiteral("signal"),
                               h.note.isEmpty() ? QStringLiteral("no data feed") : h.note});
            continue;
        }
        const DecisionRow &r = h.row;
        const auto add = [&](const char *source, const QString &reading) {
            sheet.rows.append({h.position.symbol, QLatin1String(source), reading});
        };
        add("composite", num(r.composite) + QStringLiteral(" (confidence ")
                             + num(r.confidence, 0) + QLatin1Char(')'));
        add("technical", r.haveTech ? r.techLabel + QStringLiteral(" (") + num(r.techConf, 0)
                                          + QLatin1Char(')')
                                    : QStringLiteral("absent"));
        add("web rating", r.haveRating ? num(r.rating) : QStringLiteral("absent (no feed)"));
        add("news", r.haveNews ? num(r.newsScore) + QStringLiteral(" over ")
                                     + QString::number(r.newsCount) + QStringLiteral(" headline(s)")
                               : QStringLiteral("absent (no feed)"));
        if (h.havePlan) {
            add("plan", h.plan.verdict + QStringLiteral(", P(win) ")
                            + num(h.plan.pWin * 100.0, 0) + QStringLiteral("%"));
        }
    }
    return sheet;
}

Sheet concentrationSheet(const PortfolioReportInput &in)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Concentration");
    if (!in.portfolioKnown) {
        sheet.rows.append(
            {QStringLiteral("portfolio unavailable (no credentials or not delivered in time)")});
        return sheet;
    }
    sheet.rows.append({QStringLiteral("bucket"), QStringLiteral("invested"),
                       QStringLiteral("share %")});
    const QHash<QString, double> buckets = investedByBucket(in.holdings);
    const double invested = totalInvested(in.holdings);
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        sheet.rows.append({it.key(), num(it.value()),
                           num((invested > 0.0) ? (it.value() / invested * 100.0) : 0.0, 0)});
    }
    sheet.rows.append({QStringLiteral("cash"), num(in.cash), in.currency});
    return sheet;
}

Sheet healthSheet(const PortfolioReportInput &in)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Data health");
    sheet.rows.append({QStringLiteral("generated"), in.generatedAt});
    sheet.rows.append({QStringLiteral("holdings"), QString::number(in.holdings.size())});
    const auto withSignal = std::count_if(in.holdings.cbegin(), in.holdings.cend(),
                                          [](const HoldingSignal &h) { return h.haveSignal; });
    sheet.rows.append({QStringLiteral("with a signal"), QString::number(withSignal)});
    sheet.rows.append({QStringLiteral("no data feed"),
                       QString::number(in.holdings.size() - withSignal)});
    for (const QString &absent : in.absentSources) {
        sheet.rows.append({QStringLiteral("absent"), absent});
    }
    sheet.rows.append({QStringLiteral("note"),
                       QStringLiteral("advisory only — this program links no order path and can "
                                      "place nothing; signals are experimental, not financial "
                                      "advice")});
    return sheet;
}

} // namespace

QList<Sheet> portfolioReportSheets(const PortfolioReportInput &in)
{
    return {holdingsSheet(in), evidenceSheet(in), concentrationSheet(in), healthSheet(in)};
}

} // namespace trading::console
