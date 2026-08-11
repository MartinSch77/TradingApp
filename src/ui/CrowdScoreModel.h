// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_CROWDSCOREMODEL_H
#define TRADINGAPP_UI_CROWDSCOREMODEL_H

#include "domain/CrowdScore.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

// The Qt-facing view-model for the transparent Crowd Score (REQ-F-040, Phase 2). Like
// CockpitModel it is a plain QObject that COMPUTES NOTHING: the score is produced by the pure
// domain/services code and pushed in with setResult(); this only shapes it for binding. So it is
// unit-tested with no rendering and can be driven from mock data. Direction and freshness are
// carried by WORDS (and, in a view, a glyph), never colour alone, and the AI/model distinction
// does not arise here because this is the deterministic rule-based score, labelled as such.
namespace trading::ui {

class CrowdScoreModel : public QObject
{
    Q_OBJECT;   // ";" so tree-sitter/moc see the anchor

    Q_PROPERTY(bool hasData READ hasData NOTIFY changed)
    Q_PROPERTY(double score READ score NOTIFY changed)
    Q_PROPERTY(QString direction READ direction NOTIFY changed)
    Q_PROPERTY(double confidence READ confidence NOTIFY changed)
    Q_PROPERTY(double coverage READ coverage NOTIFY changed)
    Q_PROPERTY(QString headline READ headline NOTIFY changed)
    Q_PROPERTY(QVariantList components READ components NOTIFY changed)   // per-factor contributions
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed)
    Q_PROPERTY(qint32 version READ version NOTIFY changed)

public:
    explicit CrowdScoreModel(QObject *parent = nullptr);

    void setResult(const trading::crowd::CrowdScoreResult &result);

    [[nodiscard]] bool hasData() const { return !m_result.isEmpty(); }
    [[nodiscard]] double score() const { return m_result.score; }
    [[nodiscard]] QString direction() const { return m_result.direction; }
    [[nodiscard]] double confidence() const { return m_result.confidence; }
    [[nodiscard]] double coverage() const { return m_result.coverage; }
    [[nodiscard]] QString headline() const { return m_result.headline(); }
    [[nodiscard]] QVariantList components() const;
    [[nodiscard]] QStringList warnings() const { return m_result.warnings; }
    [[nodiscard]] qint32 version() const { return m_result.version; }

signals:
    void changed();

private:
    trading::crowd::CrowdScoreResult m_result;
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_CROWDSCOREMODEL_H
