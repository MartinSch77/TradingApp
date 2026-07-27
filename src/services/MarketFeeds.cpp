#include "services/MarketFeeds.h"

#include "domain/DecisionEngine.h"
#include "services/JsonHttp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <cmath>
#include <utility>

namespace {

// Map an app instrument symbol to its TradingView ticker for the real-time technical
// rating. Every entry confirmed working via the TradingView scanner (returns non-null
// Recommend.All values). Indices/FX/commodities map to their own symbol; eToro's
// thematic baskets map to the closest liquid ETF/index proxy (marked "proxy") — an
// advisory stand-in, not the basket itself. RUBBER has no rated symbol (SGX TSR20
// futures return null ratings) -> empty = "n/a".
QString tradingViewTicker(const QString &symbol)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("SPX500"), QStringLiteral("SP:SPX")},
        {QStringLiteral("SP.24-7"), QStringLiteral("SP:SPX")},
        {QStringLiteral("NSDQ100"), QStringLiteral("NASDAQ:NDX")},
        {QStringLiteral("NSDQ100.24-7"), QStringLiteral("NASDAQ:NDX")},
        {QStringLiteral("DJ30"), QStringLiteral("OANDA:US30USD")},
        {QStringLiteral("GER40"), QStringLiteral("XETR:DAX")},
        {QStringLiteral("EUSTX50"), QStringLiteral("TVC:SX5E")},
        {QStringLiteral("RTY"), QStringLiteral("TVC:RUT")},
        {QStringLiteral("HKG50"), QStringLiteral("TVC:HSI")},
        {QStringLiteral("CHINA50"), QStringLiteral("HKEX:2823")},        // iShares FTSE A50 ETF
        {QStringLiteral("Switzerland20"), QStringLiteral("SIX:SMI")},
        {QStringLiteral("Canada60"), QStringLiteral("TSX:TX60")},        // S&P/TSX 60 index
        {QStringLiteral("Sweden30"), QStringLiteral("OMXSTO:OMXS30")},   // OMX Stockholm 30
        {QStringLiteral("USDOLLAR"), QStringLiteral("TVC:DXY")},
        {QStringLiteral("EURUSD"), QStringLiteral("FX:EURUSD")},
        {QStringLiteral("Gold.24-7"), QStringLiteral("TVC:GOLD")},
        {QStringLiteral("OIL.24-7"), QStringLiteral("FX:USOIL")},        // WTI CFD stream
        {QStringLiteral("Semiconductors"), QStringLiteral("NASDAQ:SOX")},  // proxy: PHLX semi index
        {QStringLiteral("AI.Leaders"), QStringLiteral("NASDAQ:AIQ")},      // proxy: Global X AI ETF
        {QStringLiteral("Cybersecurity"), QStringLiteral("NASDAQ:CIBR")},  // proxy: First Trust cyber ETF
        {QStringLiteral("Quantum"), QStringLiteral("NASDAQ:QTUM")},        // proxy: Defiance quantum ETF
        {QStringLiteral("GoldMiners"), QStringLiteral("AMEX:GDX")},        // proxy: VanEck gold miners ETF
        {QStringLiteral("Nuclear"), QStringLiteral("AMEX:NLR")},           // proxy: VanEck uranium+nuclear ETF
        {QStringLiteral("Crypto10"), QStringLiteral("CRYPTOCAP:TOTAL")},   // proxy: total crypto market cap
        {QStringLiteral("Colombia"), QStringLiteral("BVC:ICOLCAP")},       // proxy: iShares COLCAP ETF
    };
    return map.value(symbol);
}

// Map an app instrument symbol to its Yahoo Finance ticker for the reference
// quote. Cash indices for the standard listings; the 24/7 variants map to the
// futures contract, whose round-the-clock pricing is what the CFD tracks.
QString yahooTicker(const QString &symbol)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("SPX500"), QStringLiteral("^GSPC")},
        {QStringLiteral("SP.24-7"), QStringLiteral("ES=F")},
        {QStringLiteral("NSDQ100"), QStringLiteral("^NDX")},
        {QStringLiteral("NSDQ100.24-7"), QStringLiteral("NQ=F")},
        {QStringLiteral("DJ30"), QStringLiteral("^DJI")},
        {QStringLiteral("GER40"), QStringLiteral("^GDAXI")},
        {QStringLiteral("EUSTX50"), QStringLiteral("^STOXX50E")},
        {QStringLiteral("RTY"), QStringLiteral("^RUT")},
        {QStringLiteral("HKG50"), QStringLiteral("^HSI")},
        {QStringLiteral("Switzerland20"), QStringLiteral("^SSMI")},
        {QStringLiteral("USDOLLAR"), QStringLiteral("DX-Y.NYB")},
        {QStringLiteral("EURUSD"), QStringLiteral("EURUSD=X")},
        {QStringLiteral("Gold.24-7"), QStringLiteral("GC=F")},
        {QStringLiteral("OIL.24-7"), QStringLiteral("CL=F")},
    };
    return map.value(symbol);
}

// TradingView's rating buckets for the aggregated recommendation score in [-1, 1].

// Collect the web tickers for the tradable instruments; several app symbols can share
// one ticker (e.g. SPX500 / SP.24-7 -> SP:SPX), so map each ticker to all its symbols.
// orderedTickers receives each distinct ticker once, in first-seen order.
QHash<QString, QStringList> symbolsByWebTicker(const QStringList &tradable,
                                               QStringList &orderedTickers)
{
    QHash<QString, QStringList> byTicker;
    for (const QString &sym : tradable) {
        const QString tv = tradingViewTicker(sym);
        if (tv.isEmpty()) {
            continue;
        }
        if (!byTicker.contains(tv)) {
            orderedTickers << tv;
        }
        byTicker[tv] << sym;
    }
    return byTicker;
}

} // namespace

MarketFeeds::MarketFeeds(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_http(new JsonHttp(m_nam, this)), m_timer(new QTimer(this))
{
    
    // Abort any request that stalls with no data for 30s so its finished() always
    // fires (public feeds occasionally hang behind CDNs instead of failing fast).
    m_nam->setTransferTimeout(std::chrono::seconds{30});
    
    
    static_cast<void>(connect(m_timer, &QTimer::timeout, this, [this] {
        fetchVix();
        fetchExternalSignal();
        fetchFearGreed();
        fetchWebQuote();
    }));
}

void MarketFeeds::setTradableSymbols(const QStringList &symbols)
{
    m_tradableSymbols = symbols;
}

void MarketFeeds::setCurrentSymbol(const QString &symbol)
{
    if (m_currentSymbol == symbol) {
        return;
    }
    m_currentSymbol = symbol;
    // Refresh the single-instrument rating and reference quote promptly for the
    // new instrument (the periodic cycle would otherwise leave the panel stale
    // for a whole interval).
    if (m_timer->isActive()) {
        fetchExternalSignal();
        fetchWebQuote();
    }
}

void MarketFeeds::start(qint32 refreshIntervalMs)
{
    m_timer->setInterval(refreshIntervalMs);
    m_timer->start();
    fetchVix();
    fetchExternalSignal();
    fetchFearGreed();
    fetchWebQuote();
}

void MarketFeeds::fetchVix()
{
    // eToro only lists monthly VIX futures (awkward to roll and bulk-quote), so the
    // spot index comes from a free, no-key public feed. A browser User-Agent is
    // required or the request is blocked. Pull a few months of daily closes so the
    // reading can be judged against a longer-term baseline, not just yesterday.
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://query1.finance.yahoo.com/v8/finance/chart/%5EVIX?interval=1d&range=3mo")));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    m_http->handleReply(reply, [this](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok || !doc.isObject()) {
            return;  // transient / offline — keep the last VIX reading
        }
        const QJsonArray result = doc.object()
                                      .value(QStringLiteral("chart"))
                                      .toObject()
                                      .value(QStringLiteral("result"))
                                      .toArray();
        if (result.isEmpty()) {
            return;
        }
        const QJsonObject r0 = result.first().toObject();
        const QJsonObject meta = r0.value(QStringLiteral("meta")).toObject();

        // Daily close series (nulls appear on holidays / missing days).
        QList<double> closes;
        const QJsonArray quotes = r0.value(QStringLiteral("indicators"))
                                      .toObject()
                                      .value(QStringLiteral("quote"))
                                      .toArray();
        if (!quotes.isEmpty()) {
            const QJsonArray closeArr = quotes.first().toObject()
                                            .value(QStringLiteral("close")).toArray();
            for (const auto &v : closeArr) {
                if (v.isDouble() && (v.toDouble() > 0.0)) {
                    closes.append(v.toDouble());
                }
            }
        }

        double price = meta.value(QStringLiteral("regularMarketPrice")).toDouble();
        if ((price <= 0.0) && !closes.isEmpty()) {
            price = closes.last();
        }
        if (price <= 0.0) {
            return;
        }

        // Baseline = average over the whole fetched window (~3 months of daily closes,
        // hence always at least a month), so "elevated" / "calm" is measured against a
        // long-term norm rather than the prior day. Fall back to the previous close if
        // there is too little history.
        double baseline = 0.0;
        if (closes.size() >= 5) {
            for (const double c : std::as_const(closes)) {
                baseline += c;
            }
            baseline /= static_cast<double>(closes.size());
        } else {
            baseline = meta.value(QStringLiteral("chartPreviousClose")).toDouble();
        }
        // changePct = how far the current VIX sits above/below its longer-term average.
        const double changePct =
            (baseline > 0.0) ? (((price - baseline) / baseline) * 100.0) : 0.0;
        emit vixUpdated(price, changePct);
    });
}

void MarketFeeds::fetchExternalSignal()
{
    const QString tv = tradingViewTicker(m_currentSymbol);
    if (tv.isEmpty()) {
        emit externalSignalUpdated(false, 0.0, QString());  // no web equivalent
        return;
    }
    // TradingView's public scanner returns the aggregated technical rating for a
    // ticker/timeframe. "Recommend.All|60" = the 1-hour summary in [-1, 1].
    QJsonObject query;
    query[QStringLiteral("types")] = QJsonArray{};
    QJsonObject symbols;
    symbols[QStringLiteral("tickers")] = QJsonArray{tv};
    symbols[QStringLiteral("query")] = query;
    QJsonObject body;
    body[QStringLiteral("symbols")] = symbols;
    body[QStringLiteral("columns")] = QJsonArray{QStringLiteral("Recommend.All|60")};

    QNetworkRequest req(QUrl(QStringLiteral("https://scanner.tradingview.com/global/scan")));
    JsonHttp::setBrowserHeaders(req);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam->post(req, payload);
    const QString wantSymbol = m_currentSymbol;  // guard against a switch mid-flight
    m_http->handleReply(reply, [this, wantSymbol](bool ok, qint32 /*status*/,
                                                  const QJsonDocument &doc,
                                                  const QByteArray & /*raw*/,
                                                  const QString & /*netError*/) {
        if (!ok || (m_currentSymbol != wantSymbol)) {
            return;  // transient, or the instrument changed under us — ignore
        }
        const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
        if (data.isEmpty()) {
            return;
        }
        const QJsonArray d = data.first().toObject().value(QStringLiteral("d")).toArray();
        if (d.isEmpty() || !d.first().isDouble()) {
            return;
        }
        const double score = d.first().toDouble();
        emit externalSignalUpdated(true, score, trading::webRatingWord(score));
    });
}

void MarketFeeds::fetchInstrumentRatings()
{
    QStringList tickers;
    const QHash<QString, QStringList> symbolsByTicker =
        symbolsByWebTicker(m_tradableSymbols, tickers);
    if (tickers.isEmpty()) {
        emit instrumentRatingsUpdated({});
        return;
    }

    QJsonArray tickerArr;
    for (const QString &t : std::as_const(tickers)) {
        tickerArr.append(t);
    }
    QJsonObject symbols;
    symbols[QStringLiteral("tickers")] = tickerArr;
    symbols[QStringLiteral("query")] = QJsonObject{{QStringLiteral("types"), QJsonArray{}}};
    QJsonObject body;
    body[QStringLiteral("symbols")] = symbols;
    // Three timeframes so the decision window can form a multi-timeframe consensus.
    body[QStringLiteral("columns")] = QJsonArray{QStringLiteral("Recommend.All|15"),
                                                 QStringLiteral("Recommend.All|60"),
                                                 QStringLiteral("Recommend.All|1D")};

    QNetworkRequest req(QUrl(QStringLiteral("https://scanner.tradingview.com/global/scan")));
    JsonHttp::setBrowserHeaders(req);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam->post(req, payload);
    m_http->handleReply(reply, [this, symbolsByTicker](bool ok, qint32 /*status*/,
                                                       const QJsonDocument &doc,
                                                       const QByteArray & /*raw*/,
                                                       const QString & /*err*/) {
        if (!ok) {
            return;
        }
        // The "d" array maps 1:1 to the requested columns (15m, 1h, 1D); a null entry
        // (timeframe unavailable right now) stays NaN.
        auto cell = [](const QJsonArray &d, qsizetype i) {
            if (i < d.size()) {
                const QJsonValue v = d.at(i);
                if (v.isDouble()) {
                    return v.toDouble();
                }
            }
            return std::nan("");
        };
        QHash<QString, WebRating> ratingBySymbol;
        const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
        for (const auto &v : data) {
            const QJsonObject o = v.toObject();
            const QJsonArray d = o.value(QStringLiteral("d")).toArray();
            WebRating r;
            r.m15 = cell(d, 0);
            r.h1 = cell(d, 1);
            r.d1 = cell(d, 2);
            if (!r.valid()) {
                continue;
            }
            const QStringList syms =
                symbolsByTicker.value(o.value(QStringLiteral("s")).toString());
            for (const QString &sym : syms) {
                static_cast<void>(ratingBySymbol.insert(sym, r));
            }
        }
        emit instrumentRatingsUpdated(ratingBySymbol);
    });
}

void MarketFeeds::fetchInstrumentNews()
{
    QStringList tickers;
    const QHash<QString, QStringList> symbolsByTicker =
        symbolsByWebTicker(m_tradableSymbols, tickers);
    // One request per unique ticker, published to every app symbol that shares it.
    for (const QString &ticker : std::as_const(tickers)) {
        QUrl url(QStringLiteral("https://news-mediator.tradingview.com/public/view/v1/symbol"));
        // Build the query as a literal string: the filters carry ':' which the feed
        // expects unencoded (matching the browser widget's request).
        url.setQuery(QStringLiteral("filter=lang:en&filter=symbol:%1&client=overview&streaming=false")
                         .arg(ticker));
        QNetworkRequest req(url);
        JsonHttp::setBrowserHeaders(req);
        const QByteArray origin("https://www.tradingview.com");
        const QByteArray referer("https://www.tradingview.com/");
        req.setRawHeader("Origin", origin);
        req.setRawHeader("Referer", referer);
        QNetworkReply *reply = m_nam->get(req);
        const QStringList syms = symbolsByTicker.value(ticker);
        m_http->handleReply(reply, [this, syms](bool ok, qint32 /*status*/,
                                                const QJsonDocument &doc,
                                                const QByteArray & /*raw*/,
                                                const QString & /*err*/) {
            if (!ok) {
                return;
            }
            QList<NewsHeadline> headlines;
            const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();
            for (const auto &v : items) {
                const QJsonObject o = v.toObject();
                NewsHeadline h;
                h.title = o.value(QStringLiteral("title")).toString();
                h.provider = o.value(QStringLiteral("provider")).toObject()
                                 .value(QStringLiteral("name")).toString();
                const qint64 secs =
                    static_cast<qint64>(o.value(QStringLiteral("published")).toDouble());
                if (secs > 0) {
                    h.published = QDateTime::fromSecsSinceEpoch(secs);
                }
                if (h.title.isEmpty()) {
                    continue;
                }
                headlines << h;
                if (headlines.size() >= 5) {  // a few recent items are enough for the tooltip
                    break;
                }
            }
            for (const QString &sym : syms) {
                emit instrumentNewsUpdated(sym, headlines);
            }
        }, /*retriesLeft=*/1);
    }
}

void MarketFeeds::fetchFearGreed()
{
    // CNN's Fear & Greed index — the aggregate mood of the trading crowd (put/call
    // ratios, breadth, junk-bond demand, momentum, ...). The endpoint serves the
    // markets page and rejects plain clients: it needs the browser User-Agent AND
    // a Referer from the CNN page, or it answers 418.
    QNetworkRequest req(QUrl(
        QStringLiteral("https://production.dataviz.cnn.io/index/fearandgreed/graphdata")));
    JsonHttp::setBrowserHeaders(req);
    const QByteArray refererKey("Referer");
    const QByteArray refererValue("https://edition.cnn.com/markets/fear-and-greed");
    req.setRawHeader(refererKey, refererValue);
    QNetworkReply *reply = m_nam->get(req);
    m_http->handleReply(reply, [this](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok || !doc.isObject()) {
            return;  // transient / blocked — keep the last reading
        }
        const QJsonObject fg = doc.object().value(QStringLiteral("fear_and_greed")).toObject();
        const double score = fg.value(QStringLiteral("score")).toDouble(-1.0);
        if ((score < 0.0) || (score > 100.0)) {
            return;
        }
        emit fearGreedUpdated(score, fg.value(QStringLiteral("rating")).toString());
    });
}

void MarketFeeds::fetchIntradaySeries()
{
    // One chart request per mapped instrument; instruments without a Yahoo
    // ticker (eToro proprietary baskets) are silently skipped. Same endpoint
    // as the reference quote, but here the 1-minute close ARRAY is the payload.
    for (const QString &symbol : std::as_const(m_tradableSymbols)) {
        const QString ticker = yahooTicker(symbol);
        if (ticker.isEmpty()) {
            continue;
        }
        QNetworkRequest req(QUrl(QStringLiteral(
                                     "https://query1.finance.yahoo.com/v8/finance/chart/"
                                     "%1?interval=1m&range=1d")
                                     .arg(QString::fromLatin1(
                                         QUrl::toPercentEncoding(ticker)))));
        JsonHttp::setBrowserHeaders(req);
        QNetworkReply *reply = m_nam->get(req);
        m_http->handleReply(reply, [this, symbol](bool ok, qint32 /*status*/,
                                                  const QJsonDocument &doc,
                                                  const QByteArray & /*raw*/,
                                                  const QString & /*netError*/) {
            if (!ok || !doc.isObject()) {
                return;  // transient / blocked — this source just stays absent
            }
            const QJsonArray result = doc.object()
                                          .value(QStringLiteral("chart"))
                                          .toObject()
                                          .value(QStringLiteral("result"))
                                          .toArray();
            if (result.isEmpty()) {
                return;
            }
            const QJsonArray closesJson = result.first()
                                              .toObject()
                                              .value(QStringLiteral("indicators"))
                                              .toObject()
                                              .value(QStringLiteral("quote"))
                                              .toArray()
                                              .first()
                                              .toObject()
                                              .value(QStringLiteral("close"))
                                              .toArray();
            QList<double> closes;
            closes.reserve(closesJson.size());
            for (const auto &v : closesJson) {  // QJsonValueConstRef, no conversion
                if (v.isDouble()) {  // gaps come through as null — skip them
                    closes.append(v.toDouble());
                }
            }
            if (!closes.isEmpty()) {
                emit intradayCloses(symbol, closes);
            }
        });
    }
}

void MarketFeeds::fetchWebQuote()
{
    const QString ticker = yahooTicker(m_currentSymbol);
    if (ticker.isEmpty()) {
        return;  // no Yahoo equivalent (eToro proprietary basket) — nothing to emit
    }
    QNetworkRequest req(QUrl(
        QStringLiteral("https://query1.finance.yahoo.com/v8/finance/chart/%1?interval=1m&range=1d")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(ticker)))));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    const QString wantSymbol = m_currentSymbol;  // guard against a switch mid-flight
    m_http->handleReply(reply, [this, wantSymbol](bool ok, qint32 /*status*/,
                                                  const QJsonDocument &doc,
                                                  const QByteArray & /*raw*/,
                                                  const QString & /*netError*/) {
        if (!ok || (m_currentSymbol != wantSymbol) || !doc.isObject()) {
            return;
        }
        const QJsonArray result = doc.object()
                                      .value(QStringLiteral("chart"))
                                      .toObject()
                                      .value(QStringLiteral("result"))
                                      .toArray();
        if (result.isEmpty()) {
            return;
        }
        const QJsonObject meta = result.first().toObject()
                                     .value(QStringLiteral("meta")).toObject();
        const double price = meta.value(QStringLiteral("regularMarketPrice")).toDouble();
        if (price <= 0.0) {
            return;
        }
        // regularMarketTime = the exchange timestamp of that price (epoch seconds).
        QDateTime asOf;
        const qint64 secs =
            static_cast<qint64>(meta.value(QStringLiteral("regularMarketTime")).toDouble());
        if (secs > 0) {
            asOf = QDateTime::fromSecsSinceEpoch(secs);
        }
        emit webQuoteUpdated(wantSymbol, price, asOf);
    });
}
