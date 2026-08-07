// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_TRADESCRIPT_H
#define TRADINGAPP_DOMAIN_TRADESCRIPT_H

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// The trade-script format and its execution semantics (REQ-F-028): a plain-
// text file of conditional orders, one per line, executed as broker-side
// limit orders while the user has explicitly armed the script. This module is
// the PURE half — parsing and per-entry decision logic over plain data; the
// ui layer owns the runner that talks to the broker client.
//
// Line format (semicolon-separated; `#` starts a comment; blank lines ignored):
//   <instrument>; BUY @ <trigger> [; FROM yyyy-MM-dd HH:mm] [; TO yyyy-MM-dd HH:mm]
//                [; SIGNALS] [; AMOUNT <stake>] [; SL <amount>] [; TP <amount>]
//                [; TRAILING] [; LEV <n>]
// BUY enters at the trigger rate or LOWER, SELL at the trigger or HIGHER —
// exactly eToro's "mit" limit-order semantics, which is what each entry is
// placed as (REQ-F-027). Keywords are case-insensitive and may appear in any
// order after the first two fields; AMOUNT is required; dates may omit the
// time (midnight is assumed).
namespace trading {

// One parsed script line, as plain data.
struct ScriptEntry {
    QString symbol;
    bool isBuy = true;
    double trigger = 0.0;       // entry rate (BUY: at or lower; SELL: at or higher)
    QDateTime from;             // invalid = place immediately
    QDateTime to;               // invalid = rest until filled or disarmed
    bool requireSignals = false; // place only while ensemble AND AI favour the side
    double amount = 0.0;        // stake, display currency (required)
    double slAmount = 0.0;      // stop-loss amount, 0 = off
    double tpAmount = 0.0;      // take-profit amount, 0 = off
    bool trailing = false;      // the stop-loss trails (only meaningful with SL)
    qint32 leverage = 1;        // requested multiplier; snapped at placement
    qint32 lineNumber = 0;      // 1-based line in the file, for logging
    QString sourceLine;         // the verbatim line, for logging (REQ-F-028)
};

// Loading is all-or-nothing: any unparsable line rejects the whole file, so a
// half-loaded script can never trade lines nobody reviewed (REQ-F-028).
struct ScriptParseResult {
    bool ok = false;
    QList<ScriptEntry> entries;   // empty unless ok
    QStringList errors;           // "line N: <reason>: <content>"
};
[[nodiscard]] ScriptParseResult parseTradeScript(const QString &text);

// The leverage actually placed for a request against the instrument's offered
// values: the highest offered value that does not exceed the request — the
// "next lower" of REQ-F-028 (a request above every offered value lands on the
// highest offered one). When even the lowest offered value exceeds the
// request, that lowest value is returned — the closest the instrument allows;
// the runner logs every substitution. Empty `offered` returns the request.
[[nodiscard]] qint32 snapLeverage(qint32 requested, const QList<qint32> &offered);

// The live inputs a SIGNALS-flagged entry is judged against.
struct ScriptSignalState {
    qint32 ensembleDir = 0;    // technical ensemble: +1 BUY / -1 SELL / 0 neutral
    qint32 aiDir = 0;          // AI advisor's call, same encoding
    bool aiConfigured = false; // no advisor = no recommendation = flagged entries wait
};

// Whether this entry's broker order should be resting right now: inside the
// time window, and (for SIGNALS entries) both sources favouring the side. The
// runner places the order when this turns true and cancels when it turns
// false while the order still rests (REQ-F-028).
[[nodiscard]] bool scriptEntryShouldRest(const ScriptEntry &entry, const QDateTime &now,
                                         const ScriptSignalState &sources);

// Whether the entry's window has closed for good (TO passed): a final state —
// the runner cancels a still-resting order and never re-places it.
[[nodiscard]] bool scriptEntryExpired(const ScriptEntry &entry, const QDateTime &now);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_TRADESCRIPT_H
