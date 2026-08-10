// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_FINBERTSENTIMENT_H
#define TRADINGAPP_SERVICES_FINBERTSENTIMENT_H

#include "domain/WordPieceTokenizer.h"

#include <QString>
#include <QStringList>

#include <memory>

// The local text-sentiment scorer (REQ-F-044, Phase 6): a financial-domain BERT classifier
// exported to ONNX (tools/ml/export_finbert.py), run in-process over the news headlines the
// app already fetches — the social family without any social-network API, key or scraping.
//
// DOUBLY optional: it needs the REQ-F-042 build-time runtime AND a model directory
// (model.onnx + vocab.txt + labels.txt) the user provisions offline under the model's own
// licence. Absent either, available()/ready() answer false and status() names the remedy.
// The labels file is authoritative for what the columns MEAN — a label set without positive
// and negative is refused, never guessed at by column order.
namespace trading::crowd {

// One scored text: probabilities labelled by the model's own labels file, and the NET
// sentiment (P positive − P negative) in [−1, 1].
struct HeadlineSentiment {
    bool ok = false;
    QString error;
    QStringList labels;
    QList<double> probabilities;
    double net = 0.0;
};

// A batch of headlines reduced to the one number the store keeps.
struct SocialSentiment {
    bool ok = false;
    QString error;
    double net = 0.0;
    qint32 scored = 0;   // how many headlines actually entered the average
};

class FinBertSentiment
{
public:
    FinBertSentiment();
    ~FinBertSentiment();
    FinBertSentiment(const FinBertSentiment &) = delete;
    FinBertSentiment &operator=(const FinBertSentiment &) = delete;
    FinBertSentiment(FinBertSentiment &&) = delete;
    FinBertSentiment &operator=(FinBertSentiment &&) = delete;

    // A property of the BUILD, not of an instance: was the runtime compiled in?
    [[nodiscard]] static bool available();
    [[nodiscard]] bool ready() const;       // is a model loaded and scoreable?
    [[nodiscard]] QString status() const;   // never empty: ready words, or the remedy

    // Load model.onnx + vocab.txt + labels.txt from `directory`. False leaves status()
    // carrying the reason and NO half-usable session behind.
    bool load(const QString &directory);

    [[nodiscard]] HeadlineSentiment scoreText(const QString &text);
    [[nodiscard]] SocialSentiment scoreHeadlines(const QStringList &headlines);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;   // null until a successful load (and always, in a stub build)
    WordPieceVocab m_vocab;
    QStringList m_labels;
    qint32 m_positive = -1;   // indices into the labels/probability columns
    qint32 m_negative = -1;
    QString m_status;
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_FINBERTSENTIMENT_H
