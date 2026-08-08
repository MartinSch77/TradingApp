// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/PaperTrader.h"

#include "domain/Indicators.h"
#include "domain/TradePlan.h"

#include <QJsonArray>
#include <QMap>
#include <QTimeZone>
#include <QStringList>
#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace trading {

namespace {

// Reward:risk of the simulated geometry — the take-profit sits this many times
// the stop distance away, the same 1.5 the costed planner proposes (REQ-F-011).
constexpr double kRewardRisk = 1.5;
// Bars of history the volatility estimate uses, and the minimum series length
// the bot insists on before it sizes anything. Hourly closes, so 24 bars ≈ a day.
constexpr qsizetype kVolBars = 24;
constexpr qsizetype kMinCloses = 12;
// Friday, in QDate::dayOfWeek() terms (Mon = 1): the evening whose rollover
// eToro charges at the tripled weekend rate.
constexpr int kFriday = 5;
constexpr int kSaturday = 6;
// Fraction of free cash a single stake may use, so the opening half-spread (at most
// ~0.5% of the stake at the leverage cap) still fits and cash stays >= 0.
constexpr double kCashHeadroom = 0.98;
// Schema version of the persisted books, so a future format change can be
// detected instead of silently mis-read.
constexpr int kBookSchema = 1;

QString jsonStr(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toString();
}

double jsonNum(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toDouble();
}

QDateTime jsonTime(const QJsonObject &obj, const char *key)
{
    const QString text = jsonStr(obj, key);
    return text.isEmpty() ? QDateTime() : QDateTime::fromString(text, Qt::ISODate);
}

// The entry's feature vector as JSON, by NAME — a reordering of
// entryFeatureNames() then cannot silently reinterpret an old file.
QJsonObject featuresToJson(const EntryFeatures &f)
{
    QJsonObject o;
    const QStringList names = entryFeatureNames();
    const QList<double> values = entryFeatureValues(f);
    for (qsizetype i = 0; (i < names.size()) && (i < values.size()); ++i) {
        o.insert(names.at(i), values.at(i));
    }
    return o;
}

EntryFeatures featuresFromJson(const QJsonObject &o)
{
    EntryFeatures f;
    f.confidence = jsonNum(o, "confidence");
    f.volPct = jsonNum(o, "volPct");
    f.stopPct = jsonNum(o, "stopPct");
    f.targetPct = jsonNum(o, "targetPct");
    f.spreadPct = jsonNum(o, "spreadPct");
    f.edgeOverCost = jsonNum(o, "edgeOverCost");
    f.leverage = static_cast<qint32>(jsonNum(o, "leverage"));
    f.dir = static_cast<qint32>(jsonNum(o, "dir"));
    f.hourUtc = static_cast<qint32>(jsonNum(o, "hourUtc"));
    f.dayOfWeek = static_cast<qint32>(jsonNum(o, "dayOfWeek"));
    f.aiBacked = jsonNum(o, "aiBacked") > 0.5;
    return f;
}

QString timeStr(const QDateTime &when)
{
    return when.isValid() ? when.toString(Qt::ISODate) : QString();
}

} // namespace

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

QString closeReasonWord(CloseReason reason)
{
    switch (reason) {
    case CloseReason::StopLoss:
        return QStringLiteral("stop-loss");
    case CloseReason::TakeProfit:
        return QStringLiteral("take-profit");
    case CloseReason::SignalFlip:
        return QStringLiteral("signal flip");
    case CloseReason::MaxHold:
        return QStringLiteral("max hold");
    case CloseReason::CostsExceedEdge:
        return QStringLiteral("carry beats the edge");
    case CloseReason::WeekendCarry:
        return QStringLiteral("weekend carry");
    case CloseReason::AiExit:
        return QStringLiteral("AI says exit");
    case CloseReason::SignalFade:
        return QStringLiteral("signal faded");
    case CloseReason::GiveBack:
        return QStringLiteral("banked before giving it back");
    case CloseReason::DayTarget:
        return QStringLiteral("day target booked");
    case CloseReason::Manual:
        return QStringLiteral("manual");
    case CloseReason::Reset:
        return QStringLiteral("reset");
    case CloseReason::None:
        break;
    }
    return QStringLiteral("open");
}

QString botAiModeWord(BotAiMode mode)
{
    switch (mode) {
    case BotAiMode::Confirm:
        return QStringLiteral("confirm");
    case BotAiMode::Lead:
        return QStringLiteral("lead");
    case BotAiMode::Off:
        break;
    }
    return QStringLiteral("off");
}

QString dayGateWord(DayGate gate)
{
    switch (gate) {
    case DayGate::TargetReached:
        return QStringLiteral("daily target reached");
    case DayGate::LossLimitReached:
        return QStringLiteral("daily loss limit reached");
    case DayGate::Weekend:
        return QStringLiteral("weekend");
    case DayGate::Open:
        break;
    }
    return QStringLiteral("open");
}

namespace {

// Can this instrument be traded at all right now — market, quote, direction?
// Empty code = yes.
EntryVerdict tradabilityVerdict(const CandidateInput &in)
{
    EntryVerdict out;
    if (!in.marketOpen) {
        out.why = QStringLiteral("market closed");
        out.code = QStringLiteral("market-closed");
    } else if (!in.quoteLive) {
        out.why = QStringLiteral("no live quote");
        out.code = QStringLiteral("no-live-quote");
    } else if (in.dir == 0) {
        out.why = QStringLiteral("no directional call");
        out.code = QStringLiteral("no-signal");
    }
    return out;
}

// How much more a candidate must bring to trade inside a loud window. 1.0 in a
// normal one — and never less, whatever the configuration says.
double volatileWindowFactor(SessionPhase phase, const BotConfig &cfg)
{
    return (phase == SessionPhase::Normal) ? 1.0 : std::max(1.0, cfg.volatileWindowFactor);
}

// Instruments the bot is reluctant to trade at all: allowed only when the expected
// move is both big and fast enough to be worth the spread (REQ-F-034). "Expected move
// per hour" is the instrument's own hourly sigma multiplied by the leverage actually
// chosen, i.e. a percentage of the STAKE rather than of the price — which is what
// decides whether a position can outrun its costs in a short time.
EntryVerdict reluctanceVerdict(const CandidateInput &in, const EntrySignal &sig,
                               const BotConfig &cfg)
{
    EntryVerdict out;
    if (!cfg.reluctantSymbols.contains(in.symbol, Qt::CaseInsensitive)) {
        return out;
    }
    const double perHour = sig.volPct * static_cast<double>(sig.leverage);
    if (perHour < cfg.reluctantMinHourlyMovePct) {
        out.why = QStringLiteral("%1 moves %2%% of the stake per hour at x%3 — under the %4%% "
                                 "this instrument has to promise to be worth trading")
                      .arg(in.symbol)
                      .arg(perHour, 0, 'f', 2)
                      .arg(sig.leverage)
                      .arg(cfg.reluctantMinHourlyMovePct, 0, 'f', 2);
        out.code = QStringLiteral("reluctant-symbol");
        return out;
    }
    const double floor = cfg.minConfidence * std::max(1.0, cfg.reluctantConfidenceFactor);
    if (in.confidence < floor) {
        out.why = QStringLiteral("%1 needs %2%% conviction to be worth trading, not %3%%")
                      .arg(in.symbol)
                      .arg(floor, 0, 'f', 0)
                      .arg(in.confidence, 0, 'f', 0);
        out.code = QStringLiteral("reluctant-symbol");
    }
    return out;
}

// Never INTO a fresh break of the session's opening range: the session has just told
// everyone which way it is going (REQ-F-022). Empty code = nothing in the way.
EntryVerdict rangeBreakVerdict(const CandidateInput &in, const BotConfig &cfg)
{
    EntryVerdict out;
    if (cfg.respectOpeningRange && (in.rangeBreakDir != 0) && (in.dir != 0)
        && (in.rangeBreakDir != in.dir)) {
        out.why = QStringLiteral("the session broke its opening range %1 — not trading into it")
                      .arg((in.rangeBreakDir > 0) ? QStringLiteral("UP")
                                                  : QStringLiteral("DOWN"));
        out.code = QStringLiteral("against-range-break");
    }
    return out;
}

// The rules about WHEN, rather than what: the raised conviction bar of a
// loud session window, the per-instrument cooldown after a close, and the pace
// limit on the book as a whole (REQ-F-034). Empty code = nothing in the way.
// The two rules about the CLOCK: a window that is sat out entirely, and the raised
// conviction bar of one that is merely loud (REQ-F-034).
EntryVerdict windowVerdict(const CandidateInput &in, const BotConfig &cfg, SessionPhase phase,
                           double windowFactor)
{
    EntryVerdict out;
    // Sat out rather than sized down, because in both of these the first move
    // regularly reverses in full.
    const bool sitOut = ((phase == SessionPhase::OpeningChaos) && cfg.avoidOpeningChaos)
                        || ((phase == SessionPhase::PolicyWindow) && cfg.avoidPolicyWindow);
    if (sitOut) {
        out.why = QStringLiteral("the %1 is sat out — its first move reverses too often to trade")
                      .arg(sessionPhaseWord(phase));
        out.code = QStringLiteral("volatile-window");
        return out;
    }
    if (in.confidence < (cfg.minConfidence * windowFactor)) {
        out.why = (phase == SessionPhase::Normal)
                      ? QStringLiteral("confidence %1 below the %2 floor")
                            .arg(in.confidence, 0, 'f', 0)
                            .arg(cfg.minConfidence, 0, 'f', 0)
                      : QStringLiteral("confidence %1 below the %2 floor the %3 asks for")
                            .arg(in.confidence, 0, 'f', 0)
                            .arg(cfg.minConfidence * windowFactor, 0, 'f', 0)
                            .arg(sessionPhaseWord(phase));
        out.code = (phase == SessionPhase::Normal) ? QStringLiteral("confidence")
                                                   : QStringLiteral("volatile-window");
    }
    return out;
}

// The rules about HOW OFTEN, and the structure rule that goes with them: the
// per-instrument cooldown, the book-wide pace limit, a fresh opposite range break,
// and how many independent reads agree (REQ-F-034, REQ-F-035).
// How many of the MEASURED independent reads have to agree before an index position may
// be opened (REQ-F-035). Only measured reads count: a requirement satisfied by absent
// feeds would be a requirement in name only.
//
// The bar TRACKS THE NUMBER OF READS rather than being a constant calibrated when there
// were five of them. That is not a refinement, it is a defect fix: "3 agreeing" was a
// MAJORITY of five reads and is a MINORITY of nine, so every read added to REQ-F-035
// silently loosened the gate that is the bot's main protection. A majority of what was
// actually measured cannot drift that way — and it is still clamped to the measured count
// (never unsatisfiable) and still switched off entirely by minAgreeingReads = 0.
EntryVerdict confluenceVerdict(const CandidateInput &in, const BotConfig &cfg)
{
    EntryVerdict out;
    if ((cfg.minAgreeingReads <= 0) || (in.measuredReads <= 0)) {
        return out;
    }
    const qint32 majority = (in.measuredReads + 1) / 2;
    const qint32 needed = std::min(std::max(cfg.minAgreeingReads, majority), in.measuredReads);
    if (in.agreeingReads >= needed) {
        return out;
    }
    out.why = QStringLiteral("only %1 of %2 independent reads agree (needs %3)")
                  .arg(in.agreeingReads)
                  .arg(in.measuredReads)
                  .arg(needed);
    out.code = QStringLiteral("no-confluence");
    return out;
}

EntryVerdict paceVerdict(const CandidateInput &in, const BotConfig &cfg, SessionPhase phase,
                         double windowFactor)
{
    if (const EntryVerdict window = windowVerdict(in, cfg, phase, windowFactor);
        !window.code.isEmpty()) {
        return window;
    }
    EntryVerdict out;
    // An instrument whose position just closed is left alone for a while: every round
    // trip pays two half-spreads, so re-entering at once converts an edge into fees.
    if (in.lastClosedAt.isValid() && in.now.isValid() && (cfg.reentryCooldownMinutes > 0)) {
        const qint64 sinceClose = in.lastClosedAt.secsTo(in.now) / 60;
        if ((sinceClose >= 0) && (sinceClose < cfg.reentryCooldownMinutes)) {
            out.why = QStringLiteral("closed here %1 min ago — waiting out the %2 min cooldown "
                                     "rather than paying the spread again")
                          .arg(sinceClose)
                          .arg(cfg.reentryCooldownMinutes);
            out.code = QStringLiteral("cooldown");
            return out;
        }
    }
    if ((cfg.maxOpensPerHour > 0) && (in.opensLastHour >= cfg.maxOpensPerHour)) {
        out.why = QStringLiteral("%1 positions opened in the last hour — that is the pace limit")
                      .arg(in.opensLastHour);
        out.code = QStringLiteral("pace-limit");
        return out;
    }
    if (const EntryVerdict range = rangeBreakVerdict(in, cfg); !range.code.isEmpty()) {
        return range;
    }
    // The combined indication (REQ-F-036) may VETO, and only that: a signal of real
    // grade pointing the other way is several independent reads plus the constituent
    // field disagreeing with this trade at once. An absent signal (leadDir 0) changes
    // nothing, and a weak one is not evidence enough to overrule the composite.
    if ((in.leadDir != 0) && (in.dir != 0) && (in.leadDir != in.dir)
        && (in.leadStrength >= cfg.leadVetoStrength)) {
        out.why = QStringLiteral("the combined indication points the other way with strength "
                                 "%1 — the reads and the constituent field disagree with this "
                                 "trade")
                      .arg(in.leadStrength, 0, 'f', 0);
        out.code = QStringLiteral("lead-against");
        return out;
    }
    // …and enough independent reads have to agree (its own function: one refusal, one
    // reason, and paceVerdict is already at the complexity the gate allows).
    if (const EntryVerdict reads = confluenceVerdict(in, cfg); !reads.code.isEmpty()) {
        return reads;
    }
    return out;
}

// May the bot add to an instrument it is already in? Adding is allowed (REQ-F-032)
// but only on the model's own initiative and only up to the per-instrument caps —
// stacking on the composite alone would just buy the same opinion three times.
// Returns an empty code when there is nothing in the way.
EntryVerdict stackingVerdict(const CandidateInput &in, const BookState &book,
                             const BotConfig &cfg)
{
    EntryVerdict out;
    if (book.symbol.count <= 0) {
        return out;
    }
    if ((in.dir != 0) && (book.symbol.dir != 0) && (in.dir != book.symbol.dir)) {
        // Holding both sides at once pays the spread twice to own nothing. A change
        // of side is a CLOSE, which the flip and AI-exit rules do.
        out.why = QStringLiteral("already holding the other side — a reversal closes, "
                                 "it does not hedge");
        out.code = QStringLiteral("opposite-open");
        return out;
    }
    if (!in.aiBacked) {
        out.why = QStringLiteral("already holding it (only a fresh AI pick adds to a position)");
        out.code = QStringLiteral("already-holding");
        return out;
    }
    if (book.symbol.count >= cfg.maxPositionsPerSymbol) {
        out.why = QStringLiteral("already %1 positions in it (the per-instrument limit)")
                      .arg(book.symbol.count);
        out.code = QStringLiteral("symbol-count");
    }
    return out;
}

// Why a scan stopped, for the limit that actually bound. Its own function because
// the four cases each need different numbers, and reporting the wrong limit sends
// the reader to tune the wrong knob.
QString roomRefusalWhy(const StakeRoom &room, const BookState &book, const BotConfig &cfg,
                       const QString &symbol)
{
    if (room.limit == QStringLiteral("risk-budget")) {
        return QStringLiteral("risk budget full (%1% of equity at stake)")
            .arg((book.equity > 0.0) ? ((book.openRisk / book.equity) * 100.0) : 0.0, 0, 'f', 0);
    }
    if (room.limit == QStringLiteral("group-risk")) {
        // Named after the bucket, because "risk budget full" here would be
        // misleading: the portfolio still has room, this ONE market view does not.
        const QString group = correlationGroup(symbol);
        return QStringLiteral("%1 risk full (%2% of equity on that one view)")
            .arg(group)
            .arg((book.equity > 0.0)
                     ? ((book.riskByGroup.value(group, 0.0) / book.equity) * 100.0)
                     : 0.0,
                 0, 'f', 0);
    }
    if (room.limit == QStringLiteral("symbol-risk")) {
        return QStringLiteral("%1 alone already risks %2% of equity")
            .arg(symbol)
            .arg((book.equity > 0.0) ? ((book.symbol.risk / book.equity) * 100.0) : 0.0, 0, 'f', 0);
    }
    if (room.limit == QStringLiteral("margin-cap")) {
        return QStringLiteral("margin cap reached (%1 of %2 invested)")
            .arg(book.invested, 0, 'f', 0)
            .arg(book.equity * cfg.maxExposureFraction, 0, 'f', 0);
    }
    if (room.limit == QStringLiteral("invested-cap")) {
        return QStringLiteral("the %1 EUR invested ceiling is reached (%2 committed)")
            .arg(cfg.maxInvestedEur, 0, 'f', 0)
            .arg(book.invested, 0, 'f', 0);
    }
    return QStringLiteral("not enough free cash (%1)").arg(book.cash, 0, 'f', 0);
}

// The countable category of a closed day, for the scan summary.
QString dayGateCode(DayGate gate)
{
    switch (gate) {
    case DayGate::TargetReached:
        return QStringLiteral("day-target");
    case DayGate::LossLimitReached:
        return QStringLiteral("day-loss");
    case DayGate::Weekend:
        return QStringLiteral("weekend");
    case DayGate::Open:
        break;
    }
    return {};
}

} // namespace

QString sessionPhaseWord(SessionPhase phase)
{
    switch (phase) {
    case SessionPhase::OpeningChaos:
        return QStringLiteral("first quarter hour");
    case SessionPhase::OpeningBurst:
        return QStringLiteral("opening burst");
    case SessionPhase::DataWindow:
        return QStringLiteral("macro-data window");
    case SessionPhase::PolicyWindow:
        return QStringLiteral("central-bank window");
    case SessionPhase::PowerHour:
        return QStringLiteral("power hour");
    case SessionPhase::ClosingBurst:
        return QStringLiteral("closing burst");
    case SessionPhase::Normal:
        break;
    }
    return QStringLiteral("normal");
}

namespace {

// The exchange whose clock an instrument keeps, from the catalog's calendar
// regions. A symbol nobody catalogued is treated as European — the app's own
// account currency and the user's day.
QTimeZone exchangeZone(const QString &symbol)
{
    const InstrumentSpec *spec = instrumentSpec(symbol);
    const QString regions = (spec != nullptr) ? spec->calendarRegions : QString();
    if (regions.contains(QStringLiteral("HK")) || regions.contains(QStringLiteral("CN"))) {
        return QTimeZone("Asia/Hong_Kong");
    }
    if (regions.startsWith(QStringLiteral("US")) || regions.contains(QStringLiteral("US"))) {
        return QTimeZone("America/New_York");
    }
    return QTimeZone("Europe/Berlin");
}

// Minutes since midnight, in that exchange's own local time — which is what makes
// summer time somebody else's problem.
qint32 localMinutes(const QDateTime &now, const QTimeZone &zone)
{
    const QTime local = now.toTimeZone(zone).time();
    return static_cast<qint32>((local.hour() * 60) + local.minute());
}

bool within(qint32 minutes, qint32 fromHour, qint32 fromMinute, qint32 lengthMinutes)
{
    const qint32 start = (fromHour * 60) + fromMinute;
    return (minutes >= start) && (minutes < (start + lengthMinutes));
}

// Where the local session's own open puts us: its first quarter hour, the readable
// rest of that hour, or neither. Xetra opens at 09:00, New York and Hong Kong at
// 09:30 — the same hour, half an hour apart.
SessionPhase openPhase(const QDateTime &now, const QTimeZone &zone)
{
    const bool halfPast = (zone.id() == QByteArrayLiteral("America/New_York"))
                          || (zone.id() == QByteArrayLiteral("Asia/Hong_Kong"));
    const qint32 local = localMinutes(now, zone);
    const qint32 openMinute = halfPast ? 30 : 0;
    if (within(local, 9, openMinute, 15)) {
        return SessionPhase::OpeningChaos;
    }
    if (within(local, 9, openMinute + 15, 45)) {
        return SessionPhase::OpeningBurst;
    }
    return SessionPhase::Normal;
}

// The scheduled events the whole market waits for, on the clock they are published
// in: the central bank at 14:00 New York with its press conference at 14:30 (20:00
// and 20:30 in Berlin for most of the year), the American macro slots at 08:30 and
// 10:00 New York, and the European releases at 08:00 Berlin.
SessionPhase releasePhase(const QDateTime &now)
{
    const qint32 ny = localMinutes(now, QTimeZone("America/New_York"));
    if (within(ny, 14, 0, 45)) {
        return SessionPhase::PolicyWindow;
    }
    if (within(ny, 8, 30, 20) || within(ny, 10, 0, 20)) {
        return SessionPhase::DataWindow;
    }
    if (within(localMinutes(now, QTimeZone("Europe/Berlin")), 8, 0, 15)) {
        return SessionPhase::DataWindow;
    }
    return SessionPhase::Normal;
}

// The run into a close: the last hour of the American session with its final half
// hour the loudest, and the local close of a non-US exchange (Xetra 17:30, HKEX
// 16:00).
SessionPhase closePhase(const QDateTime &now, const QTimeZone &zone)
{
    const qint32 ny = localMinutes(now, QTimeZone("America/New_York"));
    if (within(ny, 15, 30, 30)) {
        return SessionPhase::ClosingBurst;
    }
    if (within(ny, 15, 0, 30)) {
        return SessionPhase::PowerHour;
    }
    const bool usClock = (zone.id() == QByteArrayLiteral("America/New_York"));
    const bool hkClock = (zone.id() == QByteArrayLiteral("Asia/Hong_Kong"));
    if (!usClock
        && within(localMinutes(now, zone), hkClock ? 15 : 17, hkClock ? 30 : 0, 30)) {
        return SessionPhase::ClosingBurst;
    }
    return SessionPhase::Normal;
}

} // namespace

SessionPhase sessionPhaseFor(const QString &symbol, const QDateTime &now)
{
    if (!now.isValid()) {
        return SessionPhase::Normal;
    }
    const QTimeZone zone = exchangeZone(symbol);
    if (now.toTimeZone(zone).date().dayOfWeek() > kFriday) {
        return SessionPhase::Normal;   // no session to be at the edge of
    }
    // Every window is expressed in the clock it belongs to — the local exchange for
    // the session edges, New York for the American releases and the Fed — so the few
    // weeks when Europe and the US change their clocks on different days need no
    // maintenance at all. That divergence is precisely what a fixed offset gets wrong.
    if (const SessionPhase open = openPhase(now, zone); open != SessionPhase::Normal) {
        // …except that a SCHEDULED release inside the opening hour is the more
        // specific fact about the moment, so it is asked about first.
        const SessionPhase release = releasePhase(now);
        if ((open == SessionPhase::OpeningChaos) || (release == SessionPhase::Normal)) {
            return open;
        }
        return release;
    }
    if (const SessionPhase release = releasePhase(now); release != SessionPhase::Normal) {
        return release;
    }
    return closePhase(now, zone);
}

QString paperHoldEvidence(const QList<OpenPositionBrief> &open)
{
    if (open.isEmpty()) {
        return {};
    }
    QString out = QStringLiteral("\nYou currently HOLD these positions (simulated):\n");
    for (const OpenPositionBrief &p : open) {
        out += QStringLiteral("- %1: %2, open %3 h, result so far %4%5 EUR.\n")
                   .arg(p.symbol,
                        p.isBuy ? QStringLiteral("LONG") : QStringLiteral("SHORT"))
                   .arg(p.heldHours, 0, 'f', 1)
                   .arg((p.netPnl > 0.0) ? QStringLiteral("+") : QString())
                   .arg(p.netPnl, 0, 'f', 2);
    }
    // The contract is stated as an ADDITION to the picks list, because that is the
    // only shape small models answer reliably (REQ-F-030), and it is stated
    // positively: say something only about the ones you want out of.
    out += QStringLiteral(
        "For a held position you would EXIT now, add a pick for that symbol with action "
        "CLOSE (or the opposite side). Say nothing about the ones you would keep.\n");
    return out;
}

namespace {

// Why a change of mind may not be acted on yet, or empty when it may. Split out so
// paperAiHold reads as the decision it is rather than as a list of brakes.
QString exitBrake(double confidence, double heldMinutes, const BotConfig &cfg)
{
    if (heldMinutes < static_cast<double>(cfg.minHoldMinutes)) {
        return QStringLiteral(" — held only %1 of %2 min, so not yet acted on")
            .arg(heldMinutes, 0, 'f', 0)
            .arg(cfg.minHoldMinutes);
    }
    if (confidence < cfg.aiExitMinConfidence) {
        return QStringLiteral(" — but only with %1%% conviction, under the %2%% an exit has to "
                              "pay for")
            .arg(confidence, 0, 'f', 0)
            .arg(cfg.aiExitMinConfidence, 0, 'f', 0);
    }
    return {};
}

// The model's own words appended to what it decided, when it gave any.
QString holdReason(const AiProposal &p, const QString &head)
{
    return p.rationale.isEmpty() ? head : (head + QStringLiteral(": ") + p.rationale);
}

// A contrary answer, and whether it may be acted on. The opinion is reported either
// way — the window shows what the model thinks even while the trade is too young to
// act on it.
HoldVerdict contraryVerdict(const AiProposal &p, double heldMinutes, const BotConfig &cfg)
{
    HoldVerdict out;
    out.opinion = HoldOpinion::Close;
    out.code = p.exitNow ? QStringLiteral("ai-close") : QStringLiteral("ai-reversed");
    out.why = holdReason(p, p.exitNow
                                ? QStringLiteral("the model asks to close it")
                                : QStringLiteral("the model now wants the other side"));
    const QString brake = exitBrake(p.confidence, heldMinutes, cfg);
    if (!brake.isEmpty()) {
        out.code = QStringLiteral("ai-too-soon");
        out.why += brake;
        return out;
    }
    out.close = true;
    return out;
}

} // namespace

QString holdOpinionWord(HoldOpinion opinion)
{
    switch (opinion) {
    case HoldOpinion::Hold:
        return QStringLiteral("hold");
    case HoldOpinion::Close:
        return QStringLiteral("close");
    case HoldOpinion::NoOpinion:
        break;
    }
    return QStringLiteral("—");
}

HoldVerdict paperAiHold(const PaperTrade &trade, const QList<AiProposal> &proposals,
                        BotAiMode mode, const QDateTime &now, const BotConfig &cfg)
{
    HoldVerdict out;
    if (mode == BotAiMode::Off) {
        return out;   // nobody was asked; the composite's own rules govern
    }
    // How long it has been open, for the brakes below. An unknown age counts as old
    // enough — the alternative would be a position nothing can ever close.
    const double heldMinutes = (trade.openTime.isValid() && now.isValid())
                                   ? (static_cast<double>(trade.openTime.secsTo(now)) / 60.0)
                                   : static_cast<double>(cfg.minHoldMinutes);
    const qint32 side = trade.isBuy ? 1 : -1;
    for (const AiProposal &p : proposals) {
        if (!p.ok || (p.resolvedSymbol != trade.symbol)) {
            continue;
        }
        const bool contrary = p.exitNow || ((p.dir != 0) && (p.dir != side));
        if (contrary) {
            return contraryVerdict(p, heldMinutes, cfg);
        }
        // Named, and on this position's side (or an explicit HOLD): a KEEP with the
        // model behind it, which is a different thing from not having been asked.
        out.opinion = HoldOpinion::Hold;
        out.code = QStringLiteral("ai-keep");
        out.why = holdReason(p, (p.dir == 0)
                                    ? QStringLiteral("the model says hold")
                                    : QStringLiteral("the model still wants this side"));
    }
    // Absence of an answer keeps the position AND stays NoOpinion — see the header.
    return out;
}

qint64 paperHarvestPick(const QList<HarvestOption> &options, const BotDay &day,
                        const BotConfig &cfg)
{
    if (!cfg.harvestForDailyTarget || (cfg.dailyProfitTarget <= 0.0)) {
        return 0;
    }
    const double missing = cfg.dailyProfitTarget - day.realized;
    if (missing <= 0.0) {
        return 0;   // already made: the day gate is what stops the bot, not this
    }
    qint64 pick = 0;
    double best = 0.0;
    for (const HarvestOption &o : options) {
        if (o.netIfClosedNow < missing) {
            continue;
        }
        // Smallest sufficient: give up the least upside to bank the day.
        if ((pick == 0) || (o.netIfClosedNow < best)) {
            pick = o.id;
            best = o.netIfClosedNow;
        }
    }
    return pick;
}

DayGate paperDayGate(const BotDay &day, const QDateTime &now, const BotConfig &cfg)
{
    if (cfg.tradeWeekdaysOnly && now.isValid() && (now.date().dayOfWeek() > kFriday)) {
        return DayGate::Weekend;
    }
    // Only money actually BOOKED counts. Open profit is not a made day — it can
    // still turn, and a rule that stops on unrealised gains would stop on nothing.
    if (!now.isValid() || (day.date != now.date())) {
        return DayGate::Open;  // a fresh day starts open, whatever yesterday did
    }
    if ((cfg.dailyProfitTarget > 0.0) && (day.realized >= cfg.dailyProfitTarget)) {
        return DayGate::TargetReached;
    }
    if ((cfg.dailyLossLimit > 0.0) && (day.realized <= -cfg.dailyLossLimit)) {
        return DayGate::LossLimitReached;
    }
    return DayGate::Open;
}

QString matchProposalSymbol(const QString &proposalSymbol, const QStringList &known)
{
    const QString wanted = proposalSymbol.trimmed();
    if (wanted.isEmpty()) {
        return {};
    }
    const auto exact = std::find_if(known.cbegin(), known.cend(), [&wanted](const QString &symbol) {
        return symbol.compare(wanted, Qt::CaseInsensitive) == 0;
    });
    if (exact != known.cend()) {
        return *exact;  // the model spelled it exactly
    }
    // Chatty answers ("SPX500 composite", "buy GER40 now") still resolve — but only
    // while exactly ONE instrument is named. Two candidates in one answer is an
    // ambiguous instruction, and an ambiguous instruction must not open a trade.
    QString single;
    for (const QString &symbol : known) {
        if (!wanted.contains(symbol, Qt::CaseInsensitive)) {
            continue;
        }
        if (!single.isEmpty()) {
            return {};  // ambiguous
        }
        single = symbol;
    }
    return single;
}

namespace {

// Which of the model's picks is about `symbol` — or, when none is, WHY not. Split
// out of paperAiGate so both stay inside the complexity budget and the diagnosis
// lives next to the search that produced it.
struct PickMatch {
    qsizetype index = -1;
    QString why;
    QString code;
};

PickMatch matchPick(const QString &symbol, const QList<AiProposal> &proposals)
{
    PickMatch match;
    for (qsizetype i = 0; i < proposals.size(); ++i) {
        const AiProposal &candidate = proposals.at(i);
        if (candidate.ok && !candidate.resolvedSymbol.isEmpty()
            && (candidate.resolvedSymbol.compare(symbol, Qt::CaseInsensitive) == 0)) {
            match.index = i;
            return match;
        }
    }
    // Distinguish "the model chose other instruments" from "the model named things
    // that do not exist here" — the second is a hallucination and worth its own
    // count in the scan summary.
    QStringList chosen;
    for (const AiProposal &p : proposals) {
        if (p.ok && !p.resolvedSymbol.isEmpty() && (chosen.size() < 4)) {
            chosen << p.resolvedSymbol;
        }
    }
    if (chosen.isEmpty()) {
        match.why = QStringLiteral("the AI named nothing tradable here (%1)")
                        .arg(proposals.isEmpty() ? QStringLiteral("no picks")
                                                 : proposals.constFirst().symbol);
        match.code = QStringLiteral("ai-unknown-symbol");
        return match;
    }
    match.why = QStringLiteral("AI picked %1").arg(chosen.join(u", "));
    match.code = QStringLiteral("ai-other-pick");
    return match;
}

} // namespace

// Why there is nothing to act on, in the reader's terms. Each answer names a DIFFERENT
// thing to go and do about it, which is the entire reason this is not one sentence.
QString noAnswerReason(const AiSource &source, QString *code)
{
    if (!source.configured) {
        *code = QStringLiteral("ai-not-configured");
        return QStringLiteral("no local model is configured — set ollamaModel in config.json "
                              "or OLLAMA_MODEL, or switch TRADINGAPP_BOT_AI to off/confirm so "
                              "the composite decides");
    }
    if (!source.asked || (source.ageMs < 0)) {
        *code = QStringLiteral("ai-no-answer");
        return QStringLiteral("the local model has not answered yet this session — it is "
                              "configured, so either the first scan is still in flight or the "
                              "service is not reachable");
    }
    if ((source.maxAgeMs > 0) && (source.ageMs > source.maxAgeMs)) {
        *code = QStringLiteral("ai-stale");
        return QStringLiteral("the model's last answer is %1 s old, past the %2 s bound — a "
                              "CPU model that cannot keep up with the scan cycle leaves every "
                              "instrument un-evaluated")
            .arg(source.ageMs / 1000)
            .arg(source.maxAgeMs / 1000);
    }
    if (source.received > 0) {
        *code = QStringLiteral("ai-unparsed");
        return QStringLiteral("the model answered with %1 pick(s) and none of them parsed — "
                              "see the SIM AI lines for the raw reply")
            .arg(source.received);
    }
    *code = QStringLiteral("ai-no-answer");
    return QStringLiteral("the model answered with no picks at all");
}

AiGate paperAiGate(const QString &symbol, qint32 compositeDir,
                   const QList<AiProposal> &proposals, BotAiMode mode, const AiSource &source)
{
    AiGate gate;
    if (mode == BotAiMode::Off) {
        gate.allow = (compositeDir != 0);
        gate.dir = compositeDir;
        if (!gate.allow) {
            gate.why = QStringLiteral("composite is neutral");
            gate.code = QStringLiteral("composite-neutral");
        }
        return gate;  // the composite decides; a proposal is logged elsewhere
    }
    const bool anyUsable = std::any_of(proposals.cbegin(), proposals.cend(),
                                       [](const AiProposal &p) { return p.ok; });
    if (!anyUsable) {
        // NOT one catch-all sentence. The four causes below call for four different actions,
        // and a log line exists so a person can reproduce the decision.
        gate.why = noAnswerReason(source, &gate.code);
        return gate;
    }
    const PickMatch match = matchPick(symbol, proposals);
    if (match.index < 0) {
        gate.why = match.why;
        gate.code = match.code;
        return gate;
    }
    gate.pick = match.index;
    const AiProposal &proposal = proposals.at(gate.pick);
    if (proposal.dir == 0) {
        gate.why = QStringLiteral("AI says HOLD");
        gate.code = QStringLiteral("ai-hold");
        return gate;
    }
    if (mode == BotAiMode::Lead) {
        gate.allow = true;
        gate.dir = proposal.dir;  // the model supplies the direction
        return gate;
    }
    // Confirm: the model is a veto over the composite, not a source of direction.
    if (compositeDir == 0) {
        gate.why = QStringLiteral("composite is neutral");
        gate.code = QStringLiteral("composite-neutral");
        return gate;
    }
    if (compositeDir != proposal.dir) {
        gate.why = QStringLiteral("AI and composite disagree");
        gate.code = QStringLiteral("ai-disagree");
        return gate;
    }
    gate.allow = true;
    gate.dir = compositeDir;
    return gate;
}

qint32 paperLeverageWithAi(qint32 sized, qint32 asked, BotAiMode mode)
{
    if ((mode != BotAiMode::Lead) || (asked <= 0)) {
        return sized;
    }
    return std::max(1, std::min(sized, asked));
}

double PaperTrade::notional() const
{
    return stake * static_cast<double>(leverage);
}

double PaperTrade::units() const
{
    return paperUnits(stake, leverage, openRate);
}

double PaperTrade::effectiveRate() const
{
    return (markRate > 0.0) ? markRate : openRate;
}

double PaperTrade::grossPnl() const
{
    return paperGrossPnl(stake, leverage, openRate, effectiveRate(), isBuy);
}

double PaperTrade::costsSoFar() const
{
    return openCost + feesPaid;
}

double PaperTrade::riskAtStop() const
{
    if ((slRate <= 0.0) || (openRate <= 0.0)) {
        return 0.0;  // no stop = unbounded, but the bot always sets one
    }
    return notional() * (std::abs(openRate - slRate) / openRate);
}

double PaperTrade::netPnl() const
{
    return grossPnl() - costsSoFar();
}

double PaperClosedTrade::totalCost() const
{
    return openCost + closeCost + feesPaid;
}

double PaperClosedTrade::heldHours() const
{
    if (!openTime.isValid() || !closeTime.isValid()) {
        return 0.0;
    }
    return static_cast<double>(openTime.secsTo(closeTime)) / 3600.0;
}

// ---------------------------------------------------------------------------
// Cost and P/L model
// ---------------------------------------------------------------------------

double paperUnits(double stake, qint32 leverage, double rate)
{
    if (rate <= 0.0) {
        return 0.0;
    }
    return (stake * static_cast<double>(std::max(1, leverage))) / rate;
}

double paperHalfSpreadCost(double stake, qint32 leverage, double spreadPct)
{
    if (spreadPct <= 0.0) {
        return 0.0;
    }
    const double notional = stake * static_cast<double>(std::max(1, leverage));
    // Half the spread, because a position crosses half of it on each side — the
    // identity the trade panel's opening-cost estimate uses (REQ-F-025).
    return (notional * (spreadPct / 100.0)) / 2.0;
}

double paperGrossPnl(double stake, qint32 leverage, double openRate, double markRate, bool isBuy)
{
    if ((openRate <= 0.0) || (markRate <= 0.0)) {
        return 0.0;
    }
    const double notional = stake * static_cast<double>(std::max(1, leverage));
    const double relative = (markRate - openRate) / openRate;
    return isBuy ? (notional * relative) : (-notional * relative);
}

qint32 paperRolloverNights(const QDateTime &from, const QDateTime &to)
{
    if (!from.isValid() || !to.isValid() || (to <= from)) {
        return 0;
    }
    // One night per date boundary crossed. The night that ends on date d began
    // the previous evening, so a Friday evening's rollover is the weekend one.
    qint32 nights = 0;
    for (QDate day = from.date().addDays(1); day <= to.date(); day = day.addDays(1)) {
        nights += (day.addDays(-1).dayOfWeek() == kFriday) ? 3 : 1;
    }
    return nights;
}

double paperRolloverCost(const PaperTrade &trade, const InstrumentFees &fees, qint32 nights,
                          double eurPerUsd)
{
    if ((nights <= 0) || !fees.isValid()) {
        return 0.0;
    }
    // The fee table is per unit per night in the account currency (USD), and a
    // NEGATIVE entry is a credit paid to the holder — which stays negative here,
    // so a carry-positive position correctly earns money in the simulation too.
    const double perUnit = trade.isBuy ? fees.buyOvernight : fees.sellOvernight;
    const double raw = perUnit * trade.units() * static_cast<double>(nights);
    return (eurPerUsd > 0.0) ? (raw * eurPerUsd) : raw;
}

// ---------------------------------------------------------------------------
// Entry evaluation
// ---------------------------------------------------------------------------

EntrySignal buildEntrySignal(const CandidateInput &in, const BotConfig &cfg)
{
    EntrySignal sig;
    sig.symbol = in.symbol;
    sig.instrumentId = in.instrumentId;
    sig.confidence = in.confidence;
    sig.spreadPct = in.spreadPct;
    if (in.dir == 0) {
        return sig;  // no call to act on
    }
    sig.isBuy = (in.dir > 0);
    // Priced at the mid, with the spread charged as an explicit cost on each side
    // (see the header): economically the ask fill for a buy and the bid fill for a
    // sell, but with the transaction cost visible instead of buried in the rate.
    if ((in.bid <= 0.0) || (in.ask <= 0.0)) {
        return sig;  // no two-sided quote: not priced, so not tradable
    }
    sig.fillRate = (in.bid + in.ask) / 2.0;
    if (in.closes.size() < kMinCloses) {
        return sig;  // too little history to derive a stop distance from
    }

    sig.volPct = volatilityPct(in.closes, std::min(kVolBars, in.closes.size() - 1));
    const double slFrac = proposedSlFraction(sig.volPct, cfg.horizonHours);
    // Every ceiling is applied BEFORE the ladder fold, never after: the broker offers
    // discrete steps, so clamping a folded value to a cap can land on a leverage that
    // does not exist (Gold.24-7 offers 1/2/5/20 — an x8 "cap" produced x8, which no
    // instrument sells). recommendLeverage folds to the largest OFFERED step within
    // the cap, so the answer is always a step the instrument really has.
    const qint32 groupCap = groupLeverageCap(correlationGroup(in.symbol));
    qint32 cap = cfg.leverageCap;
    if (groupCap > 0) {
        cap = std::min(cap, groupCap);
    }
    if (in.maxLeverage > 0) {
        cap = std::min(cap, in.maxLeverage);
    }
    // The combined indication's own bound (REQ-F-036), applied like every other cap:
    // BEFORE the ladder fold, so the result is a step the instrument really sells, and
    // only ever downwards — evidence may lower leverage, never raise it.
    if (in.leadMaxLeverage > 0) {
        cap = std::min(cap, in.leadMaxLeverage);
    }
    // The instrument's own ladder: what the caller supplied, else the catalog's steps
    // for that symbol (the bot scans every instrument, and only the one on screen
    // publishes its live ladder), else recommendLeverage's own default.
    QList<qint32> steps = in.leverageSteps;
    if (steps.isEmpty()) {
        const InstrumentSpec *spec = instrumentSpec(in.symbol);
        if (spec != nullptr) {
            steps = spec->simLeverage;
        }
    }
    sig.leverage = std::max(1, recommendLeverage(slFrac, cfg.riskBudgetFraction, cap, steps));

    const double tpFrac = slFrac * kRewardRisk;
    sig.slRate = sig.isBuy ? (sig.fillRate * (1.0 - slFrac)) : (sig.fillRate * (1.0 + slFrac));
    sig.tpRate = sig.isBuy ? (sig.fillRate * (1.0 + tpFrac)) : (sig.fillRate * (1.0 - tpFrac));
    // Deliberately does NOT name the source: in AI-lead mode the direction and the
    // confidence come from the model, not from the composite, and a line claiming
    // "composite BUY (conf 98)" for the model's call would be a lie in the books.
    // The runner appends the source when an AI drove the decision.
    sig.basis = QStringLiteral("%1 (conf %2), sigma %3%/h, lev x%4, stop %5%")
                    .arg(sig.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                    .arg(sig.confidence, 0, 'f', 0)
                    .arg(sig.volPct, 0, 'f', 2)
                    .arg(sig.leverage)
                    .arg(slFrac * 100.0, 0, 'f', 2);
    sig.valid = true;
    return sig;
}

double paperEntrySignalRisk(const EntrySignal &signal)
{
    if ((signal.fillRate <= 0.0) || (signal.slRate <= 0.0)) {
        return 0.0;
    }
    const double stopFrac = std::abs(signal.fillRate - signal.slRate) / signal.fillRate;
    return stopFrac * static_cast<double>(std::max(1, signal.leverage));
}

double paperStakeFor(const BookState &book, const BotConfig &cfg, double riskPerStake)
{
    return paperStakeRoom(book, cfg, riskPerStake).stake;
}

QString correlationGroup(const QString &symbol)
{
    // The catalog's selector group is the base: Indices / Forex / Commodities.
    const InstrumentSpec *spec = instrumentSpec(symbol);
    if (spec == nullptr) {
        return QStringLiteral("other:") + symbol;   // its own bucket, never shared
    }
    // The dollar index is listed under Indices because that is where a trader looks
    // for it, but it is an FX position and it moves AGAINST the equity indices.
    if (symbol == QStringLiteral("USDOLLAR")) {
        return QStringLiteral("fx");
    }
    if (spec->group == QStringLiteral("Indices")) {
        return QStringLiteral("equity-index");
    }
    if (spec->group == QStringLiteral("Forex")) {
        return QStringLiteral("fx");
    }
    if (spec->group == QStringLiteral("Commodities")) {
        // Precious metals are one trade in practice (the same real-rate story);
        // energy and softs are not part of it.
        const bool metal = symbol.contains(QStringLiteral("GOLD"), Qt::CaseInsensitive)
                           || symbol.contains(QStringLiteral("SILVER"), Qt::CaseInsensitive)
                           || symbol.contains(QStringLiteral("PLATINUM"), Qt::CaseInsensitive)
                           || symbol.contains(QStringLiteral("PALLAD"), Qt::CaseInsensitive);
        return metal ? QStringLiteral("metals") : QStringLiteral("commodity");
    }
    return QStringLiteral("group:") + spec->group;
}

qint32 groupLeverageCap(const QString &group)
{
    if (group == QStringLiteral("fx")) {
        return 5;    // a daily range of a few tenths of a percent
    }
    if (group == QStringLiteral("equity-index")) {
        return 10;
    }
    if (group == QStringLiteral("metals")) {
        return 8;
    }
    // "commodity" (oil and softs, which gap on inventory numbers) and an
    // unclassified symbol both land on the careful ceiling, so they share the
    // branch rather than repeating the same number twice.
    return 5;
}

namespace {

// The six ceilings that can bind a stake, as one value. A struct rather than six
// parameters because six would breach the parameter limit the metrics ratchet enforces,
// and because they are one concept: the set of things that could stop this trade.
struct RoomCandidates {
    double byRisk = 0.0;
    double byGroup = 0.0;
    double bySymbol = 0.0;
    double margin = 0.0;
    double invested = 0.0;
    double cash = 0.0;
};

struct BindingLimit {
    double room = 0.0;
    QString label;
};

// Which ceiling actually binds, and the name a refusal must report for it.
//
// Extracted from paperStakeRoom rather than left inline: the original was a chain of one
// `if` per limit, which cost a branch each time a limit was added, and adding the absolute
// invested ceiling as the fifth pushed that function to CCN 17 against the ratchet's 15.
// This is a fold over candidates, not control flow worth reading in place.
//
// STRICT `<`, and the order below is the reported precedence: when two ceilings are equally
// tight the EARLIER one is named. That is deliberate — "the risk budget is full" is a more
// useful thing to be told than "there is no cash", and a tie used to resolve that way in
// the if-chain this replaces.
BindingLimit bindingLimit(const RoomCandidates &c)
{
    BindingLimit out{c.byRisk, QStringLiteral("risk-budget")};
    const auto consider = [&out](double room, const QString &label) {
        if (room < out.room) {
            out.room = room;
            out.label = label;
        }
    };
    consider(c.byGroup, QStringLiteral("group-risk"));
    consider(c.bySymbol, QStringLiteral("symbol-risk"));
    consider(c.margin, QStringLiteral("margin-cap"));
    // Named apart from margin-cap on purpose: "the book is at its 15 000 EUR ceiling" and
    // "there is no free margin left at this equity" are different facts, and a log that
    // collapsed them would send someone to change the wrong number.
    consider(c.invested, QStringLiteral("invested-cap"));
    consider(c.cash, QStringLiteral("cash"));
    return out;
}

} // namespace

StakeRoom paperStakeRoom(const BookState &book, const BotConfig &cfg, double riskPerStake,
                         const QString &symbol)
{
    // A fraction of CURRENT equity, so the bot compounds its wins and shrinks its
    // size after losses without any extra rule.
    const double wanted = std::max(cfg.minStake, book.equity * cfg.stakeFraction);
    const double marginRoom = (book.equity * cfg.maxExposureFraction) - book.invested;
    // The ABSOLUTE euro ceiling on what may be committed, separate from the fractional one
    // because a fraction of current equity is not a fixed amount of money (see
    // BotConfig::maxInvestedEur). Off when 0, in which case it must not bind — hence the
    // fall back to marginRoom rather than to 0.
    const double investedRoom = (cfg.maxInvestedEur > 0.0)
                                    ? (cfg.maxInvestedEur - book.invested)
                                    : marginRoom;
    // …and never more than the account actually holds, with a slice left for the
    // opening spread (which is charged from cash on top of the stake). Without this
    // the margin cap alone let cash go negative.
    const double cashRoom = book.cash * kCashHeadroom;
    // …and the room the RISK budget leaves, which is the binding constraint by
    // design: the count of trades is free, the total loss-if-all-stops-hit is not.
    const double perStake = (riskPerStake > 0.0) ? riskPerStake : cfg.riskBudgetFraction;
    const double riskRoom = (book.equity * cfg.maxPortfolioRiskFraction) - book.openRisk;
    const double stakeByRisk = (riskRoom > 0.0) ? (riskRoom / perStake) : 0.0;
    // …and the room the candidate's own CORRELATION BUCKET leaves. Skipped when the
    // caller asks without a symbol (the portfolio-level question) or when the cap is
    // switched off.
    double stakeByGroup = stakeByRisk;
    if (!symbol.isEmpty() && (cfg.maxGroupRiskFraction > 0.0)) {
        const double groupRoom = (book.equity * cfg.maxGroupRiskFraction)
                                 - book.riskByGroup.value(correlationGroup(symbol), 0.0);
        stakeByGroup = (groupRoom > 0.0) ? (groupRoom / perStake) : 0.0;
    }
    // …and the tightest of the three: what this ONE instrument may still risk.
    // Several positions in one symbol are the same bet sized up (REQ-F-032).
    double stakeBySymbol = stakeByRisk;
    if (!symbol.isEmpty() && (cfg.maxSymbolRiskFraction > 0.0)) {
        const double symbolRoom = (book.equity * cfg.maxSymbolRiskFraction) - book.symbol.risk;
        stakeBySymbol = (symbolRoom > 0.0) ? (symbolRoom / perStake) : 0.0;
    }
    // Name the binding one, so a refusal can say which limit it was.
    const BindingLimit binding = bindingLimit(RoomCandidates{stakeByRisk, stakeByGroup,
                                                             stakeBySymbol, marginRoom,
                                                             investedRoom, cashRoom});
    StakeRoom out;
    // const since the fold above replaced the if-chain that used to reassign this.
    const double room = binding.room;
    out.limit = binding.label;
    if (room < cfg.minStake) {
        return out;  // stake stays 0: no room left for a trade worth its costs
    }
    out.stake = std::min(wanted, room);
    if (out.stake >= wanted) {
        out.limit.clear();  // the target stake fit; no limit was reached
    }
    return out;
}

EntryVerdict paperEntryVerdict(const CandidateInput &in, const EntrySignal &sig,
                                const BookState &book, const BotConfig &cfg)
{
    EntryVerdict verdict;
    // The DAY rule comes first: once the day's target or loss limit is booked, the
    // bot is done for the day — no instrument, no signal and no model changes that.
    // This is the emotionless part: it does not get to argue with its own limit.
    if (const DayGate day = paperDayGate(book.day, in.now, cfg); day != DayGate::Open) {
        verdict.why = dayGateWord(day);
        verdict.code = dayGateCode(day);
        return verdict;
    }
    // Then a chain of single-condition gates: each refusal names exactly one
    // reason, which is what the log line has to state (and it keeps every decision
    // inside the MC/DC condition limit).
    if (const EntryVerdict tradable = tradabilityVerdict(in); !tradable.code.isEmpty()) {
        return tradable;
    }
    const SessionPhase phase = sessionPhaseFor(in.symbol, in.now);
    const double windowFactor = volatileWindowFactor(phase, cfg);
    if (const EntryVerdict paced = paceVerdict(in, cfg, phase, windowFactor);
        !paced.code.isEmpty()) {
        return paced;
    }
    if (const EntryVerdict stacking = stackingVerdict(in, book, cfg); !stacking.code.isEmpty()) {
        return stacking;
    }
    if (book.openCount >= cfg.maxOpenTrades) {
        verdict.why = QStringLiteral("at the %1-trade limit").arg(cfg.maxOpenTrades);
        verdict.code = QStringLiteral("trade-limit");
        return verdict;
    }
    if (book.equity <= (cfg.startEquity * cfg.minEquityFraction)) {
        verdict.why = QStringLiteral("equity below the ruin guard — no new trades");
        verdict.code = QStringLiteral("ruin-guard");
        return verdict;
    }
    if (!sig.valid) {
        verdict.why = QStringLiteral("not enough price history to size a trade");
        verdict.code = QStringLiteral("no-history");
        return verdict;
    }
    if (sig.spreadPct <= 0.0) {
        // Charging nothing would make the result look better than it is, so an
        // instrument whose live spread is unknown is simply not traded.
        verdict.why = QStringLiteral("live spread unknown — cost model incomplete");
        verdict.code = QStringLiteral("spread-unknown");
        return verdict;
    }
    const StakeRoom room = paperStakeRoom(book, cfg, paperEntrySignalRisk(sig), in.symbol);
    if (room.stake <= 0.0) {
        // Named for what it actually is — portfolio risk, ONE market view's risk,
        // margin or cash. This is the limit that ends a scan, never a trade count.
        verdict.code = room.limit;
        verdict.why = roomRefusalWhy(room, book, cfg, in.symbol);
        return verdict;
    }
    // Size down in a loud window: the same stop is a wider bet when the range of
    // the next ten minutes is three times the usual.
    if (const EntryVerdict reluctant = reluctanceVerdict(in, sig, cfg);
        !reluctant.code.isEmpty()) {
        return reluctant;
    }
    const double stake = room.stake / windowFactor;
    if (stake < cfg.minStake) {
        verdict.why = QStringLiteral("the %1 leaves too little size to be worth its costs")
                          .arg(sessionPhaseWord(phase));
        verdict.code = QStringLiteral("volatile-window");
        return verdict;
    }
    // Worth taking AFTER costs? The move to the target has to be a multiple of the
    // round trip; otherwise the trade is a coin flip whose winning side pays the
    // spread and the rollover (REQ-F-032).
    if (cfg.minEdgeOverCost > 0.0) {
        const EntryEconomics econ = paperEntryEconomics(sig, stake, in, cfg);
        if ((econ.cost > 0.0) && (econ.ratio < cfg.minEdgeOverCost)) {
            verdict.why = QStringLiteral("target worth %1 EUR against %2 EUR of costs "
                                         "(%3x, below the %4x floor)")
                              .arg(econ.gainAtTarget, 0, 'f', 2)
                              .arg(econ.cost, 0, 'f', 2)
                              .arg(econ.ratio, 0, 'f', 1)
                              .arg(cfg.minEdgeOverCost, 0, 'f', 1);
            verdict.code = QStringLiteral("cost-vs-edge");
            return verdict;
        }
    }
    verdict.take = true;
    verdict.stake = stake;
    verdict.why = sig.basis;
    return verdict;
}

// ---------------------------------------------------------------------------
// Exit evaluation
// ---------------------------------------------------------------------------

namespace {

// Stop/target test for one side, at the rate the position closes at. The stop is
// checked FIRST by the caller: when a single mark jumps past both legs (a gap, or
// a slow poll), assuming the loss is the honest reading of an unknown path.
CloseReason barrierHit(const PaperTrade &trade, double closeRate)
{
    if (trade.isBuy) {
        if ((trade.slRate > 0.0) && (closeRate <= trade.slRate)) {
            return CloseReason::StopLoss;
        }
        if ((trade.tpRate > 0.0) && (closeRate >= trade.tpRate)) {
            return CloseReason::TakeProfit;
        }
        return CloseReason::None;
    }
    if ((trade.slRate > 0.0) && (closeRate >= trade.slRate)) {
        return CloseReason::StopLoss;
    }
    if ((trade.tpRate > 0.0) && (closeRate <= trade.tpRate)) {
        return CloseReason::TakeProfit;
    }
    return CloseReason::None;
}

} // namespace

double paperRemainingUpside(const PaperTrade &trade, double markRate)
{
    if ((trade.tpRate <= 0.0) || (markRate <= 0.0) || (trade.openRate <= 0.0)) {
        return 0.0;
    }
    // Distance still to travel, on the side the trade is on. Already past the
    // target = nothing left to win here; the take-profit rule closes it anyway.
    const double distance = trade.isBuy ? (trade.tpRate - markRate) : (markRate - trade.tpRate);
    if (distance <= 0.0) {
        return 0.0;
    }
    return trade.notional() * (distance / trade.openRate);
}

double paperCostToHold(const PaperTrade &trade, const ExitContext &ctx, const QDateTime &until)
{
    const double exitCost = paperHalfSpreadCost(trade.stake, trade.leverage, ctx.spreadPct);
    if (!ctx.feesKnown) {
        return exitCost;  // no fee table: only the spread is knowable
    }
    const qint32 nights = paperRolloverNights(ctx.now, until);
    return exitCost + paperRolloverCost(trade, ctx.fees, nights, ctx.eurPerUsd);
}

bool paperWeekendChargeAhead(const QDateTime &now)
{
    if (!now.isValid()) {
        return false;
    }
    // The rollover charged at the next date boundary is the weekend one when the
    // day that begins there is a Saturday — the same Friday-night rule
    // paperRolloverNights bills three times.
    return now.date().addDays(1).dayOfWeek() == kSaturday;
}

namespace {

// The two CARRY exits, kept together because they answer one question: is holding
// this position still worth what it costs? Returns None when it is.
CloseReason carryExit(const PaperTrade &trade, const ExitContext &ctx, const BotConfig &cfg)
{
    if (!ctx.now.isValid() || !trade.openTime.isValid()) {
        return CloseReason::None;  // nothing to reason about without a clock
    }
    const double upside = paperRemainingUpside(trade, ctx.markRate);
    if (upside <= 0.0) {
        return CloseReason::None;  // no target left to price the carry against
    }

    // 1. Carry beats the edge: what is left to win, against what the rest of the
    //    intended holding time will cost (rollover nights + the exit spread). A
    //    trade that cannot out-earn its own rent is closed now rather than paid for
    //    night after night.
    const double heldHours = static_cast<double>(trade.openTime.secsTo(ctx.now)) / 3600.0;
    const double hoursLeft = std::max(0.0, static_cast<double>(cfg.maxHoldHours) - heldHours);
    const QDateTime horizon = ctx.now.addSecs(static_cast<qint64>(hoursLeft * 3600.0));
    if (paperCostToHold(trade, ctx, horizon) >= upside) {
        return CloseReason::CostsExceedEdge;
    }

    // 2. The weekend charge has to be EARNED. eToro bills the Friday night three
    //    times; a position whose current net does not cover that charge is closed
    //    before paying it, which also avoids carrying the weekend gap. One already
    //    up by more than the charge may ride through.
    if (ctx.feesKnown && paperWeekendChargeAhead(ctx.now)) {
        const double weekendCharge = paperRolloverCost(trade, ctx.fees, 3, ctx.eurPerUsd);
        if ((weekendCharge > 0.0) && (trade.netPnl() < weekendCharge)) {
            return CloseReason::WeekendCarry;
        }
    }
    return CloseReason::None;
}

} // namespace

QStringList entryFeatureNames()
{
    // Stable, additive-only: a name added here must be added at the END, or every
    // model trained before the change would silently read shifted inputs.
    return {QStringLiteral("confidence"), QStringLiteral("volPct"),
            QStringLiteral("stopPct"),    QStringLiteral("targetPct"),
            QStringLiteral("spreadPct"),  QStringLiteral("edgeOverCost"),
            QStringLiteral("leverage"),   QStringLiteral("dir"),
            QStringLiteral("hourUtc"),    QStringLiteral("dayOfWeek"),
            QStringLiteral("aiBacked")};
}

QList<double> entryFeatureValues(const EntryFeatures &f)
{
    return {f.confidence,
            f.volPct,
            f.stopPct,
            f.targetPct,
            f.spreadPct,
            f.edgeOverCost,
            static_cast<double>(f.leverage),
            static_cast<double>(f.dir),
            static_cast<double>(f.hourUtc),
            static_cast<double>(f.dayOfWeek),
            f.aiBacked ? 1.0 : 0.0};
}

QHash<QString, double> entryFeatureMap(const EntryFeatures &f)
{
    const QStringList names = entryFeatureNames();
    const QList<double> values = entryFeatureValues(f);
    QHash<QString, double> out;
    out.reserve(names.size());
    for (qsizetype i = 0; (i < names.size()) && (i < values.size()); ++i) {
        out.insert(names.at(i), values.at(i));
    }
    return out;
}

EntryEconomics paperEntryEconomics(const EntrySignal &sig, double stake,
                                   const CandidateInput &in, const BotConfig &cfg)
{
    EntryEconomics out;
    if ((stake <= 0.0) || (sig.fillRate <= 0.0) || (sig.tpRate <= 0.0)) {
        return out;
    }
    const double notional = stake * static_cast<double>(sig.leverage);
    out.gainAtTarget = notional * (qAbs(sig.tpRate - sig.fillRate) / sig.fillRate);
    // Both crossings: the half-spread paid on the way in and the one on the way out.
    out.cost = 2.0 * paperHalfSpreadCost(stake, sig.leverage, sig.spreadPct);
    if (in.feesKnown && in.now.isValid()) {
        // …plus the rent for holding it to the horizon, weekend nights included.
        PaperTrade probe;
        probe.stake = stake;
        probe.leverage = sig.leverage;
        probe.openRate = sig.fillRate;
        probe.isBuy = sig.isBuy;
        const qint32 nights = paperRolloverNights(in.now, in.now.addSecs(qint64{3600}
                                                                        * cfg.horizonHours));
        out.cost += paperRolloverCost(probe, in.fees, nights, in.eurPerUsd);
    }
    // A rollover CREDIT can make the round trip cheaper than the spread alone, and
    // in principle free; the ratio then says "no cost stands in the way".
    out.ratio = (out.cost > 0.0) ? (out.gainAtTarget / out.cost) : 0.0;
    return out;
}

namespace {

// The two exits that read the trade's own DYNAMICS rather than a fixed level: the
// signal that justified it has faded while it is not paying, and a winner that has
// handed back most of its best result. Between the stop and the target, these are
// the only rules that can act on what actually happened.
// Has the signal that justified the trade faded, and is acting on it worth the
// spread? Compared against the COMPOSITE's conviction at entry, never the model's:
// in lead mode the two live on different scales, and mixing them made every
// model-led trade look faded the instant it opened.
bool fadedAndWorthClosing(const PaperTrade &trade, const ExitContext &ctx,
                          const BotConfig &cfg, double net)
{
    const qint32 side = trade.isBuy ? 1 : -1;
    const bool stillMine = (ctx.dirNow == side) || (ctx.dirNow == 0);
    if (!stillMine || (trade.entryCompositeConf <= 0.0) || (ctx.confNow < 0.0) || (net > 0.0)) {
        return false;
    }
    if (ctx.confNow >= (trade.entryCompositeConf * cfg.signalFadeFraction)) {
        return false;   // the conviction is intact
    }
    // Closing a position that is down less than the round trip costs pays the spread
    // to save nothing — which is how a rule with a sound premise became the biggest
    // single loss in the measured book (7 fades, −13.87 average).
    const double exitCost = paperHalfSpreadCost(trade.stake, trade.leverage, ctx.spreadPct);
    return (exitCost <= 0.0) || (-net >= (exitCost * std::max(0.0, cfg.fadeMinLossOverCost)));
}

CloseReason dynamicExit(const PaperTrade &trade, const ExitContext &ctx, const BotConfig &cfg)
{
    const double net = trade.netPnl();
    // Every rule here is DISCRETIONARY, so every one of them waits out the minimum
    // holding time. Closing a position minutes after opening it pays two half-spreads
    // for a price that has not had time to do anything.
    if (trade.openTime.isValid() && ctx.now.isValid()) {
        const double heldMinutes = static_cast<double>(trade.openTime.secsTo(ctx.now)) / 60.0;
        if (heldMinutes < static_cast<double>(cfg.minHoldMinutes)) {
            return CloseReason::None;
        }
    }
    if (fadedAndWorthClosing(trade, ctx, cfg, net)) {
        return CloseReason::SignalFade;
    }
    // Given back: it was up by something that mattered and has since surrendered most
    // of it. Banking what is left beats hoping for the target again.
    if ((trade.peakNet >= cfg.giveBackMinNet) && (net > 0.0)
        && (net <= (trade.peakNet * (1.0 - cfg.giveBackFraction)))) {
        return CloseReason::GiveBack;
    }
    return CloseReason::None;
}

} // namespace

CloseReason paperCloseDecision(const PaperTrade &trade, const ExitContext &ctx,
                                const BotConfig &cfg)
{
    if (ctx.markRate > 0.0) {
        const CloseReason hit = barrierHit(trade, ctx.markRate);
        if (hit != CloseReason::None) {
            return hit;
        }
    }
    // Then the economics: a position that costs more to hold than it can still win
    // is closed even while its price barriers are untouched (REQ-F-029).
    const CloseReason carry = carryExit(trade, ctx, cfg);
    if (carry != CloseReason::None) {
        return carry;
    }
    // The composite turning against an open trade with conviction: the bot took
    // the position on that signal, so it gives it up on the opposite one.
    const qint32 side = trade.isBuy ? 1 : -1;
    const bool flipped = (ctx.dirNow != 0) && (ctx.dirNow != side);
    if (flipped && (ctx.confNow >= cfg.flipConfidence)) {
        return CloseReason::SignalFlip;
    }
    const CloseReason dynamic = dynamicExit(trade, ctx, cfg);
    if (dynamic != CloseReason::None) {
        return dynamic;
    }
    if (trade.openTime.isValid() && ctx.now.isValid()) {
        const double heldHours = static_cast<double>(trade.openTime.secsTo(ctx.now)) / 3600.0;
        if (heldHours >= static_cast<double>(cfg.maxHoldHours)) {
            return CloseReason::MaxHold;
        }
    }
    return CloseReason::None;
}

// ---------------------------------------------------------------------------
// The books
// ---------------------------------------------------------------------------

PaperPerformance paperPerformance(const QList<PaperClosedTrade> &closed, double startEquity,
                                  double dailyTarget)
{
    PaperPerformance perf;
    perf.closedTrades = static_cast<qint32>(closed.size());
    QMap<QDate, double> perDay;   // sorted: the equity curve follows the close order
    double running = 0.0;
    double peak = 0.0;
    qint32 wins = 0;
    for (const PaperClosedTrade &c : closed) {
        perf.netTotal += c.netPnl;
        perf.costsPaid += c.totalCost();
        if (c.netPnl >= 0.0) {
            perf.grossWins += c.netPnl;
            ++wins;
        } else {
            perf.grossLosses += -c.netPnl;
        }
        if (c.closeTime.isValid()) {
            perDay[c.closeTime.date()] += c.netPnl;
        }
        const QString reason = closeReasonWord(c.reason);
        perf.netByReason[reason] += c.netPnl;
        perf.countByReason[reason] += 1;
        // Both sides, separately: a strategy that only earns on longs in a rising
        // market has not shown an edge, it has shown a market.
        if (c.isBuy) {
            perf.longNet += c.netPnl;
        } else {
            ++perf.shortTrades;
            perf.shortNet += c.netPnl;
        }
        // Drawdown on the CLOSED-trade curve: the deepest give-back from a peak,
        // which is the number that decides whether an account survives the strategy.
        running += c.netPnl;
        peak = std::max(peak, running);
        perf.maxDrawdown = std::max(perf.maxDrawdown, peak - running);
    }
    perf.tradingDays = static_cast<qint32>(perDay.size());
    if (perf.tradingDays > 0) {
        perf.netPerDay = perf.netTotal / static_cast<double>(perf.tradingDays);
        for (auto it = perDay.cbegin(); it != perDay.cend(); ++it) {
            if ((dailyTarget > 0.0) && (it.value() >= dailyTarget)) {
                ++perf.daysAtTarget;
            }
        }
        perf.targetHitRate =
            (static_cast<double>(perf.daysAtTarget) / static_cast<double>(perf.tradingDays)) * 100.0;
        // The most recent few days, newest first (QMap is date-sorted).
        auto it = perDay.cend();
        while ((it != perDay.cbegin()) && (perf.rollingDays < kRollingDays)) {
            --it;
            perf.netLastDays += it.value();
            ++perf.rollingDays;
        }
    }
    if (perf.closedTrades > 0) {
        perf.expectancy = perf.netTotal / static_cast<double>(perf.closedTrades);
        perf.winRate = (static_cast<double>(wins) / static_cast<double>(perf.closedTrades)) * 100.0;
    }
    if (perf.grossLosses > 0.0) {
        perf.profitFactor = perf.grossWins / perf.grossLosses;
    }
    if (startEquity > 0.0) {
        perf.maxDrawdownPct = (perf.maxDrawdown / startEquity) * 100.0;
    }
    return perf;
}

LiveReadiness paperLiveReadiness(const PaperPerformance &perf, const LiveGateConfig &gate)
{
    LiveReadiness out;
    if (perf.closedTrades < gate.minClosedTrades) {
        out.blockers << QStringLiteral("only %1 of %2 closed trades")
                            .arg(perf.closedTrades)
                            .arg(gate.minClosedTrades);
    }
    if (perf.tradingDays < gate.minTradingDays) {
        out.blockers << QStringLiteral("only %1 of %2 trading days")
                            .arg(perf.tradingDays)
                            .arg(gate.minTradingDays);
    }
    if (perf.netTotal <= gate.minNetTotal) {
        out.blockers << QStringLiteral("net after costs is %1 EUR, not above %2")
                            .arg(perf.netTotal, 0, 'f', 2)
                            .arg(gate.minNetTotal, 0, 'f', 2);
    }
    // A record with no losing trade yet has no measurable profit factor; that is a
    // reason to keep measuring, not a reason to pass.
    if (perf.profitFactor < gate.minProfitFactor) {
        out.blockers << QStringLiteral("profit factor %1 below %2")
                            .arg(perf.profitFactor, 0, 'f', 2)
                            .arg(gate.minProfitFactor, 0, 'f', 2);
    }
    if (perf.maxDrawdownPct > gate.maxDrawdownPct) {
        out.blockers << QStringLiteral("max drawdown %1% over the %2% limit")
                            .arg(perf.maxDrawdownPct, 0, 'f', 1)
                            .arg(gate.maxDrawdownPct, 0, 'f', 1);
    }
    if (perf.expectancy <= gate.minExpectancy) {
        out.blockers << QStringLiteral("expectancy %1 EUR per trade, not above %2")
                            .arg(perf.expectancy, 0, 'f', 2)
                            .arg(gate.minExpectancy, 0, 'f', 2);
    }
    out.ready = out.blockers.isEmpty();
    return out;
}

PaperBook::PaperBook(const BotConfig &cfg)
    : m_cfg(cfg)
    , m_cash(cfg.startEquity)
{
}

void PaperBook::reset()
{
    m_cash = m_cfg.startEquity;
    m_realized = 0.0;
    m_costsPaid = 0.0;
    m_nextId = 1;
    m_day = BotDay{};
    m_open.clear();
    m_closed.clear();
}

void PaperBook::setConfig(const BotConfig &cfg)
{
    m_cfg = cfg;
}

qsizetype PaperBook::indexOf(qint64 id) const
{
    for (qsizetype i = 0; i < m_open.size(); ++i) {
        if (m_open.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

BookState PaperBook::state() const
{
    const PaperStats s = stats();
    BookState st;
    st.equity = s.equity;
    st.cash = s.cash;
    st.invested = s.invested;
    st.openCount = s.openTrades;
    for (const PaperTrade &t : m_open) {
        const double risk = t.riskAtStop();
        st.openRisk += risk;
        st.riskByGroup[correlationGroup(t.symbol)] += risk;
    }
    st.day = m_day;
    // The per-symbol question needs a candidate, so the caller fills st.symbol from
    // exposureFor() — state() answers the portfolio-wide questions only.
    return st;
}

SymbolExposure PaperBook::exposureFor(const QString &symbol) const
{
    SymbolExposure out;
    for (const PaperTrade &t : m_open) {
        if (t.symbol != symbol) {
            continue;
        }
        ++out.count;
        out.dir = t.isBuy ? 1 : -1;
        out.risk += t.riskAtStop();
    }
    return out;
}

PaperStats PaperBook::stats() const
{
    PaperStats s;
    s.startEquity = m_cfg.startEquity;
    s.cash = m_cash;
    s.realized = m_realized;
    s.costsPaid = m_costsPaid;
    s.openTrades = static_cast<qint32>(m_open.size());
    s.closedTrades = static_cast<qint32>(m_closed.size());
    double grossOpen = 0.0;
    for (const PaperTrade &t : m_open) {
        s.invested += t.stake;
        s.openPnl += t.netPnl();
        grossOpen += t.grossPnl();
    }
    for (const PaperClosedTrade &c : m_closed) {
        if (c.netPnl >= 0.0) {
            ++s.wins;
        } else {
            ++s.losses;
        }
        s.bestTrade = std::max(s.bestTrade, c.netPnl);
        s.worstTrade = std::min(s.worstTrade, c.netPnl);
    }
    // Cash-based equity: the stakes return when the positions close, and the
    // rollover already left cash. (The P/L-based form — start + realised + open
    // net — is identical by construction; TS-PAPER-006 asserts they agree.)
    s.equity = m_cash + s.invested + grossOpen;
    s.totalPnl = s.equity - s.startEquity;
    s.totalPnlPct = (s.startEquity > 0.0) ? ((s.totalPnl / s.startEquity) * 100.0) : 0.0;
    if (s.closedTrades > 0) {
        s.winRate = (static_cast<double>(s.wins) / static_cast<double>(s.closedTrades)) * 100.0;
    }
    return s;
}

qint64 PaperBook::open(const EntrySignal &sig, double stake, const QDateTime &now)
{
    if (!sig.valid || (stake <= 0.0) || (sig.fillRate <= 0.0)) {
        return 0;
    }
    PaperTrade t;
    t.id = m_nextId++;
    t.symbol = sig.symbol;
    t.instrumentId = sig.instrumentId;
    t.isBuy = sig.isBuy;
    t.stake = stake;
    t.leverage = sig.leverage;
    t.openRate = sig.fillRate;
    t.slRate = sig.slRate;
    t.tpRate = sig.tpRate;
    t.openCost = paperHalfSpreadCost(stake, sig.leverage, sig.spreadPct);
    t.openTime = now;
    t.feesChargedTo = now;
    t.markRate = sig.fillRate;
    t.markTime = now;
    t.entryConfidence = sig.confidence;
    t.entryBasis = sig.basis;

    // The stake is committed and the entry spread is paid immediately.
    m_cash -= (stake + t.openCost);
    m_costsPaid += t.openCost;
    if (now.isValid()) {
        if (m_day.date != now.date()) {
            m_day = BotDay{};
            m_day.date = now.date();
        }
        ++m_day.opened;
    }
    m_open.append(t);
    return t.id;
}

void PaperBook::setFeatures(qint64 id, const EntryFeatures &features)
{
    const qsizetype idx = indexOf(id);
    if (idx >= 0) {
        m_open[idx].features = features;
    }
}

void PaperBook::setEntryCompositeConf(qint64 id, double confidence)
{
    const qsizetype idx = indexOf(id);
    if (idx >= 0) {
        m_open[idx].entryCompositeConf = confidence;
    }
}

void PaperBook::mark(qint64 id, double closeRate, bool live, const QDateTime &at)
{
    const qsizetype idx = indexOf(id);
    if ((idx < 0) || (closeRate <= 0.0)) {
        return;
    }
    PaperTrade &t = m_open[idx];
    t.markRate = closeRate;
    t.markLive = live;
    t.markTime = at;
    // The high-water mark of what this position was worth, which is what the
    // give-back exit compares against.
    t.peakNet = std::max(t.peakNet, t.netPnl());
}

void PaperBook::accrueRollover(qint64 id, const InstrumentFees &fees, double eurPerUsd,
                                const QDateTime &now)
{
    const qsizetype idx = indexOf(id);
    if (idx < 0) {
        return;
    }
    PaperTrade &t = m_open[idx];
    const qint32 nights = paperRolloverNights(t.feesChargedTo, now);
    if (nights <= 0) {
        return;
    }
    const double cost = paperRolloverCost(t, fees, nights, eurPerUsd);
    t.feesPaid += cost;
    t.nightsCharged += nights;
    t.feesChargedTo = now;
    m_cash -= cost;
    m_costsPaid += cost;
}

PaperClosedTrade PaperBook::close(qint64 id, double closeRate, double spreadPct,
                                   CloseReason reason, const QDateTime &now)
{
    PaperClosedTrade done;
    const qsizetype idx = indexOf(id);
    if (idx < 0) {
        return done;
    }
    const PaperTrade t = m_open.at(idx);
    // Not "exit": a local shadowing ::exit reads as a call to it in tooling, and it
    // is the closing RATE.
    const double exitRate = (closeRate > 0.0) ? closeRate : t.effectiveRate();
    const double closeCost = paperHalfSpreadCost(t.stake, t.leverage, spreadPct);
    const double gross = paperGrossPnl(t.stake, t.leverage, t.openRate, exitRate, t.isBuy);

    done.id = t.id;
    done.symbol = t.symbol;
    done.isBuy = t.isBuy;
    done.stake = t.stake;
    done.leverage = t.leverage;
    done.openRate = t.openRate;
    done.closeRate = exitRate;
    done.openTime = t.openTime;
    done.closeTime = now;
    done.grossPnl = gross;
    done.openCost = t.openCost;
    done.closeCost = closeCost;
    done.feesPaid = t.feesPaid;
    done.netPnl = gross - done.totalCost();
    done.reason = reason;
    done.features = t.features;   // the record keeps WHY it was opened, not just how it ended

    // The stake comes back with the gross result; the exit spread is paid now
    // (the rollover already left cash as it accrued).
    m_cash += (t.stake + gross - closeCost);
    m_costsPaid += closeCost;
    m_realized += done.netPnl;
    if (now.isValid()) {
        if (m_day.date != now.date()) {
            m_day = BotDay{};   // a new date starts from zero, whatever yesterday did
            m_day.date = now.date();
        }
        m_day.realized += done.netPnl;
        ++m_day.closed;
    }
    m_open.removeAt(idx);
    m_closed.append(done);
    return done;
}

QJsonObject PaperBook::toJson() const
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), kBookSchema);
    root.insert(QStringLiteral("startEquity"), m_cfg.startEquity);
    root.insert(QStringLiteral("cash"), m_cash);
    root.insert(QStringLiteral("realized"), m_realized);
    root.insert(QStringLiteral("costsPaid"), m_costsPaid);
    root.insert(QStringLiteral("nextId"), m_nextId);

    QJsonArray openArr;
    for (const PaperTrade &t : m_open) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), t.id);
        o.insert(QStringLiteral("symbol"), t.symbol);
        o.insert(QStringLiteral("instrumentId"), t.instrumentId);
        o.insert(QStringLiteral("isBuy"), t.isBuy);
        o.insert(QStringLiteral("stake"), t.stake);
        o.insert(QStringLiteral("leverage"), t.leverage);
        o.insert(QStringLiteral("openRate"), t.openRate);
        o.insert(QStringLiteral("slRate"), t.slRate);
        o.insert(QStringLiteral("tpRate"), t.tpRate);
        o.insert(QStringLiteral("openCost"), t.openCost);
        o.insert(QStringLiteral("feesPaid"), t.feesPaid);
        o.insert(QStringLiteral("nightsCharged"), t.nightsCharged);
        o.insert(QStringLiteral("markRate"), t.markRate);
        o.insert(QStringLiteral("peakNet"), t.peakNet);
        o.insert(QStringLiteral("openTime"), timeStr(t.openTime));
        o.insert(QStringLiteral("feesChargedTo"), timeStr(t.feesChargedTo));
        o.insert(QStringLiteral("entryConfidence"), t.entryConfidence);
        o.insert(QStringLiteral("entryCompositeConf"), t.entryCompositeConf);
        o.insert(QStringLiteral("entryBasis"), t.entryBasis);
        o.insert(QStringLiteral("features"), featuresToJson(t.features));
        openArr.append(o);
    }
    root.insert(QStringLiteral("open"), openArr);

    // Today's ledger, so a restart does not hand the bot a fresh daily budget it
    // has already spent (REQ-F-031). An older file without it simply starts a day.
    QJsonObject ledger;
    ledger.insert(QStringLiteral("date"), m_day.date.isValid()
                                              ? m_day.date.toString(Qt::ISODate)
                                              : QString());
    ledger.insert(QStringLiteral("realized"), m_day.realized);
    ledger.insert(QStringLiteral("opened"), m_day.opened);
    ledger.insert(QStringLiteral("closed"), m_day.closed);
    root.insert(QStringLiteral("day"), ledger);

    QJsonArray closedArr;
    for (const PaperClosedTrade &c : m_closed) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), c.id);
        o.insert(QStringLiteral("symbol"), c.symbol);
        o.insert(QStringLiteral("isBuy"), c.isBuy);
        o.insert(QStringLiteral("stake"), c.stake);
        o.insert(QStringLiteral("leverage"), c.leverage);
        o.insert(QStringLiteral("openRate"), c.openRate);
        o.insert(QStringLiteral("closeRate"), c.closeRate);
        o.insert(QStringLiteral("openTime"), timeStr(c.openTime));
        o.insert(QStringLiteral("closeTime"), timeStr(c.closeTime));
        o.insert(QStringLiteral("grossPnl"), c.grossPnl);
        o.insert(QStringLiteral("openCost"), c.openCost);
        o.insert(QStringLiteral("closeCost"), c.closeCost);
        o.insert(QStringLiteral("feesPaid"), c.feesPaid);
        o.insert(QStringLiteral("netPnl"), c.netPnl);
        o.insert(QStringLiteral("reason"), static_cast<int>(c.reason));
        closedArr.append(o);
    }
    root.insert(QStringLiteral("closed"), closedArr);
    return root;
}

bool PaperBook::fromJson(const QJsonObject &obj)
{
    if (obj.value(QStringLiteral("schema")).toInt() != kBookSchema) {
        return false;  // unknown format: keep the current books rather than guess
    }
    QList<PaperTrade> restoredOpen;
    const QJsonArray openArr = obj.value(QStringLiteral("open")).toArray();
    for (const auto &v : openArr) {
        const QJsonObject o = v.toObject();
        PaperTrade t;
        t.id = static_cast<qint64>(jsonNum(o, "id"));
        t.symbol = jsonStr(o, "symbol");
        t.instrumentId = static_cast<qint64>(jsonNum(o, "instrumentId"));
        t.isBuy = o.value(QStringLiteral("isBuy")).toBool();
        t.stake = jsonNum(o, "stake");
        t.leverage = static_cast<qint32>(jsonNum(o, "leverage"));
        t.openRate = jsonNum(o, "openRate");
        t.slRate = jsonNum(o, "slRate");
        t.tpRate = jsonNum(o, "tpRate");
        t.openCost = jsonNum(o, "openCost");
        t.feesPaid = jsonNum(o, "feesPaid");
        t.nightsCharged = static_cast<qint32>(jsonNum(o, "nightsCharged"));
        t.markRate = jsonNum(o, "markRate");
        t.peakNet = jsonNum(o, "peakNet");
        t.openTime = jsonTime(o, "openTime");
        t.feesChargedTo = jsonTime(o, "feesChargedTo");
        t.entryConfidence = jsonNum(o, "entryConfidence");
        t.entryCompositeConf = jsonNum(o, "entryCompositeConf");
        t.entryBasis = jsonStr(o, "entryBasis");
        t.features = featuresFromJson(o.value(QStringLiteral("features")).toObject());
        restoredOpen.append(t);
    }
    QList<PaperClosedTrade> restoredClosed;
    const QJsonArray closedArr = obj.value(QStringLiteral("closed")).toArray();
    for (const auto &v : closedArr) {
        const QJsonObject o = v.toObject();
        PaperClosedTrade c;
        c.id = static_cast<qint64>(jsonNum(o, "id"));
        c.symbol = jsonStr(o, "symbol");
        c.isBuy = o.value(QStringLiteral("isBuy")).toBool();
        c.stake = jsonNum(o, "stake");
        c.leverage = static_cast<qint32>(jsonNum(o, "leverage"));
        c.openRate = jsonNum(o, "openRate");
        c.closeRate = jsonNum(o, "closeRate");
        c.openTime = jsonTime(o, "openTime");
        c.closeTime = jsonTime(o, "closeTime");
        c.grossPnl = jsonNum(o, "grossPnl");
        c.openCost = jsonNum(o, "openCost");
        c.closeCost = jsonNum(o, "closeCost");
        c.feesPaid = jsonNum(o, "feesPaid");
        c.netPnl = jsonNum(o, "netPnl");
        c.reason = static_cast<CloseReason>(o.value(QStringLiteral("reason")).toInt());
        restoredClosed.append(c);
    }

    m_cfg.startEquity = jsonNum(obj, "startEquity");
    m_cash = jsonNum(obj, "cash");
    m_realized = jsonNum(obj, "realized");
    m_costsPaid = jsonNum(obj, "costsPaid");
    m_nextId = std::max<qint64>(1, static_cast<qint64>(jsonNum(obj, "nextId")));
    m_open = restoredOpen;
    m_closed = restoredClosed;
    const QJsonObject ledger = obj.value(QStringLiteral("day")).toObject();
    m_day = BotDay{};
    m_day.date = QDate::fromString(jsonStr(ledger, "date"), Qt::ISODate);
    m_day.realized = jsonNum(ledger, "realized");
    m_day.opened = static_cast<qint32>(jsonNum(ledger, "opened"));
    m_day.closed = static_cast<qint32>(jsonNum(ledger, "closed"));
    return true;
}

} // namespace trading
