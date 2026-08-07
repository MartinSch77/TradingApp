// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/TradeScript.h"

#include <algorithm>

namespace {

using trading::ScriptEntry;

// Accept "yyyy-MM-dd HH:mm" or a bare "yyyy-MM-dd" (midnight): a date-only
// window bound is natural in a hand-written file ("trade on Friday").
QDateTime parseStamp(const QString &s)
{
    QDateTime t = QDateTime::fromString(s, QStringLiteral("yyyy-MM-dd HH:mm"));
    if (!t.isValid()) {
        t = QDateTime::fromString(s, QStringLiteral("yyyy-MM-dd"));
    }
    return t;
}

// A positive, locale-independent number ("." decimal point, as documented).
bool parsePositive(const QString &s, double &out)
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (ok && (v > 0.0)) {
        out = v;
        return true;
    }
    return false;
}

// The side field: "BUY @ 6100" / "SELL @ 2500" (case-insensitive, @ required).
bool parseSideField(const QString &field, ScriptEntry &e, QString &err)
{
    const qsizetype at = field.indexOf(QLatin1Char('@'));
    if (at < 0) {
        err = QStringLiteral("expected BUY @ <trigger> or SELL @ <trigger>");
        return false;
    }
    const QString side = field.left(at).trimmed().toUpper();
    if (side == QStringLiteral("BUY")) {
        e.isBuy = true;
    } else if (side == QStringLiteral("SELL")) {
        e.isBuy = false;
    } else {
        err = QStringLiteral("side must be BUY or SELL");
        return false;
    }
    if (!parsePositive(field.mid(at + 1).trimmed(), e.trigger)) {
        err = QStringLiteral("trigger rate must be a positive number");
        return false;
    }
    return true;
}

// The two valueless flags. `handled` distinguishes "not this kind of field"
// from a real error, so the dispatcher below stays a flat chain.
bool parseFlagField(const QString &keyword, const QString &value, ScriptEntry &e,
                    bool &handled, QString &err)
{
    handled = (keyword == QStringLiteral("SIGNALS")) || (keyword == QStringLiteral("TRAILING"));
    if (!handled) {
        return false;
    }
    if (!value.isEmpty()) {
        err = QStringLiteral("%1 takes no value").arg(keyword);
        return false;
    }
    if (keyword == QStringLiteral("SIGNALS")) {
        e.requireSignals = true;
    } else {
        e.trailing = true;
    }
    return true;
}

// The window bounds (FROM/TO, full stamp or bare date).
bool parseWindowField(const QString &keyword, const QString &value, ScriptEntry &e,
                      bool &handled, QString &err)
{
    handled = (keyword == QStringLiteral("FROM")) || (keyword == QStringLiteral("TO"));
    if (!handled) {
        return false;
    }
    const QDateTime t = parseStamp(value);
    if (!t.isValid()) {
        err = QStringLiteral("%1 needs yyyy-MM-dd HH:mm (or a bare date)").arg(keyword);
        return false;
    }
    ((keyword == QStringLiteral("FROM")) ? e.from : e.to) = t;
    return true;
}

// The numeric fields: the three positive amounts and the integer leverage.
bool parseNumberField(const QString &keyword, const QString &value, ScriptEntry &e,
                      bool &handled, QString &err)
{
    handled = true;
    if (keyword == QStringLiteral("LEV")) {
        bool ok = false;
        const qint32 lev = value.toInt(&ok);
        if (!ok || (lev < 1)) {
            err = QStringLiteral("LEV needs a whole number >= 1");
            return false;
        }
        e.leverage = lev;
        return true;
    }
    double v = 0.0;
    const bool numeric = parsePositive(value, v);
    if (keyword == QStringLiteral("AMOUNT")) {
        e.amount = v;
    } else if (keyword == QStringLiteral("SL")) {
        e.slAmount = v;
    } else if (keyword == QStringLiteral("TP")) {
        e.tpAmount = v;
    } else {
        handled = false;
        return false;
    }
    if (!numeric) {
        err = QStringLiteral("%1 needs a positive number").arg(keyword);
        return false;
    }
    return true;
}

// One optional keyworded field. `seen` rejects duplicates: a line saying
// "AMOUNT 100; AMOUNT 500" has exactly one honest reading — the author's
// mistake — and mistakes in a file that trades money must not be guessed at.
bool parseKeywordField(const QString &field, ScriptEntry &e, QStringList &seen, QString &err)
{
    const QString upper = field.toUpper();
    const QString keyword = upper.section(QLatin1Char(' '), 0, 0);
    if (seen.contains(keyword)) {
        err = QStringLiteral("duplicate field '%1'").arg(keyword);
        return false;
    }
    seen << keyword;
    const QString value = field.mid(keyword.size()).trimmed();

    bool handled = false;
    const bool flagOk = parseFlagField(keyword, value, e, handled, err);
    if (handled) {
        return flagOk;
    }
    const bool windowOk = parseWindowField(keyword, value, e, handled, err);
    if (handled) {
        return windowOk;
    }
    const bool numberOk = parseNumberField(keyword, value, e, handled, err);
    if (handled) {
        return numberOk;
    }
    err = QStringLiteral("unknown field '%1'").arg(keyword);
    return false;
}

// One non-comment line into an entry; on failure `err` says what and where.
bool parseLine(const QString &line, qint32 lineNumber, ScriptEntry &e, QString &err)
{
    const QStringList fields = line.split(QLatin1Char(';'));
    if (fields.size() < 2) {
        err = QStringLiteral("expected at least <instrument>; BUY|SELL @ <trigger>");
        return false;
    }
    e.symbol = fields.at(0).trimmed();
    if (e.symbol.isEmpty()) {
        err = QStringLiteral("instrument symbol is empty");
        return false;
    }
    if (!parseSideField(fields.at(1).trimmed(), e, err)) {
        return false;
    }
    QStringList seen;
    for (qsizetype i = 2; i < fields.size(); ++i) {
        const QString field = fields.at(i).trimmed();
        if (field.isEmpty()) {
            continue;  // a trailing ';' is harmless
        }
        if (!parseKeywordField(field, e, seen, err)) {
            return false;
        }
    }
    if (e.amount <= 0.0) {
        err = QStringLiteral("AMOUNT is required (the stake to trade)");
        return false;
    }
    if (e.from.isValid() && e.to.isValid() && (e.from > e.to)) {
        err = QStringLiteral("FROM is after TO");
        return false;
    }
    e.lineNumber = lineNumber;
    e.sourceLine = line;
    return true;
}

} // namespace

namespace trading {

ScriptParseResult parseTradeScript(const QString &text)
{
    ScriptParseResult result;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        // Strip a comment (whole-line or trailing) and surrounding whitespace.
        const qsizetype hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0) {
            line.truncate(hash);
        }
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        ScriptEntry e;
        QString err;
        if (parseLine(line, static_cast<qint32>(i + 1), e, err)) {
            result.entries.append(e);
        } else {
            result.errors << QStringLiteral("line %1: %2: %3").arg(i + 1).arg(err, line);
        }
    }
    // All-or-nothing (REQ-F-028): errors empty = the file is accepted as a whole.
    result.ok = result.errors.isEmpty();
    if (!result.ok) {
        result.entries.clear();
    }
    return result;
}

qint32 snapLeverage(qint32 requested, const QList<qint32> &offered)
{
    if (offered.isEmpty()) {
        return requested;
    }
    // Highest offered value not exceeding the request; the lowest offered one
    // when nothing qualifies (see the header for the rationale).
    qint32 best = 0;
    qint32 lowest = offered.first();
    for (const qint32 step : offered) {
        lowest = std::min(lowest, step);
        if ((step <= requested) && (step > best)) {
            best = step;
        }
    }
    return (best > 0) ? best : lowest;
}

bool scriptEntryShouldRest(const ScriptEntry &entry, const QDateTime &now,
                           const ScriptSignalState &sources)
{
    if (scriptEntryExpired(entry, now)) {
        return false;
    }
    if (entry.from.isValid() && (now < entry.from)) {
        return false;  // window not open yet
    }
    if (entry.requireSignals) {
        // Both sources must favour the side; an unconfigured AI is no
        // recommendation, so the entry waits rather than trade on half the
        // evidence (REQ-F-028).
        const qint32 side = entry.isBuy ? 1 : -1;
        if (!sources.aiConfigured || (sources.ensembleDir != side)
            || (sources.aiDir != side)) {
            return false;
        }
    }
    return true;
}

bool scriptEntryExpired(const ScriptEntry &entry, const QDateTime &now)
{
    return entry.to.isValid() && (now > entry.to);
}

} // namespace trading
