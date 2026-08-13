// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_POSITIONMATH_H
#define TRADINGAPP_DOMAIN_POSITIONMATH_H

#include "domain/Models.h"

#include <QString>
#include <QHash>
#include <QStringList>

// Money/rate arithmetic for positions and prices. Pure functions shared by the
// trade panel, the open-trades table and the SL/TP editors.
namespace trading {

// Sensible number of decimals for a price, by magnitude: indices in the
// thousands need 2, while low-priced instruments (e.g. forex ~1.08) need more.
[[nodiscard]] qint32 priceDecimals(double price) noexcept;

// Account-currency value of a one-point price move for a position. eToro quotes
// some instruments in a currency other than the account (e.g. HKG50 in HKD), so
// "units x price-distance" is in the *quote* currency — not the account currency,
// and using it directly under-/over-states SL/TP amounts by the FX rate. The
// account-currency notional is amount x leverage, which moves 1:1 with the price
// relative to the open rate, so amount x leverage / openRate is the value per point
// with no FX rate needed. Falls back to units when the notional is unknown (so
// account-currency-quoted instruments still read out).
[[nodiscard]] double accountValuePerPoint(const Position &p);

// A quote older than this no longer marks a position the way eToro's own UI does:
// the public rates feed republishes an open market's price every few seconds, so
// anything beyond this is either a delayed publication or a closed session. Chosen
// above 60 s because a candle-derived mark is stamped with its minute's start.
constexpr qint64 kQuoteStaleMs = 120LL * 1000;

// eToro's own unrealised-P/L identity, verified field by field against its /pnl
// payload for every position of a real account: P/L = units x (close rate - open
// rate) x conversion rate, signed by the side. There is no fee or spread term —
// what a long "pays" for the spread is already inside its open rate, and the mark
// is the bid (see Quote::closeRate).
//
// So matching eToro to the cent is entirely a question of marking at the rate
// eToro marks at. `units` come from the payload; when they are unknown (simulated
// positions) the account-currency value per point stands in, which already carries
// the conversion rate as of the open.
[[nodiscard]] double positionPnl(const Position &p, const Quote &q);

// The account-currency amount a stop-loss / take-profit rate represents for a
// position (value-per-point x price distance from the open rate). Empty when off.
// eurPerUsd converts the account (USD) figure to the display currency; pass 0
// (or a negative value) to show the raw account amount.
[[nodiscard]] QString slTpAmountText(const Position &p, double rate, double eurPerUsd);

// The SIGNED account-currency P/L a stop-loss rate represents: negative when the
// stop closes the trade at a loss (below open for a long, above for a short),
// positive when it closes locked in a profit (stop on the winning side). The sign
// lets the user place the stop on either side. Empty when the leg is off.
[[nodiscard]] QString slSignedAmountText(const Position &p, double eurPerUsd);

// The position ids present in `previous` but gone from `current`: the trades that
// closed between two portfolio snapshots, whoever closed them (this app, eToro's
// own UI, an SL/TP hit, or a liquidation). Order follows `previous`, so the log
// reads in the order the rows were shown. Empty on the first snapshot — with no
// previous set there is nothing to have disappeared.
[[nodiscard]] QStringList closedSincePreviousIds(const QList<Position> &previous,
                                                 const QList<Position> &current);

// What a just-closed position should do to the open-trades table, split three ways.
//
// WHY THIS IS NOT SIMPLY "REMOVE THE ROW". A close is confirmed by the broker's own reply,
// but the open-trades table is driven by the PORTFOLIO poll, and this project has already
// measured that endpoint lagging its own truth by seconds — the same lag that made
// refreshClosedTradesForVanished fetch twice. So deleting the row on the reply is correct
// and insufficient: the very next poll still lists the position, and the row reappears. A
// row that vanishes and comes back is worse than one that lingers, because it reads as a
// close that failed.
//
// So a confirmed close is remembered, and reported positions matching a remembered id are
// hidden. Two things stop that from becoming a lie:
//
//   * The memory is BOUNDED. If the broker is still reporting the position after
//     `windowMs`, the close did not take effect and the row comes BACK, named in `expired`
//     so the caller can say so. Hiding a position forever would tell someone they were flat
//     while they still carried the risk — the most dangerous thing this table can do.
//   * An id the broker has stopped reporting is `confirmed` and dropped from the book, so
//     it cannot grow without limit and cannot hide a later position. (eToro ids are unique
//     per position, so a reopened trade gets a new one and is never suppressed by this.)
struct CloseSuppression {
    QList<Position> visible;     // what the table should show right now
    QStringList expired;         // still reported after the window — shown again, and said
    QStringList confirmed;       // gone from the broker's list — drop from the book
};

// `closedAtMs` maps a position id to when its close was confirmed.
[[nodiscard]] CloseSuppression suppressClosedPositions(const QList<Position> &reported,
                                                       const QHash<QString, qint64> &closedAtMs,
                                                       qint64 nowMs, qint64 windowMs);

// The exposure-cap guard (REQ-F-004): true when adding `newAmount` to what is already
// committed (open trades PLUS resting limit orders — one WILL become exposure the
// moment it triggers, with nobody at the keyboard to be warned then) would push the
// total past `cap`. A small epsilon absorbs floating-point/display rounding, so an
// amount that lands EXACTLY on the cap is not refused for a rounding artefact.
[[nodiscard]] bool exceedsExposureCap(double committedExposure, double newAmount,
                                      double cap) noexcept;

} // namespace trading

#endif // TRADINGAPP_DOMAIN_POSITIONMATH_H
