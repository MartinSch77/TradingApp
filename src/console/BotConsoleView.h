// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_CONSOLE_BOTCONSOLEVIEW_H
#define TRADINGAPP_CONSOLE_BOTCONSOLEVIEW_H

#include "domain/PaperTrader.h"

#include <QList>
#include <QString>
#include <QStringList>

// The bot-console front end's text, prepared as PURE functions so it can be TESTED without a
// terminal (REQ-F-029, the same discipline CockpitModel follows for the QML cockpit).
//
// A console view makes the same kind of claim a GUI does — "invested is 4200 EUR", "NVDA is
// up 1.2%" — and a screenshot of a terminal proves no more than a screenshot of a window.
// So every line the screen shows is produced here, from plain data, and checked by
// tst_botconsoleview. The terminal main does I/O and nothing else: raw-mode keys, ANSI
// positioning, and calling these.
namespace trading::console {

// One heavyweight of an index, and how far it has moved on the session. `changePct` is the
// close-vs-open percentage; `known` is false when its series has not arrived — an absent
// constituent is shown as absent, never as 0.0% (the same absent-is-not-zero rule the whole
// app is built on).
struct HeavyMove {
    QString name;
    double changePct = 0.0;
    bool known = false;
};

// The header: the two numbers the user asked to see at the top, exactly as the GUI shows
// them — booked P/L and money invested — plus the equity and cash that frame them. One line,
// because a header that wraps is no longer a header.
[[nodiscard]] QString consoleHeader(const PaperStats &stats, BotAiMode mode, bool armed);

// The heavyweight bar chart: one row per constituent, `NAME  ▕bar▏  +1.24%`. Direction is in
// the BAR DIRECTION and the signed number, never colour alone — a red/green pair is exactly
// what deuteranopia cannot separate, so the console must read correctly in a monochrome
// terminal too. `width` is the number of cells the bar may fill on each side of centre.
//
// The point of the chart (REQ-F-035): the biggest constituents lead the index, so watching
// the 10-plus megacaps move is a stand-in for a breadth read the app cannot fetch per
// constituent. An absent name is drawn as a dashed placeholder and labelled `—`.
[[nodiscard]] QStringList consoleHeavyBars(const QString &indexLabel,
                                           const QList<HeavyMove> &names, qsizetype width);

// Two indices' heavyweight charts SIDE BY SIDE, so SPX500 and NSDQ100 read as a pair rather
// than one stacked under the other. Each column is consoleHeavyBars; this pads the left column
// to a fixed width and joins them line for line, so a shorter column does not drag the right
// one out of alignment.
[[nodiscard]] QStringList consoleHeavyBarsSideBySide(const QString &labelA,
                                                     const QList<HeavyMove> &namesA,
                                                     const QString &labelB,
                                                     const QList<HeavyMove> &namesB,
                                                     qsizetype width);

// The one-line SUMMARISED constituent-lead indicator the user asked for: the two indices'
// top-ten cap-weighted direction, side by side — "in which direction do the top constituents
// go together, up or down?". The wording of each side is produced by the domain
// (HeavyweightPulse::leadIndicator, so the number and the arrow live in one place); this
// only lays the two strings out as one aligned line with a label, padding the left side to a
// fixed column so the right does not shift when the left grows or shrinks.
[[nodiscard]] QString consoleConstituentLead(const QString &leftIndicator,
                                             const QString &rightIndicator,
                                             qsizetype leftWidth = 40);

// The open book, one STABLE row per position keyed by id, so a trade does not jump rows
// between refreshes (the user asked for a static screen; a table that reshuffles is the
// opposite). Marked P/L and its carry cost so far are shown, because a position that looks
// green on price can be red once the overnight fee is counted.
[[nodiscard]] QStringList consoleOpenTrades(const QList<PaperTrade> &open);

// The closed book, newest first, with the exit rule that closed each — the record is
// decomposed by exit reason elsewhere, and seeing the reason per trade is how a reader
// connects a loss to the rule that caused it.
[[nodiscard]] QStringList consoleClosedTrades(const QList<PaperClosedTrade> &closed,
                                              qsizetype limit);

} // namespace trading::console

#endif // TRADINGAPP_CONSOLE_BOTCONSOLEVIEW_H
