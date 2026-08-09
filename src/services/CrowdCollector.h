// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_CROWDCOLLECTOR_H
#define TRADINGAPP_SERVICES_CROWDCOLLECTOR_H

#include "domain/CrowdInference.h"
#include "domain/CrowdScore.h"
#include "services/Config.h"
#include "services/CrowdModel.h"
#include "services/CrowdStore.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

// The collection loop that turns the crowd subsystem from "built and tested" into "running"
// (REQ-F-043, Phase 7): it owns the real providers, asks the CONFIGURED ones for the focus
// instruments on a timer, persists what arrives into the raw store (idempotently — the dedup
// key makes a repeated fetch a no-op), recomputes and persists the transparent crowd score,
// and, when an exported model is present, scores it through the optional inference seam.
//
// Honesty rules the dashboard depends on: an unconfigured provider is reported UNAVAILABLE in
// words and asked nothing; a provider failure becomes a named status, never a crash and never
// data; and the model inputs this class cannot compute in-process (the price-context features)
// are left MISSING for the model's own embedded imputation — reimplementing them here could
// drift from the trainer's arithmetic, and the imputation COUNT the prediction carries keeps
// the gap visible instead of hidden.
//
// Nothing here can trade: the collector reads public data, writes its own store and emits
// evidence. No EtoroClient, no order type, no path to REQ-N-005's gate.
namespace trading::crowd {

class CrowdHttpProvider;

// One provider's state in words, for the dashboard.
struct CollectorProviderStatus {
    QString name;
    bool configured = false;
    QString detail;   // "unavailable (not configured)", "3 new observations stored", an error
};

class CrowdCollector : public QObject
{
    Q_OBJECT

public:
    // `cfg` supplies the IG credentials (the git-ignored file / TRADINGAPP_IG_*); `storePath`
    // is the SQLite file (the app passes its config dir, tests a temporary one).
    CrowdCollector(const Config &cfg, const QString &storePath,
                   QObject *parent = nullptr);

    // First refresh immediately, then every `refreshMinutes`. Crowd data is slow-moving
    // (COT weekly, VIX daily), so the default is deliberately unhurried.
    void start(qint32 refreshMinutes = 30);
    void refreshNow();

    // The instruments the loop collects for — the two index instruments every crowd series
    // covers today.
    [[nodiscard]] static QStringList instruments();

    [[nodiscard]] QList<CollectorProviderStatus> providerStatuses() const;
    [[nodiscard]] QString modelStatus() const;
    [[nodiscard]] CrowdStore &store();          // the raw layer (the window shows its count)
    [[nodiscard]] ICrowdModel &model();

    // The model input assembled from the store AS OF `nowUtc`, matched BY NAME against the
    // trainer's manifest names (TS-DASH-002 pins the subset relation). Series the store has
    // never seen carry only their `_measured = 0` marker; the price-context features are
    // absent on purpose (see the class comment).
    [[nodiscard]] QHash<QString, double> modelFeaturesFor(const QString &instrument,
                                                          const QDateTime &nowUtc) const;

public slots:
    // Store a batch of observations (the providers' observationsReady feeds this; tests drive
    // it directly with mock-fetched batches) and recompute for the instruments it touched.
    void ingest(const QList<trading::crowd::Observation> &observations);
    // A provider's failure becomes a named status beside the others — never a crash.
    void noteProviderIssue(const QString &providerName, const QString &detail);
    // Recompute + persist the score for one instrument and, when a model is loaded, emit its
    // prediction for the same instant.
    void recomputeFor(const QString &instrument, const QDateTime &nowUtc);

signals:
    void statusChanged();
    void scoreUpdated(const QString &instrument, const trading::crowd::CrowdScoreResult &result);
    void predictionUpdated(const QString &instrument,
                           const trading::crowd::CrowdPrediction &prediction);

private:
    void loadModelIfPresent();

    CrowdStore m_store;
    QList<CrowdHttpProvider *> m_providers;   // owned through QObject parenting
    OnnxCrowdModel m_model;
    QTimer m_timer;
    QHash<QString, QString> m_details;        // provider name -> status words
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_CROWDCOLLECTOR_H
