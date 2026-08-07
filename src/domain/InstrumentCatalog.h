// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_INSTRUMENTCATALOG_H
#define TRADINGAPP_DOMAIN_INSTRUMENTCATALOG_H

#include <QList>
#include <QString>
#include <QStringList>

// The single source of truth for the instruments this app trades. Everything
// the code knows statically about an instrument lives HERE — before this
// catalog the same facts were scattered over six tables in four files (the
// selector universe in the UI, the TradingView and Yahoo ticker maps in
// MarketFeeds, the calendar regions in EconomicCalendar, the leverage steps
// and base prices in SimulationEngine), and adding an instrument meant six
// synchronised edits. Now it means one entry.
//
// Live, per-account data (instrument ids, real leverage caps, fees, spreads)
// is deliberately NOT here: that is learned from the API at runtime.
namespace trading {

struct InstrumentSpec {
    QString symbol;            // the eToro app symbol — the key used everywhere
    QString group;             // selector group: "Indices" / "Forex" / "Commodities"
    // Web-feed tickers. Empty = the feed has no usable symbol for it and the
    // consumer shows "n/a" (e.g. RUBBER: SGX TSR20 futures return null ratings).
    QString tradingViewTicker; // rated technical-signal symbol (proxies marked in the entry)
    QString yahooTicker;       // reference quote / intraday series
    // Macro regions whose calendar events tend to move it (TradingView country
    // codes, comma-separated).
    QString calendarRegions;
    // SIMULATION-mode stand-ins; real mode learns both from the API.
    QList<qint32> simLeverage; // leverage steps the simulated account offers
    double simBasePrice = 0.0; // plausible starting price for the synthetic feed
};

// Every tradable instrument, in the order the instrument selector lists them
// (grouped: Indices, then Forex, then Commodities).
[[nodiscard]] const QList<InstrumentSpec> &instrumentCatalog();

// The catalogued symbols, selector order — what setTradableSymbols() consumes.
[[nodiscard]] QStringList tradableSymbols();

// The spec for one symbol, or nullptr for an unknown one. Consumers keep their
// own documented fallbacks (calendar -> "US", simulation -> index defaults).
[[nodiscard]] const InstrumentSpec *instrumentSpec(const QString &symbol);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_INSTRUMENTCATALOG_H
