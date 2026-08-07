// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/DecisionLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace trading {

namespace {

// Fixed-width columns up to the reason, so a person scanning a week of these can see the
// shape of a day without reading every sentence. The reason is last because it is the only
// field with no useful maximum length.
constexpr qint32 kSymbolWidth = 12;

QString sideWord(qint32 dir, bool traded)
{
    if (dir > 0) {
        return QStringLiteral("long");
    }
    if (dir < 0) {
        return QStringLiteral("short");
    }
    // A refusal can legitimately have no side — the gate may have refused before any
    // direction existed. Saying "no side" is the honest rendering; "long" would be a lie
    // and an empty column would read as a formatting bug.
    return traded ? QStringLiteral("long?") : QStringLiteral("no side");
}

// What the evidence was, in the same words the window uses: how many reads agreed out of
// how many could be measured. "6 of 9" and "6 of 9 (3 unmeasurable)" are different facts
// and the log must not collapse them.
QString evidenceOf(const DecisionNote &note)
{
    const qint32 total = note.leadMeasured + note.leadUnknowns;
    if (total <= 0) {
        return QStringLiteral("no reads evaluated");
    }
    QString text = QStringLiteral("strength %1, %2 of %3 reads measured")
                       .arg(note.leadStrength, 0, 'f', 0)
                       .arg(note.leadMeasured)
                       .arg(total);
    if (note.leadUnknowns > 0) {
        text += QStringLiteral(" (%1 unmeasurable)").arg(note.leadUnknowns);
    }
    return text;
}

} // namespace

QString decisionLogHeader()
{
    return QStringLiteral(
        "# TradingApp bot decision log — every instrument the bot CONSIDERED, and why it\n"
        "# was or was not traded. SIMULATED money on live prices; this file never means a\n"
        "# real order was placed.\n"
        "#\n"
        "# <UTC timestamp>  <symbol>  TRADED|REFUSED  <side>  <detail>  — <reason>\n"
        "#\n"
        "# TRADED lines carry the geometry that was opened (stake, leverage, fill, stop,\n"
        "# target, opening cost). REFUSED lines carry the refusal CODE, which is stable and\n"
        "# countable — the same codes the scan summary tallies.\n"
        "#\n"
        "# A refused instrument is not a failure: staying out is a decision, and the reason\n"
        "# it stayed out is the point of this file. prediction-ledger.jsonl records the same\n"
        "# decisions in machine-readable form for measuring how good they were.\n");
}

QString decisionLine(const DecisionNote &note)
{
    if (!note.isValid()) {
        return {};
    }
    const QString stamp = note.at.toUTC().toString(Qt::ISODate);
    const QString symbol = note.symbol.leftJustified(kSymbolWidth, u' ');
    const QString what = note.traded ? QStringLiteral("TRADED ") : QStringLiteral("REFUSED");
    const QString side = sideWord(note.dir, note.traded).leftJustified(7, u' ');

    QString detail;
    if (note.traded) {
        detail = QStringLiteral("%1 EUR x%2 @ %3 · SL %4 · TP %5 · open cost %6 · conf %7 · %8")
                     .arg(note.stake, 0, 'f', 2)
                     .arg(note.leverage)
                     .arg(note.fillRate, 0, 'f', 4)
                     .arg(note.slRate, 0, 'f', 4)
                     .arg(note.tpRate, 0, 'f', 4)
                     .arg(note.openCost, 0, 'f', 2)
                     .arg(note.confidence, 0, 'f', 0)
                     .arg(evidenceOf(note));
    } else {
        // The CODE first: it is what makes a week of refusals countable, and a reader
        // grepping for one rule wants it in a fixed position rather than inside prose.
        detail = QStringLiteral("%1 · conf %2 · %3")
                     .arg(note.code.isEmpty() ? QStringLiteral("(no code)") : note.code)
                     .arg(note.confidence, 0, 'f', 0)
                     .arg(evidenceOf(note));
    }

    QString line = QStringLiteral("%1  %2  %3  %4  %5").arg(stamp, symbol, what, side, detail);
    if (!note.why.isEmpty()) {
        // The rule's own sentence, unedited. It is the part that explains rather than
        // labels, so it is never truncated to keep the line tidy.
        line += QStringLiteral("  — %1").arg(note.why);
    }
    return line;
}

bool appendDecision(const QString &path, const DecisionNote &note)
{
    if (path.isEmpty() || !note.isValid()) {
        return false;
    }
    const QString line = decisionLine(note);
    if (line.isEmpty()) {
        return false;
    }
    const QFileInfo info(path);
    if (!info.dir().exists() && !QDir().mkpath(info.absolutePath())) {
        return false;
    }
    const bool fresh = !info.exists();
    // Plain append rather than QSaveFile's rewrite: this file grows for weeks and
    // rewriting it per decision would turn a log line into a whole-file copy.
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    if (fresh) {
        out << decisionLogHeader();
    }
    out << line << '\n';
    return out.status() == QTextStream::Ok;
}

} // namespace trading
