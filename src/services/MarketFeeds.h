#ifndef TRADINGAPP_SERVICES_MARKETFEEDS_H
#define TRADINGAPP_SERVICES_MARKETFEEDS_H

#include "domain/Models.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

class JsonHttp;
class QNetworkAccessManager;
class QTimer;

// Market context from public, non-eToro web feeds: the CBOE VIX (Yahoo), the
// TradingView technical rating (single instrument and bulk), recent news
// headlines, the CNN Fear & Greed crowd-sentiment index, and a Yahoo Finance
// reference quote for the current instrument (an independent, near-real-time
// cross-check on eToro's own rate). Independent of the broker connection — the
// feeds run identically in real, demo and simulation mode. Extracted from
// EtoroClient so the broker client only talks to eToro.
class MarketFeeds : public QObject
{
    Q_OBJECT
public:
    explicit MarketFeeds(QObject *parent = nullptr);

    // The instruments the bulk rating/news fetches cover.
    void setTradableSymbols(const QStringList &symbols);

    // The instrument the periodic single-instrument rating tracks; switching
    // re-fetches promptly so the panel isn't stale for a whole refresh cycle.
    void setCurrentSymbol(const QString &symbol);

    // Fetch now, then refresh the VIX and the current instrument's rating
    // every refreshIntervalMs (the slow-moving cadence of both sources).
    void start(qint32 refreshIntervalMs);

    // Point EVERY feed host (Yahoo query1, TradingView scanner + news, CNN
    // dataviz) at one base URL, e.g. the tests' in-process MockHttpServer.
    // These are public feeds with no config entry, so this override is the
    // test seam — mirroring Config::baseUrl for the broker client. Empty
    // (the default) keeps the real hosts.
    void setEndpointBaseForTesting(const QString &base);


    // Fetch TradingView's aggregated technical rating for every tradable instrument
    // that has a mapped web ticker, in one call; result via instrumentRatingsUpdated.
    void fetchInstrumentRatings();
    // Fetch recent news headlines for every tradable instrument that has a mapped web
    // ticker; results arrive per instrument via instrumentNewsUpdated.
    void fetchInstrumentNews();
    // Fetch the Yahoo Finance intraday close series (1-minute bars, session so
    // far) for every tradable instrument with a mapped Yahoo ticker; results
    // arrive per instrument via intradayCloses. Feeds the decision composite.
    void fetchIntradaySeries();
    // The REFERENCE series that are not tradable instruments but say what the index
    // instruments are doing: the two volatility indices (^VIX, ^VXN), the US 10-year
    // yield (^TNX) and the top-ten constituents of BOTH indices, whose participation is
    // the closest stand-in for breadth this app can fetch (REQ-F-035). The two lists
    // share eight megacaps, so the union is 15 tickers, and each index's read picks its
    // own ten. Results arrive per ticker via referenceSeries.
    void fetchReferenceSeries();

signals:
    // CBOE VIX ("fear index") level and its change vs. its multi-month average, in percent.
    void vixUpdated(double level, double changePct);
    // Real-time technical rating from the web (TradingView) for the current
    // instrument: available=false when the instrument has no mapped web symbol;
    // otherwise score is in [-1,1] (Strong Sell ... Strong Buy) with a text rating.
    void externalSignalUpdated(bool available, double score, const QString &rating);
    // TradingView multi-timeframe technical rating for each tradable instrument with a
    // mapped web ticker, keyed by app symbol (instruments without a ticker are omitted).
    void instrumentRatingsUpdated(const QHash<QString, WebRating> &ratingBySymbol);
    // Recent news headlines for one tradable instrument (newest first).
    void instrumentNewsUpdated(const QString &symbol, const QList<NewsHeadline> &headlines);
    // One reference ticker's 1-minute session closes, keyed by its Yahoo ticker
    // ("^VXN", "^TNX", "MSFT", …) rather than by an app instrument.
    void referenceSeries(const QString &ticker, const QList<double> &closes);
    // CNN Fear & Greed index — what the trading crowd is doing right now:
    // 0 (extreme fear) .. 100 (extreme greed), with CNN's own rating word.
    void fearGreedUpdated(double score, const QString &rating);
    // Independent Yahoo Finance quote for the current instrument, with the
    // exchange timestamp — a freshness/level cross-check on the eToro rate.
    // Not emitted for instruments without a mapped Yahoo ticker.
    void webQuoteUpdated(const QString &symbol, double price, const QDateTime &asOf);
    // Yahoo intraday close series (1-minute bars) for one tradable instrument —
    // the independent session-momentum source of the decision composite.
    void intradayCloses(const QString &symbol, const QList<double> &closes);
    // Fetch failures, throttled to one line per feed per 10 minutes: these are
    // public web feeds that hiccup routinely (the panels keep their last
    // reading), but a feed that is DOWN must not fail silently — the user
    // should know the VIX/news/sentiment inputs have gone stale.
    void log(const QString &message, bool isError);

private:
    // The URL for one feed request: the real host + pathAndQuery, or the test
    // override + pathAndQuery when setEndpointBaseForTesting() was called.
    [[nodiscard]] QString feedUrl(const QString &host, const QString &pathAndQuery) const;
    // Emit a throttled log line for a failed feed fetch (see the log signal).
    void reportFeedError(const QString &feed, const QString &detail);
    // Fetch the spot CBOE VIX from a free public feed (eToro only lists monthly
    // VIX futures) and emit vixUpdated.
    void fetchVix();
    // Fetch the current instrument's real-time technical rating from TradingView
    // and emit externalSignalUpdated.
    void fetchExternalSignal();
    // Fetch the CNN Fear & Greed index and emit fearGreedUpdated.
    void fetchFearGreed();
    // Fetch the current instrument's Yahoo Finance quote and emit webQuoteUpdated.
    void fetchWebQuote();

    QNetworkAccessManager *m_nam = nullptr;
    JsonHttp *m_http = nullptr;
    QTimer *m_timer = nullptr;      // periodic VIX + current-instrument rating refresh
    QStringList m_tradableSymbols;  // instruments the bulk fetches cover
    QString m_currentSymbol;        // instrument the single-rating fetch tracks
    QString m_endpointBaseForTesting;          // empty = the real feed hosts
    QHash<QString, qint64> m_lastFeedErrorMs;  // feed name -> last reported (throttle)
};

#endif // TRADINGAPP_SERVICES_MARKETFEEDS_H
