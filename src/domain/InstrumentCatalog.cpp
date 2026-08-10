// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/InstrumentCatalog.h"

#include <QHash>

#include <algorithm>

namespace trading {

// The crypto slice of the catalog, in its OWN function so the flat instrument table stays
// under the metrics NLOC limit as coins are added. eToro names crypto by the bare ticker
// (BTC, ETH, SOL, XRP, …) while the local model answers with the exchange pair (BTCUSDT,
// ETH-USD, LTCUSD), which matchProposalSymbol strips. Every crypto economic — the x2 leverage
// cap, the 1% cost floor, the own correlation bucket, 24/7 weekend trading — keys off
// group == "Crypto", so a new coin needs only its row here. TradingView spot pairs are
// Coinbase where listed, Binance for the two it does not carry (TRX, BNB); Yahoo quotes every
// one as <TICKER>-USD. TRX and BNB use a BINANCE: TradingView reference because Coinbase has no
// spot pair for them; eToro trades both, BNB priced in USD like the rest (verified live).
namespace {
QList<InstrumentSpec> cryptoInstruments()
{
    return {
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
        {QStringLiteral("AVAX"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:AVAXUSD"), QStringLiteral("AVAX-USD"),
         QStringLiteral("US"), {1, 2}, 35.0},
        {QStringLiteral("DOGE"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:DOGEUSD"), QStringLiteral("DOGE-USD"),
         QStringLiteral("US"), {1, 2}, 0.16},
        {QStringLiteral("DOT"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:DOTUSD"), QStringLiteral("DOT-USD"),
         QStringLiteral("US"), {1, 2}, 7.0},
        {QStringLiteral("LINK"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:LINKUSD"), QStringLiteral("LINK-USD"),
         QStringLiteral("US"), {1, 2}, 18.0},
        {QStringLiteral("SAND"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:SANDUSD"), QStringLiteral("SAND-USD"),
         QStringLiteral("US"), {1, 2}, 0.45},
        {QStringLiteral("TRX"), QStringLiteral("Crypto"),
         QStringLiteral("BINANCE:TRXUSDT"), QStringLiteral("TRX-USD"),
         QStringLiteral("US"), {1, 2}, 0.13},
        {QStringLiteral("BCH"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:BCHUSD"), QStringLiteral("BCH-USD"),
         QStringLiteral("US"), {1, 2}, 480.0},
        {QStringLiteral("LTC"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:LTCUSD"), QStringLiteral("LTC-USD"),
         QStringLiteral("US"), {1, 2}, 90.0},
        {QStringLiteral("BNB"), QStringLiteral("Crypto"),
         QStringLiteral("BINANCE:BNBUSDT"), QStringLiteral("BNB-USD"),
         QStringLiteral("US"), {1, 2}, 590.0},
        {QStringLiteral("XLM"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:XLMUSD"), QStringLiteral("XLM-USD"),
         QStringLiteral("US"), {1, 2}, 0.12},
        {QStringLiteral("SUN"), QStringLiteral("Crypto"),
         QStringLiteral("BINANCE:SUNUSDT"), QStringLiteral("SUN-USD"),
         QStringLiteral("US"), {1, 2}, 0.02},
        {QStringLiteral("ADA"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:ADAUSD"), QStringLiteral("ADA-USD"),
         QStringLiteral("US"), {1, 2}, 0.45},
        {QStringLiteral("MATIC"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:MATICUSD"), QStringLiteral("MATIC-USD"),
         QStringLiteral("US"), {1, 2}, 0.50},
        {QStringLiteral("EOS"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:EOSUSD"), QStringLiteral("EOS-USD"),
         QStringLiteral("US"), {1, 2}, 0.70},
        {QStringLiteral("FTM"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:FTMUSD"), QStringLiteral("FTM-USD"),
         QStringLiteral("US"), {1, 2}, 0.50},
        {QStringLiteral("ATOM"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:ATOMUSD"), QStringLiteral("ATOM-USD"),
         QStringLiteral("US"), {1, 2}, 6.0},
        {QStringLiteral("ALGO"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:ALGOUSD"), QStringLiteral("ALGO-USD"),
         QStringLiteral("US"), {1, 2}, 0.15},
        {QStringLiteral("XTZ"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:XTZUSD"), QStringLiteral("XTZ-USD"),
         QStringLiteral("US"), {1, 2}, 0.90},
        {QStringLiteral("ETC"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:ETCUSD"), QStringLiteral("ETC-USD"),
         QStringLiteral("US"), {1, 2}, 20.0},
        {QStringLiteral("ZEC"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:ZECUSD"), QStringLiteral("ZEC-USD"),
         QStringLiteral("US"), {1, 2}, 30.0},
        {QStringLiteral("DASH"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:DASHUSD"), QStringLiteral("DASH-USD"),
         QStringLiteral("US"), {1, 2}, 30.0},
        {QStringLiteral("MANA"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:MANAUSD"), QStringLiteral("MANA-USD"),
         QStringLiteral("US"), {1, 2}, 0.40},
        {QStringLiteral("NEAR"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:NEARUSD"), QStringLiteral("NEAR-USD"),
         QStringLiteral("US"), {1, 2}, 5.0},
        {QStringLiteral("AAVE"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:AAVEUSD"), QStringLiteral("AAVE-USD"),
         QStringLiteral("US"), {1, 2}, 100.0},
        {QStringLiteral("SUSHI"), QStringLiteral("Crypto"),
         QStringLiteral("COINBASE:SUSHIUSD"), QStringLiteral("SUSHI-USD"),
         QStringLiteral("US"), {1, 2}, 1.0},
    };
}
} // namespace

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
    static const QList<InstrumentSpec> kCatalog = [] {
        QList<InstrumentSpec> all = {
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
        };
        // The crypto slice lives in its own function (above), so this flat table stays under
        // the metrics NLOC limit as coins are added.
        all.append(cryptoInstruments());
        return all;
    }();
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

QString marketHoursText(const QString &symbol)
{
    const InstrumentSpec *spec = instrumentSpec(symbol);
    if (spec == nullptr) {
        return QStringLiteral("hours unknown");
    }
    if (spec->group == QStringLiteral("Crypto")) {
        return QStringLiteral("24/7 — trades every day");
    }
    // The FIRST calendar region picks the exchange. Edges match PaperTrader's openPhase/
    // closePhase (09:30–16:00 New York, 09:00–17:30 Xetra, 09:30–16:00 HKEX) so the display and
    // the bot's session sit-outs cannot tell the user different things.
    const QString region = spec->calendarRegions.section(QLatin1Char(','), 0, 0).trimmed();
    static const QHash<QString, QString> kHours = {
        {QStringLiteral("US"), QStringLiteral("09:30–16:00 New York · Mon–Fri")},
        {QStringLiteral("DE"), QStringLiteral("09:00–17:30 Frankfurt · Mon–Fri")},
        {QStringLiteral("EU"), QStringLiteral("09:00–17:30 Frankfurt · Mon–Fri")},
        {QStringLiteral("FR"), QStringLiteral("09:00–17:30 Frankfurt · Mon–Fri")},
        {QStringLiteral("HK"), QStringLiteral("09:30–16:00 Hong Kong · Mon–Fri")},
        {QStringLiteral("CN"), QStringLiteral("09:30–16:00 Hong Kong · Mon–Fri")},
        {QStringLiteral("CH"), QStringLiteral("09:00–17:30 Zurich · Mon–Fri")},
        {QStringLiteral("SE"), QStringLiteral("09:00–17:30 Stockholm · Mon–Fri")},
        {QStringLiteral("CA"), QStringLiteral("09:30–16:00 Toronto · Mon–Fri")},
        {QStringLiteral("CO"), QStringLiteral("09:30–15:55 Bogotá · Mon–Fri")}};
    return kHours.value(region, region + QStringLiteral(" exchange · Mon–Fri"));
}

QStringList nonCryptoTradableSymbols()
{
    QStringList out;
    for (const InstrumentSpec &spec : instrumentCatalog()) {
        if (spec.group != QLatin1String("Crypto")) {
            out.append(spec.symbol);
        }
    }
    return out;
}

bool isCryptoSymbol(const QString &symbol)
{
    const InstrumentSpec *spec = instrumentSpec(symbol);
    return (spec != nullptr) && (spec->group == QLatin1String("Crypto"));
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
