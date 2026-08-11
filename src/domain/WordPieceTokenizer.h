// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_WORDPIECETOKENIZER_H
#define TRADINGAPP_DOMAIN_WORDPIECETOKENIZER_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// WordPiece tokenization for the local text-sentiment model (REQ-F-044, Phase 6) — the PURE
// half, testable everywhere. A tokenizer that does not match its model scores noise with
// confidence, so this follows BERT's reference behaviour: lowercase (uncased models), split
// punctuation into its own tokens, then greedy longest-match subwords with the "##"
// continuation prefix, unknown words to [UNK], and the [CLS] … [SEP] classifier framing.
namespace trading::crowd {

// The vocabulary read from the model's own vocab.txt (token per line, id = line number).
// Not-ok when a required special token is missing — encoding against a wrong vocabulary is
// refused, never approximated.
struct WordPieceVocab {
    bool ok = false;
    QString error;
    QHash<QString, qint32> ids;
    qint32 cls = -1;
    qint32 sep = -1;
    qint32 unk = -1;
    bool lowerCase = true;   // financial BERTs are uncased; a cased model would set this false
};

[[nodiscard]] WordPieceVocab wordPieceVocabFromLines(const QStringList &lines);

// Encode one text as [CLS] pieces… [SEP], truncated so at most `maxTokens` ids come back
// (the [SEP] survives truncation — a classifier input must stay well-formed).
[[nodiscard]] QList<qint32> wordPieceEncode(const WordPieceVocab &vocab, const QString &text,
                                            qint32 maxTokens);

} // namespace trading::crowd

#endif // TRADINGAPP_DOMAIN_WORDPIECETOKENIZER_H
