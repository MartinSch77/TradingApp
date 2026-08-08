// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "console/BotConsoleView.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace trading::console {

namespace {

// A signed EUR figure with an explicit sign, so a gain and a loss differ in the TEXT and not
// only in whatever colour the terminal adds. "+12.40" / "-3.10".
QString signedEur(double value)
{
    return QStringLiteral("%1%2").arg(value >= 0.0 ? QStringLiteral("+") : QString())
        .arg(value, 0, 'f', 2);
}

// A field padded to a fixed width, so columns line up in a monospace terminal. Truncated
// with an ellipsis rather than allowed to push the row out of shape.
QString pad(const QString &text, qsizetype width)
{
    if (text.size() > width) {
        return text.left(std::max<qsizetype>(0, width - 1)) + QChar(u'…');
    }
    return text.leftJustified(width, u' ');
}

} // namespace

QString consoleHeader(const PaperStats &stats, BotAiMode mode, bool armed)
{
    // Booked P/L and invested FIRST — the two the user asked to see at the top — then the
    // equity and cash that give them scale, then the bot's own state so a glance says whether
    // it is even trading. `netByReason` and the full record live a screen down; this is the
    // one-line pulse.
    return QStringLiteral(
               "P/L %1 EUR  ·  invested %2 EUR  ·  equity %3 EUR  ·  cash %4 EUR   │   "
               "%5 open · %6 closed · win %7%   │   AI %8 · %9")
        .arg(signedEur(stats.realized))
        .arg(stats.invested, 0, 'f', 2)
        .arg(stats.equity, 0, 'f', 2)
        .arg(stats.cash, 0, 'f', 2)
        .arg(stats.openTrades)
        .arg(stats.closedTrades)
        .arg(stats.winRate, 0, 'f', 0)
        .arg(botAiModeWord(mode))
        .arg(armed ? QStringLiteral("ARMED") : QStringLiteral("disarmed"));
}

QStringList consoleHeavyBars(const QString &indexLabel, const QList<HeavyMove> &names,
                             qsizetype width)
{
    QStringList out;
    out.append(QStringLiteral("%1 — top constituents (session move; the megacaps lead the "
                              "index)").arg(indexLabel));

    // The full-scale move: whichever known constituent moved furthest, floored at 1% so a
    // dead-flat morning does not blow every tiny wiggle up to full width. Bars are then drawn
    // relative to THIS, so the chart re-scales to the day instead of clipping.
    const double scale = std::accumulate(
        names.cbegin(), names.cend(), 1.0, [](double m, const HeavyMove &h) {
            return h.known ? std::max(m, std::abs(h.changePct)) : m;
        });

    for (const HeavyMove &h : names) {
        if (!h.known) {
            // Absent, not zero: a dashed placeholder and an em dash, never a flat bar that
            // would read as "unchanged".
            out.append(QStringLiteral("  %1 %2  —")
                           .arg(pad(h.name, 6))
                           .arg(QString(2 * width, QChar(u'·'))));
            continue;
        }
        const qsizetype filled =
            std::min(width, static_cast<qsizetype>(std::round(std::abs(h.changePct) / scale * width)));
        // The bar grows LEFT of centre for a fall and RIGHT for a rise, so up and down differ
        // in shape. Centre marked with a bar so the zero is visible even at a glance.
        const QString left = (h.changePct < 0.0)
                                 ? QString(width - filled, u' ') + QString(filled, QChar(u'█'))
                                 : QString(width, u' ');
        const QString right = (h.changePct > 0.0)
                                  ? QString(filled, QChar(u'█')) + QString(width - filled, u' ')
                                  : QString(width, u' ');
        out.append(QStringLiteral("  %1 %2│%3  %4%")
                       .arg(pad(h.name, 6))
                       .arg(left)
                       .arg(right)
                       .arg(signedEur(h.changePct)));
    }
    return out;
}

QStringList consoleHeavyBarsSideBySide(const QString &labelA, const QList<HeavyMove> &namesA,
                                       const QString &labelB, const QList<HeavyMove> &namesB,
                                       qsizetype width)
{
    const QStringList left = consoleHeavyBars(labelA, namesA, width);
    const QStringList right = consoleHeavyBars(labelB, namesB, width);
    // The left column is padded to a fixed cell width so the right column starts at the same
    // place on every row, however many bars each side has. Visible width, not QString::size:
    // the bar glyphs and box-drawing characters are one cell each (all single BMP code
    // points), so size() is the cell count here.
    const qsizetype leftWidth =
        std::accumulate(left.cbegin(), left.cend(), qsizetype{0},
                        [](qsizetype w, const QString &l) { return std::max(w, l.size()); })
        + 4;   // + a gutter between the two columns

    QStringList out;
    const qsizetype rows = std::max(left.size(), right.size());
    for (qsizetype i = 0; i < rows; ++i) {
        const QString l = (i < left.size()) ? left.at(i) : QString();
        const QString r = (i < right.size()) ? right.at(i) : QString();
        out.append(QStringLiteral("  %1%2").arg(l.leftJustified(leftWidth, u' ')).arg(r));
    }
    return out;
}

QStringList consoleOpenTrades(const QList<PaperTrade> &open)
{
    QStringList out;
    // A STABLE order by id, so a position keeps its row between refreshes. The book may
    // return them in any order; sorting by id (the open sequence) makes the screen static,
    // which is what the user asked for.
    QList<PaperTrade> rows = open;
    std::sort(rows.begin(), rows.end(),
              [](const PaperTrade &a, const PaperTrade &b) { return a.id < b.id; });

    out.append(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7")
                   .arg(pad(QStringLiteral("instrument"), 14))
                   .arg(pad(QStringLiteral("side"), 6))
                   .arg(pad(QStringLiteral("stake"), 9))
                   .arg(pad(QStringLiteral("lev"), 4))
                   .arg(pad(QStringLiteral("net"), 10))
                   .arg(pad(QStringLiteral("costs"), 9))
                   .arg(QStringLiteral("opened")));
    if (rows.isEmpty()) {
        out.append(QStringLiteral("  (none open)"));
        return out;
    }
    for (const PaperTrade &t : rows) {
        out.append(
            QStringLiteral("  %1 %2 %3 %4 %5 %6 %7")
                .arg(pad(t.symbol, 14))
                .arg(pad(t.isBuy ? QStringLiteral("▲ BUY") : QStringLiteral("▼ SELL"), 6))
                .arg(pad(QStringLiteral("%1").arg(t.stake, 0, 'f', 0), 9))
                .arg(pad(QStringLiteral("x%1").arg(t.leverage), 4))
                .arg(pad(signedEur(t.netPnl()), 10))
                .arg(pad(QStringLiteral("%1").arg(t.costsSoFar(), 0, 'f', 2), 9))
                .arg(t.openTime.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))));
    }
    return out;
}

QStringList consoleClosedTrades(const QList<PaperClosedTrade> &closed, qsizetype limit)
{
    QStringList out;
    out.append(QStringLiteral("  %1 %2 %3 %4 %5")
                   .arg(pad(QStringLiteral("instrument"), 14))
                   .arg(pad(QStringLiteral("side"), 6))
                   .arg(pad(QStringLiteral("net"), 10))
                   .arg(pad(QStringLiteral("reason"), 16))
                   .arg(QStringLiteral("closed")));
    if (closed.isEmpty()) {
        out.append(QStringLiteral("  (none closed yet)"));
        return out;
    }
    // Newest first: the record is read from the most recent decision backwards.
    QList<PaperClosedTrade> rows = closed;
    std::sort(rows.begin(), rows.end(),
              [](const PaperClosedTrade &a, const PaperClosedTrade &b) {
                  return a.closeTime > b.closeTime;
              });
    const qsizetype shown = (limit > 0) ? std::min(limit, rows.size()) : rows.size();
    for (qsizetype i = 0; i < shown; ++i) {
        const PaperClosedTrade &t = rows.at(i);
        out.append(
            QStringLiteral("  %1 %2 %3 %4 %5")
                .arg(pad(t.symbol, 14))
                .arg(pad(t.isBuy ? QStringLiteral("▲ BUY") : QStringLiteral("▼ SELL"), 6))
                .arg(pad(signedEur(t.netPnl), 10))
                .arg(pad(closeReasonWord(t.reason), 16))
                .arg(t.closeTime.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))));
    }
    return out;
}

} // namespace trading::console
