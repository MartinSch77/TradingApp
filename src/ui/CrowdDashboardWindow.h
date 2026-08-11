// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_CROWDDASHBOARDWINDOW_H
#define TRADINGAPP_UI_CROWDDASHBOARDWINDOW_H

#include "domain/CrowdInference.h"
#include "domain/CrowdScore.h"

#include <QDialog>
#include <QHash>

class OllamaAdvisor;
class QLabel;
class QPushButton;

namespace trading::crowd {
class CrowdCollector;
}

// The Crowd & AI dashboard (REQ-F-043, Phase 7): shows what the collection loop knows and
// COMPUTES NOTHING — provider states in words, the transparent crowd score's own headline and
// warnings, and the optional model's verdict with its imputation count. Deliberately free of
// any trading affordance: the disclaimer is part of the layout, and the only button re-asks
// the providers. Every widget carries a stable objectName (REQ-N-007).
class CrowdDashboardWindow : public QDialog
{
    Q_OBJECT

public:
    explicit CrowdDashboardWindow(trading::crowd::CrowdCollector *collector,
                                  OllamaAdvisor *ollama, QWidget *parent = nullptr);

private slots:
    void onStatusChanged();
    void onExplainClicked();
    void onExplanationReady(const QString &explanation, const QString &error);
    void onScoreUpdated(const QString &instrument,
                        const trading::crowd::CrowdScoreResult &result);
    void onPredictionUpdated(const QString &instrument,
                             const trading::crowd::CrowdPrediction &prediction);

private:
    // The evidence the view currently shows, as one text — exactly what the explanation
    // request carries (REQ-F-045: the words are about what the user sees).
    [[nodiscard]] QString shownEvidence() const;

    trading::crowd::CrowdCollector *m_collector = nullptr;
    OllamaAdvisor *m_ollama = nullptr;   // optional; null or unconfigured disables the button
    QLabel *m_disclaimerLabel = nullptr;
    QLabel *m_providersLabel = nullptr;
    QHash<QString, QLabel *> m_scoreLabels;       // per instrument
    QHash<QString, QLabel *> m_predictionLabels;  // per instrument
    QLabel *m_modelStatusLabel = nullptr;
    QLabel *m_storeLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_explainButton = nullptr;
    QLabel *m_explanationLabel = nullptr;
};

#endif // TRADINGAPP_UI_CROWDDASHBOARDWINDOW_H
