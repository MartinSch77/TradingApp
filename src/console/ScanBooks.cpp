// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScanBooks.h"

#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"

namespace trading::console {

void wireScanBooks(const EtoroClient &client, const MarketFeeds &feeds, ScanBooks *books,
                   QObject *context)
{
    QObject::connect(&feeds, &MarketFeeds::referenceSeries, context,
                     [books](const QString &ticker, const QList<double> &closes) {
                         books->reference.insert(ticker, closes);
                     });
    QObject::connect(&feeds, &MarketFeeds::referenceVolumeSeries, context,
                     [books](const QString &ticker, const trading::VolumeSeries &bars) {
                         books->refVol.insert(ticker, bars);
                     });
    QObject::connect(&feeds, &MarketFeeds::intradayCloses, context,
                     [books](const QString &symbol, const QList<double> &closes) {
                         books->intraday.insert(symbol, closes);
                     });
    QObject::connect(&feeds, &MarketFeeds::vixUpdated, context,
                     [books](double level, double changePct) {
                         books->vixValid = true;
                         books->vix = level;
                         books->vixChange = changePct;
                     });
    QObject::connect(&feeds, &MarketFeeds::fearGreedUpdated, context,
                     [books](double score, const QString & /*rating*/) {
                         books->fgValid = true;
                         books->fearGreed = score;
                     });
    QObject::connect(&feeds, &MarketFeeds::instrumentRatingsUpdated, context,
                     [books](const QHash<QString, WebRating> &ratingBySymbol) {
                         books->ratings = ratingBySymbol;
                     });
    QObject::connect(&feeds, &MarketFeeds::instrumentNewsUpdated, context,
                     [books](const QString &symbol, const QList<NewsHeadline> &headlines) {
                         books->news.insert(symbol, headlines);
                     });
    QObject::connect(&client, &EtoroClient::screenerRow, context,
                     [books](const ScreenerRow &row) { books->rows.append(row); });
}

trading::MarketSnapshot snapshotFrom(const ScanBooks &books)
{
    trading::MarketSnapshot snap;
    snap.screenerRows = books.rows;
    snap.referenceSeries = books.reference;
    snap.referenceVolumes = books.refVol;
    snap.intradayBySymbol = books.intraday;
    snap.ratingBySymbol = books.ratings;
    snap.newsBySymbol = books.news;
    snap.vixValid = books.vixValid;
    snap.vix = books.vix;
    snap.vixChangePct = books.vixChange;
    snap.fgValid = books.fgValid;
    snap.fearGreed = books.fearGreed;
    return snap;
}

} // namespace trading::console
