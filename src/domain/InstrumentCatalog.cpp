// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/InstrumentCatalog.h"

#include <QHash>

#include <algorithm>

namespace trading {

// One entry per instrument. Ticker provenance (kept from the original
// MarketFeeds maps, where every entry was verified against the live feeds):
//  * TradingView tickers confirmed working via the scanner (non-null
//    Recommend.All). Thematic eToro baskets map to the closest liquid
//    ETF/index proxy — an advisory stand-in, not the basket itself; those are
//    marked "proxy". RUBBER has no rated symbol (SGX TSR20 futures return
//    null ratings) -> empty = "n/a".
//  * Yahoo tickers: cash indices for the standard listings; the 24/7 variants
//    map to the futures contract, whose round-the-clock pricing is what the
//    CFD tracks. Empty = no usable reference quote.
const QList<InstrumentSpec> &instrumentCatalog()
{
    static const QList<InstrumentSpec> kCatalog = {
        // --- Indices --------------------------------------------------------
        {QStringLiteral("SPX500"), QStringLiteral("Indices"),
         QStringLiteral("SP:SPX"), QStringLiteral("^GSPC"),
         QStringLiteral("US"), {1, 2, 5, 10, 20}, 5800.0},
        {QStringLiteral("SP.24-7"), QStringLiteral("Indices"),
         QStringLiteral("SP:SPX"), QStringLiteral("ES=F"),
         QStringLiteral("US"), {1, 2, 5, 20}, 5800.0},
        {QStringLiteral("USDOLLAR"), QStringLiteral("Indices"),
         QStringLiteral("TVC:DXY"), QStringLiteral("DX-Y.NYB"),
         QStringLiteral("US,EU"), {1, 2, 5, 10, 20}, 104.0},
        {QStringLiteral("NSDQ100"), QStringLiteral("Indices"),
         QStringLiteral("NASDAQ:NDX"), QStringLiteral("^NDX"),
         QStringLiteral("US"), {1, 2, 5, 10, 20}, 20500.0},
        {QStringLiteral("DJ30"), QStringLiteral("Indices"),
         QStringLiteral("OANDA:US30USD"), QStringLiteral("^DJI"),
         QStringLiteral("US"), {1, 2, 5, 10, 20}, 42000.0},
        {QStringLiteral("GER40"), QStringLiteral("Indices"),
         QStringLiteral("XETR:DAX"), QStringLiteral("^GDAXI"),
         QStringLiteral("DE,EU"), {1, 2, 5, 10, 20}, 18500.0},
        {QStringLiteral("HKG50"), QStringLiteral("Indices"),
         QStringLiteral("TVC:HSI"), QStringLiteral("^HSI"),
         QStringLiteral("HK,CN"), {1, 2, 5, 10, 20}, 18000.0},
        {QStringLiteral("CHINA50"), QStringLiteral("Indices"),
         QStringLiteral("HKEX:2823"), QString(),  // proxy: iShares FTSE A50 ETF
         QStringLiteral("CN"), {1, 2, 5, 10}, 12500.0},
        {QStringLiteral("EUSTX50"), QStringLiteral("Indices"),
         QStringLiteral("TVC:SX5E"), QStringLiteral("^STOXX50E"),
         QStringLiteral("EU,DE,FR"), {1, 2, 5, 10, 20}, 4900.0},
        {QStringLiteral("RTY"), QStringLiteral("Indices"),
         QStringLiteral("TVC:RUT"), QStringLiteral("^RUT"),
         QStringLiteral("US"), {1, 2, 5, 10, 20}, 2200.0},
        {QStringLiteral("Switzerland20"), QStringLiteral("Indices"),
         QStringLiteral("SIX:SMI"), QStringLiteral("^SSMI"),
         QStringLiteral("CH,EU"), {1, 2, 5, 10, 20}, 12000.0},
        {QStringLiteral("Semiconductors"), QStringLiteral("Indices"),
         QStringLiteral("NASDAQ:SOX"), QString(),  // proxy: PHLX semiconductor index
         QStringLiteral("US"), {1, 2}, 250.0},
        {QStringLiteral("AI.Leaders"), QStringLiteral("Indices"),
         QStringLiteral("NASDAQ:AIQ"), QString(),  // proxy: Global X AI ETF
         QStringLiteral("US"), {1, 2}, 150.0},
        {QStringLiteral("Cybersecurity"), QStringLiteral("Indices"),
         QStringLiteral("NASDAQ:CIBR"), QString(),  // proxy: First Trust cyber ETF
         QStringLiteral("US"), {1, 2}, 60.0},
        {QStringLiteral("Quantum"), QStringLiteral("Indices"),
         QStringLiteral("NASDAQ:QTUM"), QString(),  // proxy: Defiance quantum ETF
         QStringLiteral("US"), {1, 2}, 40.0},
        {QStringLiteral("GoldMiners"), QStringLiteral("Indices"),
         QStringLiteral("AMEX:GDX"), QString(),  // proxy: VanEck gold miners ETF
         QStringLiteral("US"), {1, 2}, 35.0},
        {QStringLiteral("Crypto10"), QStringLiteral("Indices"),
         QStringLiteral("CRYPTOCAP:TOTAL"), QString(),  // proxy: total crypto market cap
         QStringLiteral("US"), {1, 2}, 2500.0},
        {QStringLiteral("Canada60"), QStringLiteral("Indices"),
         QStringLiteral("TSX:TX60"), QString(),  // S&P/TSX 60 index
         QStringLiteral("CA,US"), {1, 2, 5, 10}, 1300.0},
        {QStringLiteral("Sweden30"), QStringLiteral("Indices"),
         QStringLiteral("OMXSTO:OMXS30"), QString(),  // OMX Stockholm 30
         QStringLiteral("SE,EU"), {1, 2, 5, 10}, 2500.0},
        {QStringLiteral("NSDQ100.24-7"), QStringLiteral("Indices"),
         QStringLiteral("NASDAQ:NDX"), QStringLiteral("NQ=F"),
         QStringLiteral("US"), {1, 2, 5, 20}, 20500.0},
        {QStringLiteral("Nuclear"), QStringLiteral("Indices"),
         QStringLiteral("AMEX:NLR"), QString(),  // proxy: VanEck uranium+nuclear ETF
         QStringLiteral("US"), {1, 2}, 45.0},
        {QStringLiteral("Colombia"), QStringLiteral("Indices"),
         QStringLiteral("BVC:ICOLCAP"), QString(),  // proxy: iShares COLCAP ETF
         QStringLiteral("CO,US"), {1, 2, 5}, 30.0},
        // --- Forex ----------------------------------------------------------
        {QStringLiteral("EURUSD"), QStringLiteral("Forex"),
         QStringLiteral("FX:EURUSD"), QStringLiteral("EURUSD=X"),
         QStringLiteral("EU,US"), {1, 2, 5, 10, 20, 30}, 1.08},
        // --- Commodities ----------------------------------------------------
        {QStringLiteral("RUBBER"), QStringLiteral("Commodities"),
         QString(), QString(),  // no rated web symbol, no Yahoo quote
         QStringLiteral("CN,US"), {1, 2, 5, 10}, 170.0},
        {QStringLiteral("Gold.24-7"), QStringLiteral("Commodities"),
         QStringLiteral("TVC:GOLD"), QStringLiteral("GC=F"),
         QStringLiteral("US"), {1, 2, 5, 20}, 2400.0},
        {QStringLiteral("OIL.24-7"), QStringLiteral("Commodities"),
         QStringLiteral("FX:USOIL"), QStringLiteral("CL=F"),  // WTI CFD stream
         QStringLiteral("US"), {1, 2, 5, 10}, 78.0},

        // --- Crypto ---------------------------------------------------------
        // eToro's own names are the bare tickers (BTC, ETH, SOL, XRP) — the local model tends to
        // answer with the exchange pair (BTCUSDT, ETH-USD), which matchProposalSymbol now
        // maps back. Leverage is {1, 2} because eToro caps retail crypto CFDs at x2, and the
        // crypto correlation bucket + a 1% cost floor (see PaperTrader) reflect the rest of
        // the real economics. Yahoo quotes crypto as BTC-USD; TradingView as the Coinbase
        // spot pair.
        {QStringLiteral("BTC"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:BTCUSD"), QStringLiteral("BTC-USD"),
         QStringLiteral("US"), {1, 2}, 68000.0},
        {QStringLiteral("ETH"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:ETHUSD"), QStringLiteral("ETH-USD"),
         QStringLiteral("US"), {1, 2}, 2600.0},
        {QStringLiteral("SOL"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:SOLUSD"), QStringLiteral("SOL-USD"),
         QStringLiteral("US"), {1, 2}, 145.0},
        {QStringLiteral("XRP"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:XRPUSD"), QStringLiteral("XRP-USD"),
         QStringLiteral("US"), {1, 2}, 0.55},
    };
    return kCatalog;
}

QStringList tradableSymbols()
{
    QStringList symbols;
    const QList<InstrumentSpec> &catalog = instrumentCatalog();
    symbols.reserve(catalog.size());
    for (const InstrumentSpec &spec : catalog) {
        symbols << spec.symbol;
    }
    return symbols;
}

const InstrumentSpec *instrumentSpec(const QString &symbol)
{
    // Index built once: the catalog is immutable and lookups run per poll tick.
    static const QHash<QString, const InstrumentSpec *> kBySymbol = [] {
        QHash<QString, const InstrumentSpec *> map;
        const QList<InstrumentSpec> &catalog = instrumentCatalog();
        map.reserve(catalog.size());
        for (const InstrumentSpec &spec : catalog) {
            map.insert(spec.symbol, &spec);
        }
        return map;
    }();
    return kBySymbol.value(symbol, nullptr);
}

} // namespace trading
