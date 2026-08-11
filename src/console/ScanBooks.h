// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_CONSOLE_SCANBOOKS_H
#define TRADINGAPP_CONSOLE_SCANBOOKS_H

#include "domain/DecisionEngine.h"
#include "domain/Models.h"

#include <QHash>
#include <QList>
#include <QObject>

class EtoroClient;
class MarketFeeds;

// The one console-side collector for everything a MarketSnapshot is built from — shared by
// the bot console and the advise console, so the gather exists ONCE (the alternative is two
// drifting copies of the same connect block). Plain data plus two functions: wire the signals
// into the books, and turn the books into the SAME MarketSnapshot the GUI builds.
namespace trading::console {

struct ScanBooks {
    QList<ScreenerRow> rows;                        // the scan's per-instrument candles
    QHash<QString, QList<double>> reference;        // reference series per Yahoo ticker
    QHash<QString, trading::VolumeSeries> refVol;   // reference volume bars per ticker
    QHash<QString, QList<double>> intraday;         // 1-minute series per app symbol
    QHash<QString, WebRating> ratings;              // web rating per app symbol
    QHash<QString, QList<NewsHeadline>> news;       // headlines per app symbol
    bool vixValid = false;
    double vix = 0.0;
    double vixChange = 0.0;
    bool fgValid = false;
    double fearGreed = 50.0;
};

// Connect every feed signal into `books` (owned by the caller, outliving `context`).
void wireScanBooks(const EtoroClient &client, const MarketFeeds &feeds, ScanBooks *books,
                   QObject *context);

// The books as the GUI's MarketSnapshot — absent stays absent, nothing is zeroed.
[[nodiscard]] trading::MarketSnapshot snapshotFrom(const ScanBooks &books);

} // namespace trading::console

#endif // TRADINGAPP_CONSOLE_SCANBOOKS_H
