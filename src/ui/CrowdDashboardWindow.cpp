// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CrowdDashboardWindow.h"

#include "services/CrowdCollector.h"

#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

using trading::crowd::CollectorProviderStatus;
using trading::crowd::CrowdCollector;
using trading::crowd::CrowdPrediction;
using trading::crowd::CrowdScoreResult;

namespace {

QString scoreText(const CrowdScoreResult &result)
{
    if (result.isEmpty()) {
        return QStringLiteral("no measured data yet — every family is missing");
    }
    QString text = result.headline();
    if (!result.warnings.isEmpty()) {
        text += QStringLiteral("\n  ") + result.warnings.join(QStringLiteral("\n  "));
    }
    return text;
}

QString predictionText(const CrowdPrediction &prediction)
{
    if (!prediction.ok) {
        return prediction.error;
    }
    QStringList parts;
    for (qsizetype i = 0; i < prediction.classes.size(); ++i) {
        parts.append(QStringLiteral("%1 %2%")
                         .arg(prediction.classes.at(i))
                         .arg(prediction.probabilities.at(i) * 100.0, 0, 'f', 1));
    }
    // The imputation count is part of the answer: a prediction made mostly of fill-ins is a
    // weaker claim, and hiding that would dress it up as a measurement.
    return QStringLiteral("%1 — top %2 (%3 input(s) imputed by the model's own medians)")
        .arg(parts.join(QStringLiteral(" · ")), prediction.topClass)
        .arg(prediction.imputed);
}

} // namespace

CrowdDashboardWindow::CrowdDashboardWindow(CrowdCollector *collector, QWidget *parent)
    : QDialog(parent), m_collector(collector)
{
    setObjectName(QStringLiteral("crowdDashboardWindow"));
    setWindowTitle(QStringLiteral("Crowd & AI — evidence only"));
    auto *layout = new QVBoxLayout(this);

    m_disclaimerLabel = new QLabel(
        QStringLiteral("Experimental crowd signals. Not financial advice; probabilities can be "
                       "wrong. Nothing here places, sizes or stops a trade."),
        this);
    m_disclaimerLabel->setObjectName(QStringLiteral("crowdDisclaimerLabel"));
    m_disclaimerLabel->setWordWrap(true);
    layout->addWidget(m_disclaimerLabel);

    auto *providersBox = new QGroupBox(QStringLiteral("Data providers"), this);
    providersBox->setObjectName(QStringLiteral("crowdProvidersBox"));
    auto *providersLayout = new QVBoxLayout(providersBox);
    m_providersLabel = new QLabel(providersBox);
    m_providersLabel->setObjectName(QStringLiteral("crowdProvidersLabel"));
    m_providersLabel->setWordWrap(true);
    providersLayout->addWidget(m_providersLabel);
    layout->addWidget(providersBox);

    auto *scoreBox = new QGroupBox(QStringLiteral("Transparent crowd score (the baseline)"),
                                   this);
    scoreBox->setObjectName(QStringLiteral("crowdScoreBox"));
    auto *scoreLayout = new QVBoxLayout(scoreBox);
    auto *modelBox = new QGroupBox(QStringLiteral("Trained model (evidence only)"), this);
    modelBox->setObjectName(QStringLiteral("crowdModelBox"));
    auto *modelLayout = new QVBoxLayout(modelBox);
    m_modelStatusLabel = new QLabel(modelBox);
    m_modelStatusLabel->setObjectName(QStringLiteral("crowdModelStatusLabel"));
    m_modelStatusLabel->setWordWrap(true);
    modelLayout->addWidget(m_modelStatusLabel);
    for (const QString &instrument : CrowdCollector::instruments()) {
        auto *scoreLabel = new QLabel(scoreBox);
        scoreLabel->setObjectName(QStringLiteral("crowdScoreLabel_") + instrument);
        scoreLabel->setWordWrap(true);
        scoreLabel->setText(instrument + QStringLiteral(": no score computed yet"));
        scoreLayout->addWidget(scoreLabel);
        m_scoreLabels.insert(instrument, scoreLabel);

        auto *predictionLabel = new QLabel(modelBox);
        predictionLabel->setObjectName(QStringLiteral("crowdPredictionLabel_") + instrument);
        predictionLabel->setWordWrap(true);
        predictionLabel->setText(instrument + QStringLiteral(": no prediction"));
        modelLayout->addWidget(predictionLabel);
        m_predictionLabels.insert(instrument, predictionLabel);
    }
    layout->addWidget(scoreBox);
    layout->addWidget(modelBox);

    auto *storeLabel = new QLabel(this);
    storeLabel->setObjectName(QStringLiteral("crowdStoreLabel"));
    layout->addWidget(storeLabel);
    m_storeLabel = storeLabel;
    m_refreshButton = new QPushButton(QStringLiteral("Refresh now"), this);
    m_refreshButton->setObjectName(QStringLiteral("crowdRefreshButton"));
    layout->addWidget(m_refreshButton);

    static_cast<void>(connect(m_refreshButton, &QPushButton::clicked, m_collector,
                              &CrowdCollector::refreshNow));
    static_cast<void>(connect(m_collector, &CrowdCollector::statusChanged, this,
                              &CrowdDashboardWindow::onStatusChanged));
    static_cast<void>(connect(m_collector, &CrowdCollector::scoreUpdated, this,
                              &CrowdDashboardWindow::onScoreUpdated));
    static_cast<void>(connect(m_collector, &CrowdCollector::predictionUpdated, this,
                              &CrowdDashboardWindow::onPredictionUpdated));

    // What is already known shows immediately: the persisted score survives restarts.
    onStatusChanged();
    for (const QString &instrument : CrowdCollector::instruments()) {
        onScoreUpdated(instrument, m_collector->store().latestScore(instrument));
    }
}

void CrowdDashboardWindow::onStatusChanged()
{
    QStringList lines;
    const QList<CollectorProviderStatus> statuses = m_collector->providerStatuses();
    for (const CollectorProviderStatus &status : statuses) {
        lines.append(QStringLiteral("%1 — %2 — %3")
                         .arg(status.name,
                              status.configured ? QStringLiteral("configured")
                                                : QStringLiteral("not configured"),
                              status.detail));
    }
    m_providersLabel->setText(lines.join(QLatin1Char('\n')));
    m_modelStatusLabel->setText(m_collector->modelStatus());
    m_storeLabel->setText(QStringLiteral("%1 observation(s) in the raw store")
                              .arg(m_collector->store().count()));
}

void CrowdDashboardWindow::onScoreUpdated(const QString &instrument,
                                          const CrowdScoreResult &result)
{
    QLabel *label = m_scoreLabels.value(instrument);
    if (label != nullptr) {
        label->setText(instrument + QStringLiteral(": ") + scoreText(result));
    }
}

void CrowdDashboardWindow::onPredictionUpdated(const QString &instrument,
                                               const CrowdPrediction &prediction)
{
    QLabel *label = m_predictionLabels.value(instrument);
    if (label != nullptr) {
        label->setText(instrument + QStringLiteral(": ") + predictionText(prediction));
    }
}
