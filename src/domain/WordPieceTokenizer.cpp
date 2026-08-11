// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/WordPieceTokenizer.h"

namespace trading::crowd {

namespace {

// A word the vocabulary cannot begin is [UNK] WHOLE — BERT's rule; emitting the matched
// prefix pieces of a half-known word would encode a different word than the text held.
constexpr qint32 kMaxWordChars = 100;

QStringList basicTokens(const QString &text, bool lowerCase)
{
    const QString prepared = lowerCase ? text.toLower() : text;
    QStringList words;
    QString current;
    for (const QChar ch : prepared) {
        if (ch.isSpace()) {
            if (!current.isEmpty()) {
                words.append(current);
                current.clear();
            }
            continue;
        }
        if (ch.isPunct() || ch.isSymbol()) {
            if (!current.isEmpty()) {
                words.append(current);
                current.clear();
            }
            words.append(QString(ch));   // punctuation is its own token, BERT-style
            continue;
        }
        current.append(ch);
    }
    if (!current.isEmpty()) {
        words.append(current);
    }
    return words;
}

void appendWordPieces(const WordPieceVocab &vocab, const QString &word, QList<qint32> *out)
{
    if (word.size() > kMaxWordChars) {
        out->append(vocab.unk);
        return;
    }
    QList<qint32> pieces;
    qsizetype start = 0;
    while (start < word.size()) {
        qsizetype end = word.size();
        qint32 pieceId = -1;
        // Greedy LONGEST match first — the reference WordPiece behaviour.
        while (end > start) {
            QString candidate = word.mid(start, end - start);
            if (start > 0) {
                candidate.prepend(QStringLiteral("##"));
            }
            const auto it = vocab.ids.constFind(candidate);
            if (it != vocab.ids.constEnd()) {
                pieceId = it.value();
                break;
            }
            --end;
        }
        if (pieceId < 0) {
            out->append(vocab.unk);
            return;
        }
        pieces.append(pieceId);
        start = end;
    }
    out->append(pieces);
}

} // namespace

WordPieceVocab wordPieceVocabFromLines(const QStringList &lines)
{
    WordPieceVocab vocab;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString token = lines.at(i).trimmed();
        if (!token.isEmpty()) {
            vocab.ids.insert(token, static_cast<qint32>(i));
        }
    }
    vocab.cls = vocab.ids.value(QStringLiteral("[CLS]"), -1);
    vocab.sep = vocab.ids.value(QStringLiteral("[SEP]"), -1);
    vocab.unk = vocab.ids.value(QStringLiteral("[UNK]"), -1);
    if (vocab.cls < 0 || vocab.sep < 0 || vocab.unk < 0) {
        vocab.error = QStringLiteral("the vocabulary lacks a required special token "
                                     "([CLS]/[SEP]/[UNK]) — not a classifier vocabulary");
        return vocab;
    }
    vocab.ok = true;
    return vocab;
}

QList<qint32> wordPieceEncode(const WordPieceVocab &vocab, const QString &text,
                              qint32 maxTokens)
{
    QList<qint32> ids;
    if (!vocab.ok || maxTokens < 3) {
        return ids;   // an unusable vocabulary encodes nothing, never something approximate
    }
    ids.append(vocab.cls);
    const QStringList words = basicTokens(text, vocab.lowerCase);
    for (const QString &word : words) {
        appendWordPieces(vocab, word, &ids);
    }
    // Truncate the BODY, keep the framing: [CLS] … [SEP] must survive whole.
    if (ids.size() > maxTokens - 1) {
        ids = ids.mid(0, maxTokens - 1);
    }
    ids.append(vocab.sep);
    return ids;
}

} // namespace trading::crowd
