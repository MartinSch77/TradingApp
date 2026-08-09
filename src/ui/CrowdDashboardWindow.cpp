// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CrowdDashboardWindow.h"

#include "services/CrowdCollector.h"
#include "services/OllamaAdvisor.h"

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

CrowdDashboardWindow::CrowdDashboardWindow(CrowdCollector *collector, OllamaAdvisor *ollama,
                                           QWidget *parent)
    : QDialog(parent), m_collector(collector), m_ollama(ollama)
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
    // Parentless, installed via setLayout below: the unconditional ownership transfer is
    // also what lets the static analyzer see the layout cannot leak on any path.
    auto *scoreLayout = new QVBoxLayout;
    for (const QString &instrument : CrowdCollector::instruments()) {
        auto *scoreLabel = new QLabel(scoreBox);
        scoreLabel->setObjectName(QStringLiteral("crowdScoreLabel_") + instrument);
        scoreLabel->setWordWrap(true);
        scoreLabel->setText(instrument + QStringLiteral(": no score computed yet"));
        scoreLayout->addWidget(scoreLabel);
        m_scoreLabels.insert(instrument, scoreLabel);
    }
    scoreBox->setLayout(scoreLayout);
    layout->addWidget(scoreBox);

    auto *modelBox = new QGroupBox(QStringLiteral("Trained model (evidence only)"), this);
    modelBox->setObjectName(QStringLiteral("crowdModelBox"));
    auto *modelLayout = new QVBoxLayout(modelBox);
    m_modelStatusLabel = new QLabel(modelBox);
    m_modelStatusLabel->setObjectName(QStringLiteral("crowdModelStatusLabel"));
    m_modelStatusLabel->setWordWrap(true);
    modelLayout->addWidget(m_modelStatusLabel);
    for (const QString &instrument : CrowdCollector::instruments()) {
        auto *predictionLabel = new QLabel(modelBox);
        predictionLabel->setObjectName(QStringLiteral("crowdPredictionLabel_") + instrument);
        predictionLabel->setWordWrap(true);
        predictionLabel->setText(instrument + QStringLiteral(": no prediction"));
        modelLayout->addWidget(predictionLabel);
        m_predictionLabels.insert(instrument, predictionLabel);
    }
    layout->addWidget(modelBox);

    // The optional local-model explanation (REQ-F-045): WORDS about the evidence above,
    // displayed and consumed by NOTHING — no parser, no decision path.
    auto *explainBox = new QGroupBox(QStringLiteral("Local model's words (never data)"), this);
    explainBox->setObjectName(QStringLiteral("crowdExplainBox"));
    auto *explainLayout = new QVBoxLayout(explainBox);
    m_explainButton = new QPushButton(QStringLiteral("Explain this evidence"), explainBox);
    m_explainButton->setObjectName(QStringLiteral("crowdExplainButton"));
    m_explanationLabel = new QLabel(explainBox);
    m_explanationLabel->setObjectName(QStringLiteral("crowdExplanationLabel"));
    m_explanationLabel->setWordWrap(true);
    const bool haveModel = m_ollama != nullptr && m_ollama->isConfigured();
    m_explainButton->setEnabled(haveModel);
    m_explanationLabel->setText(
        haveModel ? QStringLiteral("Ask %1 to put the evidence above into words.")
                        .arg(m_ollama->model())
                  : QStringLiteral("no local model configured (./setup.sh ollama, REQ-F-030)"));
    explainLayout->addWidget(m_explainButton);
    explainLayout->addWidget(m_explanationLabel);
    layout->addWidget(explainBox);
    if (haveModel) {
        static_cast<void>(connect(m_explainButton, &QPushButton::clicked, this,
                                  &CrowdDashboardWindow::onExplainClicked));
        static_cast<void>(connect(m_ollama, &OllamaAdvisor::explanationReady, this,
                                  &CrowdDashboardWindow::onExplanationReady));
    }

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

QString CrowdDashboardWindow::shownEvidence() const
{
    QStringList lines{QStringLiteral("Data providers:"), m_providersLabel->text(),
                      QStringLiteral("Crowd score:")};
    for (auto it = m_scoreLabels.constBegin(); it != m_scoreLabels.constEnd(); ++it) {
        lines.append(it.value()->text());
    }
    lines.append(QStringLiteral("Trained model: ") + m_modelStatusLabel->text());
    for (auto it = m_predictionLabels.constBegin(); it != m_predictionLabels.constEnd(); ++it) {
        lines.append(it.value()->text());
    }
    return lines.join(QLatin1Char('\n'));
}

void CrowdDashboardWindow::onExplainClicked()
{
    m_explainButton->setEnabled(false);
    m_explanationLabel->setText(QStringLiteral("asking %1…").arg(m_ollama->model()));
    m_ollama->requestExplanation(shownEvidence());
}

void CrowdDashboardWindow::onExplanationReady(const QString &explanation, const QString &error)
{
    m_explainButton->setEnabled(true);
    if (!error.isEmpty()) {
        m_explanationLabel->setText(error);
        return;
    }
    // Displayed, never parsed: the caveat is part of the text so a screenshot carries it too.
    m_explanationLabel->setText(
        QStringLiteral("%1 says (words, not data — numbers in prose are not measurements): %2")
            .arg(m_ollama->model(), explanation));
}
