// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PortfolioReport.h"

#include "domain/PaperTrader.h"

#include <algorithm>

namespace trading::console {

namespace {

QString num(double v, int precision = 2)
{
    return QString::number(v, 'f', precision);
}

// Invested cash per correlation bucket — exposure the next buy must respect. Deliberately
// the INVESTED amount, not loss-at-stop: real positions carry no simulated stop geometry,
// and inventing one would dress a guess as arithmetic.
QHash<QString, double> investedByBucket(const QList<Position> &positions)
{
    QHash<QString, double> by;
    for (const Position &p : positions) {
        by[trading::correlationGroup(p.symbol)] += p.amount;
    }
    return by;
}

struct Ranked {
    PortfolioCandidate candidate;
    double score = 0.0;
    QString constraint;   // the named reason a concentrated bucket demoted it, or empty
};

QList<Ranked> ranked(const PortfolioReportInput &in)
{
    const QHash<QString, double> buckets = investedByBucket(in.positions);
    double invested = 0.0;
    for (const Position &p : in.positions) {
        invested += p.amount;
    }
    QList<Ranked> out;
    for (const PortfolioCandidate &c : in.candidates) {
        Ranked r;
        r.candidate = c;
        r.score = c.plan.confidence;
        const QString bucket = trading::correlationGroup(c.symbol);
        const double share = (invested > 0.0) ? (buckets.value(bucket) / invested) : 0.0;
        // Concentration demotes, it does not forbid: the proposal is advisory, and the
        // reader sees WHY the ranking moved — a dozen positions in one bucket are one bet.
        if (share > 0.35) {
            r.score -= 25.0;
            r.constraint = QStringLiteral("bucket '%1' already holds %2% of invested money")
                               .arg(bucket)
                               .arg(share * 100.0, 0, 'f', 0);
        } else if (share > 0.20) {
            r.score -= 10.0;
            r.constraint = QStringLiteral("bucket '%1' holds %2% of invested money")
                               .arg(bucket)
                               .arg(share * 100.0, 0, 'f', 0);
        }
        out.append(r);
    }
    std::sort(out.begin(), out.end(),
              [](const Ranked &a, const Ranked &b) { return a.score > b.score; });
    return out;
}

Sheet proposalSheet(const PortfolioReportInput &in, const QList<Ranked> &list)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Proposal");
    sheet.rows.append({QStringLiteral("rank"), QStringLiteral("instrument"),
                       QStringLiteral("side"), QStringLiteral("confidence"),
                       QStringLiteral("leverage"), QStringLiteral("stop"),
                       QStringLiteral("target"), QStringLiteral("suggested stake (advisory)"),
                       QStringLiteral("P(win) %"), QStringLiteral("bucket"),
                       QStringLiteral("constraint"), QStringLiteral("why")});
    // Equity proxy for the stake suggestion: cash plus what is already invested. Advisory
    // numbers, bounded by the cash that actually exists.
    double invested = 0.0;
    for (const Position &p : in.positions) {
        invested += p.amount;
    }
    const double equity = in.cash + invested;
    qint32 rank = 1;
    for (const Ranked &r : list) {
        const TradePlan &p = r.candidate.plan;
        const double stake = std::min(equity * 0.06, in.cash);
        const QString why = QStringLiteral("composite %1; tech %2; rating %3; news %4")
                                .arg(num(r.candidate.row.composite),
                                     r.candidate.row.haveTech ? r.candidate.row.techLabel
                                                              : QStringLiteral("absent"),
                                     r.candidate.row.haveRating
                                         ? num(r.candidate.row.rating)
                                         : QStringLiteral("absent"),
                                     r.candidate.row.haveNews ? num(r.candidate.row.newsScore)
                                                              : QStringLiteral("absent"));
        sheet.rows.append({QString::number(rank++), r.candidate.symbol, p.verdict,
                           num(p.confidence, 0), QString::number(p.leverage), num(p.slRate),
                           num(p.tpRate), num(stake), num(p.pWin * 100.0, 0),
                           trading::correlationGroup(r.candidate.symbol), r.constraint, why});
    }
    return sheet;
}

Sheet evidenceSheet(const QList<Ranked> &list)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Evidence");
    sheet.rows.append({QStringLiteral("instrument"), QStringLiteral("source"),
                       QStringLiteral("reading")});
    for (const Ranked &r : list) {
        const DecisionRow &row = r.candidate.row;
        const auto add = [&](const char *source, const QString &reading) {
            sheet.rows.append({r.candidate.symbol, QLatin1String(source), reading});
        };
        add("composite", num(row.composite) + QStringLiteral(" (confidence ")
                             + num(row.confidence, 0) + QLatin1Char(')'));
        add("technical", row.haveTech ? row.techLabel + QStringLiteral(" (")
                                            + num(row.techConf, 0) + QLatin1Char(')')
                                      : QStringLiteral("absent"));
        add("web rating", row.haveRating ? num(row.rating) : QStringLiteral("absent"));
        add("news", row.haveNews ? num(row.newsScore) + QStringLiteral(" over ")
                                       + QString::number(row.newsCount)
                                       + QStringLiteral(" headline(s)")
                                 : QStringLiteral("absent"));
        add("plan verdict", r.candidate.plan.verdict
                                + (r.candidate.plan.verdictReason.isEmpty()
                                       ? QString()
                                       : QStringLiteral(" — ")
                                             + r.candidate.plan.verdictReason));
        add("plan costs",
            QStringLiteral("open %1 + close %2, %3/night x %4")
                .arg(num(r.candidate.plan.openCost), num(r.candidate.plan.closeCost),
                     num(r.candidate.plan.feePerNight),
                     QString::number(r.candidate.plan.nights)));
    }
    return sheet;
}

Sheet portfolioSheet(const PortfolioReportInput &in)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Portfolio");
    if (!in.portfolioKnown) {
        sheet.rows.append({QStringLiteral(
            "portfolio unavailable (no credentials or not delivered in time) — the proposal "
            "ranks over a FLAT book and no concentration constraint could be applied")});
        return sheet;
    }
    sheet.rows.append({QStringLiteral("instrument"), QStringLiteral("side"),
                       QStringLiteral("invested"), QStringLiteral("leverage"),
                       QStringLiteral("P/L"), QStringLiteral("bucket")});
    for (const Position &p : in.positions) {
        sheet.rows.append({p.symbol,
                           p.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                           num(p.amount), num(p.leverage, 0), num(p.profit),
                           trading::correlationGroup(p.symbol)});
    }
    sheet.rows.append({QStringLiteral("cash"), QString(), num(in.cash), QString(), QString(),
                       in.currency});
    const QHash<QString, double> buckets = investedByBucket(in.positions);
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        sheet.rows.append({QStringLiteral("bucket: ") + it.key(), QString(), num(it.value())});
    }
    return sheet;
}

Sheet consideredSheet(const PortfolioReportInput &in)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Considered");
    sheet.rows.append({QStringLiteral("instrument"), QStringLiteral("status"),
                       QStringLiteral("detail")});
    for (const QStringList &row : in.considered) {
        sheet.rows.append(row);
    }
    return sheet;
}

Sheet healthSheet(const PortfolioReportInput &in)
{
    Sheet sheet;
    sheet.name = QStringLiteral("Data health");
    sheet.rows.append({QStringLiteral("generated"), in.generatedAt});
    sheet.rows.append({QStringLiteral("candidates evaluated"),
                       QString::number(in.candidates.size())});
    for (const QString &absent : in.absentSources) {
        sheet.rows.append({QStringLiteral("absent"), absent});
    }
    sheet.rows.append({QStringLiteral("note"),
                       QStringLiteral("advisory only — this program links no order path and "
                                      "can place nothing; signals are experimental, not "
                                      "financial advice")});
    return sheet;
}

} // namespace

QList<Sheet> portfolioReportSheets(const PortfolioReportInput &in)
{
    const QList<Ranked> list = ranked(in);
    return {proposalSheet(in, list), consideredSheet(in), evidenceSheet(list),
            portfolioSheet(in), healthSheet(in)};
}

} // namespace trading::console
