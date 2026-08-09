// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CrowdCollector.h"

#include "domain/RollingZScore.h"
#include "services/CftcCotProvider.h"
#include "services/CrowdHttpProvider.h"
#include "services/CrowdScoreBuilder.h"
#include "services/FredProvider.h"
#include "services/IgSentimentProvider.h"

#include <QFileInfo>
#include <QStandardPaths>

#include <array>

namespace trading::crowd {

namespace {

struct SeriesSpec {
    Source family;
    const char *seriesId;
    const char *prefix;
};

// Mirrors the SERIES table in tools/ml/crowd_dataset.py — the model matches inputs BY NAME
// against its embedded manifest names, so these prefixes must stay in lockstep with the
// trainer's (TS-DASH-002 pins the subset relation against a pipeline-built manifest).
constexpr std::array<SeriesSpec, 6> kSeries = {{
    {Source::RetailPositioning, "IG-PCT-LONG", "retail_pct_long"},
    {Source::Options, "PUT-CALL", "put_call"},
    {Source::Volatility, "VIX", "vix"},
    {Source::InstitutionalPositioning, "COT-ASSET-MGR-NET", "cot_asset_mgr_net"},
    {Source::InstitutionalPositioning, "COT-LEV-FUND-NET", "cot_lev_fund_net"},
    {Source::Social, "NET-SENTIMENT", "social_net_sentiment"},
}};

// The same prior-history floor the Phase 2 score uses before it trusts a z.
constexpr int kZMinHistory = 3;

} // namespace

CrowdCollector::CrowdCollector(const Config &cfg, const QString &storePath,
                               QObject *parent)
    : QObject(parent), m_store(storePath)
{
    auto *cot = new CftcCotProvider(this);
    auto *fred = new FredProvider(this);
    auto *ig = new IgSentimentProvider(this);
    ig->setCredentials(cfg.igApiKey, cfg.igIdentifier, cfg.igPassword);
    ig->setDemoAccount(cfg.igDemo);
    m_providers = {cot, fred, ig};
    for (CrowdHttpProvider *provider : std::as_const(m_providers)) {
        const QString name = provider->name();
        static_cast<void>(connect(provider, &CrowdHttpProvider::observationsReady, this,
                                  &CrowdCollector::ingest));
        static_cast<void>(connect(provider, &CrowdHttpProvider::providerError, this,
                                  [this, name](const QString &detail) {
                                      noteProviderIssue(name, detail);
                                  }));
    }
    loadModelIfPresent();
    loadFinBertIfPresent();
    static_cast<void>(connect(&m_timer, &QTimer::timeout, this, &CrowdCollector::refreshNow));
}

void CrowdCollector::start(qint32 refreshMinutes)
{
    refreshNow();
    m_timer.start(refreshMinutes * 60 * 1000);
}

void CrowdCollector::refreshNow()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (CrowdHttpProvider *provider : std::as_const(m_providers)) {
        // An unconfigured provider is a recoverable "unavailable", asked NOTHING — the same
        // rule the providers themselves follow, surfaced here in words for the dashboard.
        if (!provider->isConfigured()) {
            m_details.insert(provider->name(),
                             QStringLiteral("unavailable (not configured)"));
            continue;
        }
        m_details.insert(provider->name(), QStringLiteral("refresh requested"));
        for (const QString &instrument : instruments()) {
            provider->refresh(instrument, now);
        }
    }
    emit statusChanged();
}

QStringList CrowdCollector::instruments()
{
    return {QStringLiteral("SPX500"), QStringLiteral("NSDQ100")};
}

QList<CollectorProviderStatus> CrowdCollector::providerStatuses() const
{
    QList<CollectorProviderStatus> statuses;
    statuses.reserve(m_providers.size() + 1);
    for (const CrowdHttpProvider *provider : m_providers) {
        CollectorProviderStatus status;
        status.name = provider->name();
        status.configured = provider->isConfigured();
        status.detail = m_details.value(status.name,
                                        QStringLiteral("no refresh yet"));
        statuses.append(status);
    }
    // The local text-sentiment scorer reports beside the network providers: it feeds the same
    // store, and "not configured" must be readable in the same place.
    CollectorProviderStatus finbert;
    finbert.name = QStringLiteral("FinBERT");
    finbert.configured = m_finbert.ready();
    finbert.detail = m_details.value(finbert.name, m_finbert.status());
    statuses.append(finbert);
    return statuses;
}

QString CrowdCollector::modelStatus() const
{
    return m_model.status();
}

CrowdStore &CrowdCollector::store()
{
    return m_store;
}

ICrowdModel &CrowdCollector::model()
{
    return m_model;
}

QHash<QString, double> CrowdCollector::modelFeaturesFor(const QString &instrument,
                                                        const QDateTime &nowUtc) const
{
    QHash<QString, double> out;
    for (const SeriesSpec &spec : kSeries) {
        const QString prefix = QLatin1String(spec.prefix);
        const QString seriesId = QLatin1String(spec.seriesId);
        const Observation latest = m_store.latest(instrument, spec.family, seriesId);
        if (!latest.valid) {
            // Missing stays missing: only the marker is a measurement ("this series is
            // absent"); value/z/age are left for the model's embedded medians to fill.
            out.insert(prefix + QStringLiteral("_measured"), 0.0);
            continue;
        }
        out.insert(prefix + QStringLiteral("_measured"), 1.0);
        out.insert(prefix + QStringLiteral("_value"), latest.value);
        const QList<double> history =
            m_store.seriesValuesBefore(instrument, spec.family, seriesId, latest.receivedTime);
        const std::optional<double> z = zScore(latest.value, history, kZMinHistory);
        if (z.has_value()) {
            out.insert(prefix + QStringLiteral("_z"), *z);
        }
        out.insert(prefix + QStringLiteral("_age_days"),
                   static_cast<double>(latest.ageSeconds(nowUtc)) / 86400.0);
    }
    // The price-context features (ret_1d/5d/20d_pct, vol_20d_pct) are DELIBERATELY absent:
    // computing them here would be a second implementation of the trainer's arithmetic, free
    // to drift. The model imputes them with its own embedded medians and the prediction
    // carries the count, so the gap stays visible.
    return out;
}

void CrowdCollector::scoreHeadlines(const QString &instrument, const QStringList &headlines)
{
    if (!m_finbert.ready()) {
        return;   // honestly absent — the FinBERT status row carries the reason
    }
    const SocialSentiment sentiment = m_finbert.scoreHeadlines(headlines);
    if (!sentiment.ok) {
        noteProviderIssue(QStringLiteral("FinBERT"), sentiment.error);
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    Observation obs;
    obs.instrument = instrument;
    obs.source = Source::Social;
    obs.sourceName = QStringLiteral("FinBERT");
    obs.seriesId = QStringLiteral("NET-SENTIMENT");
    // Quantized to the HOUR: news is re-polled far more often than sentiment meaningfully
    // moves, and the store's dedup key turns the repeats into no-ops instead of a flood.
    obs.eventTime = QDateTime(now.date(), QTime(now.time().hour(), 0), QTimeZone::UTC);
    obs.receivedTime = now;
    obs.value = sentiment.net;
    obs.unit = QStringLiteral("net");
    obs.valid = true;
    m_details.insert(QStringLiteral("FinBERT"),
                     QStringLiteral("net %1 over %2 headline(s)")
                         .arg(sentiment.net, 0, 'f', 3)
                         .arg(sentiment.scored));
    ingest({obs});
}

void CrowdCollector::ingest(const QList<trading::crowd::Observation> &observations)
{
    if (observations.isEmpty()) {
        return;
    }
    const qint32 stored = m_store.upsert(observations);
    QStringList touched;
    for (const Observation &observation : observations) {
        if (!touched.contains(observation.instrument)) {
            touched.append(observation.instrument);
        }
    }
    m_details.insert(observations.first().sourceName,
                     QStringLiteral("%1 new observation(s) stored").arg(stored));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QString &instrument : std::as_const(touched)) {
        recomputeFor(instrument, now);
    }
    emit statusChanged();
}

void CrowdCollector::noteProviderIssue(const QString &providerName, const QString &detail)
{
    m_details.insert(providerName, detail);
    emit statusChanged();
}

void CrowdCollector::recomputeFor(const QString &instrument, const QDateTime &nowUtc)
{
    const CrowdScoreResult result = buildCrowdScore(m_store, instrument, nowUtc,
                                                    CrowdScoreConfig{});
    static_cast<void>(m_store.saveScore(instrument, result, nowUtc));
    emit scoreUpdated(instrument, result);
    if (m_model.ready()) {
        emit predictionUpdated(instrument, m_model.predict(modelFeaturesFor(instrument, nowUtc)));
    }
}

void CrowdCollector::loadModelIfPresent()
{
    // TRADINGAPP_CROWD_MODEL names the exported .onnx explicitly; without it, the app's own
    // config directory is checked for crowd-model.onnx. No file is not an error — the model
    // seam's own status words already say "no model loaded" (or name the missing runtime).
    QString path = qEnvironmentVariable("TRADINGAPP_CROWD_MODEL");
    if (path.isEmpty()) {
        const QString fallback =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/crowd-model.onnx");
        if (QFileInfo::exists(fallback)) {
            path = fallback;
        }
    }
    if (!path.isEmpty()) {
        static_cast<void>(m_model.load(path));
    }
}

void CrowdCollector::loadFinBertIfPresent()
{
    // TRADINGAPP_FINBERT_DIR names the exported model directory explicitly; without it, the
    // app config dir's finbert/ is checked. Absent is not an error — the status row says so.
    QString dir = qEnvironmentVariable("TRADINGAPP_FINBERT_DIR");
    if (dir.isEmpty()) {
        const QString fallback =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/finbert");
        if (QFileInfo::exists(fallback + QStringLiteral("/model.onnx"))) {
            dir = fallback;
        }
    }
    if (!dir.isEmpty()) {
        static_cast<void>(m_finbert.load(dir));
    }
}

} // namespace trading::crowd
