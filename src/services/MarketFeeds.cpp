// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/MarketFeeds.h"

#include "domain/DecisionEngine.h"
#include "domain/IndexConfluence.h"
#include "domain/InstrumentCatalog.h"
#include "domain/YahooChartParser.h"
#include "services/JsonHttp.h"

#include <QDateTime>
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
#include <numeric>
#include <utility>

namespace {

// The web tickers per app symbol live in the domain InstrumentCatalog (one
// entry per instrument, provenance documented there). Empty = "n/a".
QString tradingViewTicker(const QString &symbol)
{
    const trading::InstrumentSpec *spec = trading::instrumentSpec(symbol);
    return (spec != nullptr) ? spec->tradingViewTicker : QString();
}

QString yahooTicker(const QString &symbol)
{
    const trading::InstrumentSpec *spec = trading::instrumentSpec(symbol);
    return (spec != nullptr) ? spec->yahooTicker : QString();
}

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


// yahooChartResult / yahooCloses / yahooOhlc / yahooMetaSessionChange / yahooBars
// (the Yahoo v8 "chart" JSON parse) live in domain/YahooChartParser — Qt Core
// only, so they are independently testable and fuzzable without this TU's
// QNetworkAccessManager/JsonHttp dependency. See that header for the shape of
// each function; the long comment on the session-change fallback below is kept
// here because it explains WHY MarketFeeds calls it, not how it parses.
using trading::yahooBars;
using trading::yahooChartResult;
using trading::yahooCloses;
using trading::yahooMetaSessionChange;
using trading::yahooOhlc;

// The session change from the chart result's META block, as a two-point series
// [previousClose, regularMarketPrice] — or empty when the meta cannot supply both.
//
// WHY THIS EXISTS, and it is not a nicety: Yahoo's chart endpoint stopped returning
// intraday bars for EQUITY tickers while still answering 200. Measured 2026-08-07 with a
// browser User-Agent, mid-session (11:47 New York, Thursday, the cash market open):
// `AAPL?interval=1m&range=1d` came back with NO `timestamp` key at all and an empty close
// array, while `^VIX` on the same request returned bars normally. `range=5d` did return
// 1561 bars for AAPL, but the newest was the PREVIOUS day's 16:00 close — the current
// session was absent entirely. Cookies, query2, includePrePost and interval=5m all made
// no difference.
//
// The consequence in the running app was silent and total: yahooCloses() returned an empty
// list for all twelve heavyweight constituents, so `emit referenceSeries` never fired for
// them, `heavyweightPulse` measured 0 of 10, and the Heavyweights window sat on "no
// constituent prices yet" forever. The bot's own decision log recorded the same damage as
// "4 of 10 reads measured (6 unmeasurable)" on every single evaluation.
//
// The SAME response still carries live `regularMarketPrice` and `previousClose`, and the
// difference between them IS the session change — the exact quantity `sessionChangePct`
// derives from the first and last bar. So this is a MEASURED value, not a guess, which is
// why using it does not violate the rule that an unmeasurable read stays UNKNOWN.
//
// Two traps, both load-bearing:
//   * `chartPreviousClose` is the WRONG field. On a 5-day range it is the close before the
//     WINDOW, not before today: for AAPL it read 333.43 against a live 311.00, i.e. −6.7%,
//     where the true session change was +0.52% from previousClose 309.38. Only
//     `previousClose` is yesterday's close on every range.
//   * Two points are all this can honestly claim. It restores the session-change reads
//     (participation, volatility direction, yields, the curve) and it must NOT be dressed
//     up as an intraday series — the VWAP and up/down-volume reads need real bars and stay
//     UNKNOWN, and the constituent CHART skips series this short rather than drawing a
//     straight line that looks like a flat session.
//
// The close AND volume arrays of a Yahoo chart result (yahooBars) are ALIGNED bars for
// the same reason: yahooCloses skips gaps, and the close/volume arrays do not have their
// gaps in the same places, so two independent parses would return two lists whose index i
// refers to two different minutes — a VWAP built from that is a number about nothing.

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

// One line per feed per 10 minutes: enough to notice a dead source without a
// poll-cadence feed turning the log into noise while the panels keep their
// last reading.
void MarketFeeds::reportFeedError(const QString &feed, const QString &detail)
{
    constexpr qint64 kThrottleMs = 10LL * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lastFeedErrorMs.value(feed, 0);
    if ((now - last) < kThrottleMs) {
        return;
    }
    m_lastFeedErrorMs.insert(feed, now);
    emit log(QStringLiteral("%1 feed unavailable%2 - keeping the last reading.")
                 .arg(feed, detail.isEmpty() ? QString() : (QStringLiteral(": ") + detail)),
             true);
}

void MarketFeeds::setTradableSymbols(const QStringList &symbols)
{
    m_tradableSymbols = symbols;
}

void MarketFeeds::setEndpointBaseForTesting(const QString &base)
{
    m_endpointBaseForTesting = base;
}

QString MarketFeeds::feedUrl(const QString &host, const QString &pathAndQuery) const
{
    return (m_endpointBaseForTesting.isEmpty() ? host : m_endpointBaseForTesting) + pathAndQuery;
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
    QNetworkRequest req(QUrl(feedUrl(
        QStringLiteral("https://query1.finance.yahoo.com"),
        QStringLiteral("/v8/finance/chart/%5EVIX?interval=1d&range=3mo"))));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    m_http->handleReply(reply, [this](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString &netError) {
        if (!ok || !doc.isObject()) {
            reportFeedError(QStringLiteral("VIX"), netError);  // keep the last reading
            return;
        }
        const QJsonObject r0 = yahooChartResult(doc);
        const QJsonObject meta = r0.value(QStringLiteral("meta")).toObject();

        // Daily close series (nulls appear on holidays / missing days).
        const QList<double> closes = yahooCloses(r0, /*positiveOnly=*/true);

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
            baseline = std::accumulate(closes.cbegin(), closes.cend(), 0.0)
                       / static_cast<double>(closes.size());
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

    QNetworkRequest req(QUrl(feedUrl(QStringLiteral("https://scanner.tradingview.com"),
                                     QStringLiteral("/global/scan"))));
    JsonHttp::setBrowserHeaders(req);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam->post(req, payload);
    const QString wantSymbol = m_currentSymbol;  // guard against a switch mid-flight
    m_http->handleReply(reply, [this, wantSymbol](bool ok, qint32 /*status*/,
                                                  const QJsonDocument &doc,
                                                  const QByteArray & /*raw*/,
                                                  const QString &netError) {
        if (!ok) {
            reportFeedError(QStringLiteral("Web rating"), netError);
            return;
        }
        if (m_currentSymbol != wantSymbol) {
            return;  // the instrument changed under us — ignore
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

    QNetworkRequest req(QUrl(feedUrl(QStringLiteral("https://scanner.tradingview.com"),
                                     QStringLiteral("/global/scan"))));
    JsonHttp::setBrowserHeaders(req);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam->post(req, payload);
    m_http->handleReply(reply, [this, symbolsByTicker](bool ok, qint32 /*status*/,
                                                       const QJsonDocument &doc,
                                                       const QByteArray & /*raw*/,
                                                       const QString &netError) {
        if (!ok) {
            reportFeedError(QStringLiteral("Instrument ratings"), netError);
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
        QUrl url(feedUrl(QStringLiteral("https://news-mediator.tradingview.com"),
                         QStringLiteral("/public/view/v1/symbol")));
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
                                                const QString &netError) {
            if (!ok) {
                reportFeedError(QStringLiteral("News"), netError);
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
    QNetworkRequest req(QUrl(feedUrl(
        QStringLiteral("https://production.dataviz.cnn.io"),
        QStringLiteral("/index/fearandgreed/graphdata"))));
    JsonHttp::setBrowserHeaders(req);
    const QByteArray refererKey("Referer");
    const QByteArray refererValue("https://edition.cnn.com/markets/fear-and-greed");
    req.setRawHeader(refererKey, refererValue);
    QNetworkReply *reply = m_nam->get(req);
    m_http->handleReply(reply, [this](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString &netError) {
        if (!ok || !doc.isObject()) {
            reportFeedError(QStringLiteral("Fear & Greed"), netError);  // keep the last reading
            return;
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
        fetchIntradayRange(symbol, ticker, QStringLiteral("1d"), /*candlesOnly=*/false);
    }
}

void MarketFeeds::fetchIntradayRange(const QString &symbol, const QString &ticker,
                                     const QString &range, bool candlesOnly)
{
    QNetworkRequest req(QUrl(feedUrl(
        QStringLiteral("https://query1.finance.yahoo.com"),
        QStringLiteral("/v8/finance/chart/%1?interval=1m&range=%2")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(ticker)), range))));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    m_http->handleReply(reply, [this, symbol, ticker, range, candlesOnly](
                                   bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                   const QByteArray & /*raw*/, const QString &netError) {
        if (!ok || !doc.isObject()) {
            reportFeedError(QStringLiteral("Intraday series"), netError);  // stays absent
            return;
        }
        const QJsonObject result = yahooChartResult(doc);
        const QList<double> closes = yahooCloses(result, /*positiveOnly=*/false);
        // The close series feeds the DECISION composite, so it stays the CURRENT session and is
        // only emitted from the primary (1d) request — never from the wider fallback below.
        if (!candlesOnly && !closes.isEmpty()) {
            emit intradayCloses(symbol, closes);
        }
        // The candles feed a CHART. Parsed from the SAME response but emitted separately, so a
        // parse change on one cannot move the other.
        const trading::CandleColumns ohlc = yahooOhlc(result);
        const QList<trading::Candle> candles =
            trading::candlesFrom(ohlc.opens, ohlc.highs, ohlc.lows, ohlc.closes);
        if (!candles.isEmpty()) {
            emit intradayCandles(symbol, candles);
            return;
        }
        // Fall back ONLY when the session is TRULY empty — no closes AND no candles — which is a
        // CLOSED market: the .24-7 futures (ES=F, CL=F, NQ=F, GC=F) return an empty 1-minute
        // series on a weekend or holiday, and that is what left their chart blank. Retry ONCE
        // with a wider range so the chart shows the LAST session's candles. Candles only (see
        // above), and only from the primary request, so it can never recurse. An instrument that
        // returned closes but no drawable candles is NOT closed and must not trigger this.
        if ((range == QStringLiteral("1d")) && closes.isEmpty()) {
            fetchIntradayRange(symbol, ticker, QStringLiteral("5d"), /*candlesOnly=*/true);
        }
    });
}

void MarketFeeds::fetchDailyBars(const QString &symbol, const QString &ticker)
{
    if (ticker.isEmpty()) {
        return;
    }
    // A year of daily bars is comfortably past SwingPullbackConfig's default 200-session
    // slow EMA (EMA200 needs 200 CLOSES behind it to be meaningful at all, and a year of
    // trading days is ~252).
    QNetworkRequest req(QUrl(feedUrl(
        QStringLiteral("https://query1.finance.yahoo.com"),
        QStringLiteral("/v8/finance/chart/%1?interval=1d&range=1y")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(ticker))))));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    m_http->handleReply(reply, [this, symbol](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                              const QByteArray & /*raw*/, const QString &netError) {
        if (!ok || !doc.isObject()) {
            reportFeedError(QStringLiteral("Daily bars"), netError);
            emit dailyBarsReady(symbol, {});
            return;
        }
        const trading::CandleColumns ohlc = yahooOhlc(yahooChartResult(doc));
        const QList<trading::Candle> candles =
            trading::candlesFrom(ohlc.opens, ohlc.highs, ohlc.lows, ohlc.closes);
        QList<trading::DailyBar> bars;
        bars.reserve(candles.size());
        for (const trading::Candle &c : candles) {
            trading::DailyBar bar;
            bar.open = c.open;
            bar.high = c.high;
            bar.low = c.low;
            bar.close = c.close;
            bars.append(bar);
        }
        emit dailyBarsReady(symbol, bars);
    });
}

void MarketFeeds::fetchReferenceSeries()
{
    // The same chart endpoint the instrument sweep uses, over tickers that are not
    // instruments: expected volatility, the yield that moves growth shares, and the
    // companies that ARE most of the Nasdaq-100 and the S&P 500 (REQ-F-035). Fifteen
    // requests — the union of the two top-ten lists, which overlap in eight megacaps,
    // so nothing is fetched twice — and each one that fails simply leaves its read
    // absent rather than guessed.
    for (const QString &ticker : trading::referenceTickers()) {
        QNetworkRequest req(QUrl(feedUrl(
            QStringLiteral("https://query1.finance.yahoo.com"),
            QStringLiteral("/v8/finance/chart/%1?interval=1m&range=1d")
                .arg(QString::fromLatin1(QUrl::toPercentEncoding(ticker))))));
        JsonHttp::setBrowserHeaders(req);
        QNetworkReply *reply = m_nam->get(req);
        m_http->handleReply(reply, [this, ticker](bool ok, qint32 /*status*/,
                                                  const QJsonDocument &doc,
                                                  const QByteArray & /*raw*/,
                                                  const QString &netError) {
            if (!ok || !doc.isObject()) {
                reportFeedError(QStringLiteral("Reference series"), netError);
                return;
            }
            const QJsonObject result = yahooChartResult(doc);
            QList<double> closes = yahooCloses(result, /*positiveOnly=*/false);
            // Fewer than two points cannot yield a session change, so the read would be
            // UNKNOWN. Before accepting that, ask the meta block — which still carries a
            // live price and yesterday's close even on the equity responses that now come
            // back with no bars at all. See yahooMetaSessionChange for the measurement.
            if (closes.size() < 2) {
                const QList<double> fromMeta = yahooMetaSessionChange(result);
                if (!fromMeta.isEmpty()) {
                    closes = fromMeta;
                }
            }
            if (!closes.isEmpty()) {
                emit referenceSeries(ticker, closes);
            }
            // The same response, read a second way: the bars that carry volume, aligned.
            // Emitted separately rather than folded into the close series above, because
            // the close series must keep its own gap handling — the volatility and yield
            // indices have no volume, and dropping their volume-less bars would empty
            // the very reads that do work.
            const trading::VolumeSeries bars = yahooBars(result);
            if (!bars.volumes.isEmpty()) {
                emit referenceVolumeSeries(ticker, bars);
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
    QNetworkRequest req(QUrl(feedUrl(
        QStringLiteral("https://query1.finance.yahoo.com"),
        QStringLiteral("/v8/finance/chart/%1?interval=1m&range=1d")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(ticker))))));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    const QString wantSymbol = m_currentSymbol;  // guard against a switch mid-flight
    m_http->handleReply(reply, [this, wantSymbol](bool ok, qint32 /*status*/,
                                                  const QJsonDocument &doc,
                                                  const QByteArray & /*raw*/,
                                                  const QString &netError) {
        if (!ok || !doc.isObject()) {
            reportFeedError(QStringLiteral("Web quote"), netError);
            return;
        }
        if (m_currentSymbol != wantSymbol) {
            return;  // the instrument changed under us — ignore
        }
        const QJsonObject meta =
            yahooChartResult(doc).value(QStringLiteral("meta")).toObject();
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
