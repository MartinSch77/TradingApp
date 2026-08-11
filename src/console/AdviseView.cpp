// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AdviseView.h"

namespace trading::console {

namespace {

QStringList compositeLines(const DecisionRow &r)
{
    QStringList lines;
    lines << QStringLiteral("composite: %1 (confidence %2)")
                 .arg(r.dir > 0   ? QStringLiteral("BUY")
                      : r.dir < 0 ? QStringLiteral("SELL")
                                  : QStringLiteral("NEUTRAL"))
                 .arg(r.confidence, 0, 'f', 0);
    lines << (r.haveTech ? QStringLiteral("technical ensemble: %1 (%2)")
                               .arg(r.techLabel)
                               .arg(r.techConf, 0, 'f', 0)
                         : QStringLiteral("technical ensemble: absent"));
    lines << (r.haveRating ? QStringLiteral("web rating: %1").arg(r.rating, 0, 'f', 2)
                           : QStringLiteral("web rating: absent"));
    lines << (r.haveNews ? QStringLiteral("news sentiment: %1 over %2 headline(s)")
                               .arg(r.newsScore, 0, 'f', 2)
                               .arg(r.newsCount)
                         : QStringLiteral("news sentiment: absent"));
    return lines;
}

QString sourceLines(const AdviseInput &in)
{
    QStringList lines;
    if (in.haveRow) {
        lines += compositeLines(in.row);
    }
    lines << (in.vixValid ? QStringLiteral("VIX: %1").arg(in.vix, 0, 'f', 1)
                          : QStringLiteral("VIX: absent"));
    lines << (in.fgValid ? QStringLiteral("Fear & Greed: %1").arg(in.fearGreed, 0, 'f', 0)
                         : QStringLiteral("Fear & Greed: absent"));
    for (const QString &line : in.eventLines) {
        lines << QStringLiteral("calendar: ") + line;
    }
    for (const QString &line : in.readLines) {
        lines << QStringLiteral("read: ") + line;
    }
    for (const QString &line : in.heavyLines) {
        lines << QStringLiteral("constituent: ") + line;
    }
    if (!in.crowdLine.isEmpty()) {
        lines << in.crowdLine;
    }
    if (in.aiAsked) {
        lines << QStringLiteral("local model: ")
                     + (in.aiLine.isEmpty() ? QStringLiteral("no usable answer") : in.aiLine);
    }
    for (const QString &name : in.absentSources) {
        lines << QStringLiteral("absent: %1 (not gathered in time)").arg(name);
    }
    return lines.join(QLatin1Char('\n'));
}

QString planLines(const TradePlan &p, double price)
{
    QString out = QStringLiteral("plan: %1 — P(win) %2%, break-even %3%, risk %4/5\n")
                      .arg(p.verdict)
                      .arg(p.pWin * 100.0, 0, 'f', 0)
                      .arg(p.breakeven * 100.0, 0, 'f', 0)
                      .arg(p.riskFactor);
    out += QStringLiteral("geometry: x%1, stop %2 (−%3), target %4 (+%5)")
               .arg(p.leverage)
               .arg(p.slRate, 0, 'f', 2)
               .arg(p.slAmount, 0, 'f', 2)
               .arg(p.tpRate, 0, 'f', 2)
               .arg(p.tpAmount, 0, 'f', 2);
    if (price > 0.0) {
        out += QStringLiteral(" from %1").arg(price, 0, 'f', 2);
    }
    out += QStringLiteral("\ncosts: open %1 + close %2, %3/night over %4 night(s)%5")
               .arg(p.openCost, 0, 'f', 2)
               .arg(p.closeCost, 0, 'f', 2)
               .arg(p.feePerNight, 0, 'f', 2)
               .arg(p.nights)
               .arg(p.crossesWeekend ? QStringLiteral(" (crosses the weekend)") : QString());
    return out;
}

} // namespace

QString decisionSources(const AdviseInput &in)
{
    QStringList lines;
    // INDICATORS: the deterministic composite of technical ensemble, web rating, news, regime
    // and the intraday/reference reads — the machinery the app has always used.
    if (in.haveRow) {
        const QString dir = in.row.dir > 0   ? QStringLiteral("BUY")
                            : in.row.dir < 0 ? QStringLiteral("SELL")
                                             : QStringLiteral("NEUTRAL");
        lines << QStringLiteral("  INDICATORS say: %1 (composite confidence %2)")
                     .arg(dir)
                     .arg(in.row.confidence, 0, 'f', 0);
    } else {
        lines << QStringLiteral("  INDICATORS say: no reading (no scan row)");
    }
    // AI: the local language model's own pick (a different kind of judgement, shown apart so a
    // reader never mistakes the model's word for the measured composite).
    if (in.aiAsked) {
        lines << QStringLiteral("  AI (local model) says: %1")
                     .arg(in.aiLine.isEmpty() ? QStringLiteral("no usable answer") : in.aiLine);
    } else {
        lines << QStringLiteral("  AI (local model): not consulted");
    }
    // The trained crowd model, when it answered, is a third source and labelled as such.
    if (!in.crowdLine.isEmpty()) {
        lines << QStringLiteral("  AI (crowd model): ") + in.crowdLine;
    }
    return lines.join(QLatin1Char('\n'));
}

AdviseVerdict adviseReport(const AdviseInput &in)
{
    AdviseVerdict out;
    QString head;
    // The verdict comes from the COSTED plan; the composite alone never proposes — a
    // direction that cannot pay its own costs is a "no", however confident it looks.
    const bool actionable = in.havePlan && in.plan.valid
                            && (in.plan.verdict != QLatin1String("STAY OUT"))
                            && (in.plan.dir != 0);
    if (actionable) {
        out.exitCode = 0;
        head = QStringLiteral("PROPOSAL: %1 %2 x%3 — stop %4, target %5 (confidence %6)")
                   .arg(in.plan.verdict, in.symbol)
                   .arg(in.plan.leverage)
                   .arg(in.plan.slRate, 0, 'f', 2)
                   .arg(in.plan.tpRate, 0, 'f', 2)
                   .arg(in.plan.confidence, 0, 'f', 0);
    } else if (in.havePlan && in.plan.valid) {
        out.exitCode = 2;
        head = QStringLiteral("NO TRADE for %1 — %2")
                   .arg(in.symbol, in.plan.verdictReason.isEmpty()
                                       ? QStringLiteral("the plan is not actionable")
                                       : in.plan.verdictReason);
    } else {
        out.exitCode = 3;
        head = QStringLiteral("NOT ENOUGH DATA for %1 — no usable price series arrived; "
                              "the absents below say what is missing")
                   .arg(in.symbol);
    }
    out.text = head + QLatin1Char('\n');
    out.text += QStringLiteral("--- decision sources ---\n") + decisionSources(in)
                + QLatin1Char('\n');
    if (in.havePlan && in.plan.valid) {
        out.text += planLines(in.plan, in.price) + QLatin1Char('\n');
    }
    out.text += QStringLiteral("--- evidence ---\n") + sourceLines(in) + QLatin1Char('\n');
    out.text += QStringLiteral(
        "advisory only: this program links no order path and can place nothing; "
        "signals are experimental, not financial advice.\n");
    return out;
}

} // namespace trading::console
