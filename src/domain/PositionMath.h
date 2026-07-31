#ifndef TRADINGAPP_DOMAIN_POSITIONMATH_H
#define TRADINGAPP_DOMAIN_POSITIONMATH_H

#include "domain/Models.h"

#include <QString>
#include <QStringList>

// Money/rate arithmetic for positions and prices. Pure functions shared by the
// trade panel, the open-trades table and the SL/TP editors.
namespace trading {

// Sensible number of decimals for a price, by magnitude: indices in the
// thousands need 2, while low-priced instruments (e.g. forex ~1.08) need more.
qint32 priceDecimals(double price);

// Account-currency value of a one-point price move for a position. eToro quotes
// some instruments in a currency other than the account (e.g. HKG50 in HKD), so
// "units x price-distance" is in the *quote* currency — not the account currency,
// and using it directly under-/over-states SL/TP amounts by the FX rate. The
// account-currency notional is amount x leverage, which moves 1:1 with the price
// relative to the open rate, so amount x leverage / openRate is the value per point
// with no FX rate needed. Falls back to units when the notional is unknown (so
// account-currency-quoted instruments still read out).
double accountValuePerPoint(const Position &p);

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
double positionPnl(const Position &p, const Quote &q);

// The account-currency amount a stop-loss / take-profit rate represents for a
// position (value-per-point x price distance from the open rate). Empty when off.
// eurPerUsd converts the account (USD) figure to the display currency; pass 0
// (or a negative value) to show the raw account amount.
QString slTpAmountText(const Position &p, double rate, double eurPerUsd);

// The SIGNED account-currency P/L a stop-loss rate represents: negative when the
// stop closes the trade at a loss (below open for a long, above for a short),
// positive when it closes locked in a profit (stop on the winning side). The sign
// lets the user place the stop on either side. Empty when the leg is off.
QString slSignedAmountText(const Position &p, double eurPerUsd);

// The position ids present in `previous` but gone from `current`: the trades that
// closed between two portfolio snapshots, whoever closed them (this app, eToro's
// own UI, an SL/TP hit, or a liquidation). Order follows `previous`, so the log
// reads in the order the rows were shown. Empty on the first snapshot — with no
// previous set there is nothing to have disappeared.
QStringList closedSincePreviousIds(const QList<Position> &previous,
                                   const QList<Position> &current);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_POSITIONMATH_H
