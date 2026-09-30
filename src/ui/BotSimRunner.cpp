// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/BotSimRunner.h"

#include "domain/PositionMath.h"   // priceDecimals + the quote-freshness bound
#include "services/EtoroClient.h"
#include "domain/Forecasting.h"
#include "domain/IndexConfluence.h"
#include "domain/Indicators.h"
#include "domain/InstrumentCatalog.h"
#include "domain/LeadSignal.h"
#include "domain/SwingPullbackStrategy.h"
#include "services/OllamaAdvisor.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>
#include <utility>

using trading::CloseReason;
using trading::PaperClosedTrade;
using trading::PaperStats;
using trading::PaperTrade;

QString botMoney(double value)
{
    return QStringLiteral("%1%2 EUR").arg((value > 0.0) ? QStringLiteral("+") : QString())
        .arg(value, 0, 'f', 2);
}

QString botPlain(double value)
{
    return QStringLiteral("%1 EUR").arg(value, 0, 'f', 2);
}

QString botRate(double value)
{
    return QStringLiteral("%1").arg(value, 0, 'f', trading::priceDecimals(value));
}

namespace {

// How often the bot re-marks its simulated positions and re-checks their exits.
// The entry side runs on the all-instruments scan instead (~5 min), which is what
// produces new decisions; there is nothing to gain from evaluating entries faster
// than the data behind them changes.
constexpr int kTickMs = 5000;
// How long a pure-mark tick may leave the book unsaved. The marks move every tick above,
// but nothing a mark carries (P/L, peakNet, accrued rollover) is worth a full atomic
// rewrite of botsim.json twelve times a minute; a minute bounds what a crash can lose to
// something a restart re-marks within seconds anyway. Shape changes never wait.
constexpr qint64 kMarkSaveIntervalSecs = 60;
// How old a local model's proposal may be when it lands and still be acted on.
// A CPU model legitimately takes tens of seconds, and another scan may well have
// completed meanwhile — that is fine, because the entry is re-validated against
// live quotes when it opens. What is NOT fine is reasoning that predates the
// market by more than a scan cycle, so the answer is dropped past this bound.
constexpr qint64 kProposalMaxAgeMs = qint64{5} * 60 * 1000;
// Persisted books (REQ-F-029: an experiment spans days, not one session).
constexpr auto kStoreFile = "botsim.json";
// The strategy version every ledger row of the composite/AI bot carries
// (Prediction::strategyVersion). v2 marks the switch of 2026-09-29: the "previous five
// minutes" baseline comes from the 1-minute series (before it, from the last six HOURLY
// scan closes — five hours) and the row is priced by the fill's own rule (before it, the
// hourly last close), so the earlier rows measure a different thing. They carry an EMPTY
// version, and reportForecast scores ONLY rows of this version: the old ones stay in the
// file for anyone scoring them on their own, but they never enter the runner's record —
// filtering is what the field's contract promises, and the filter has to be applied
// somewhere for it to be true.
constexpr auto kCompositeStrategyVersion = "composite-v2";
// The ONE age rule for a per-tick quote in this runner, shared by the entry pricing
// (sidesFor) and the mark (markFor): a quote whose own stamp is older than this is
// neither traded off nor reported as a live mark. The bound is deliberately the
// open-trades table's own trading::kQuoteStaleMs (120 s) and not a second number —
// two thresholds for "stale" in one process would let the bot open a trade off a
// quote the window beside it already flags as a stale row. The runner's 5 s tick
// against a scan cycle of minutes needs nothing longer than the feed's own bound.
bool perTickQuoteFresh(const Quote &quote)
{
    return trading::quoteIsFresh(quote, QDateTime::currentDateTimeUtc(), trading::kQuoteStaleMs);
}
// How many newly closed trades it takes before the outcome model is refitted. Low
// enough that a running experiment keeps learning, high enough that the fit is not
// repeated for a single new example (REQ-F-033).
constexpr qint64 kRetrainEvery = 25;
// How many instruments the local model is SHOWN per scan. Above the decision
// window's handful, because the bot reports the model's read per instrument and an
// instrument it never saw cannot have one — and bounded, because a small model
// answers a long prompt worse than a short one (REQ-F-034).
constexpr qsizetype kAskedCandidates = 14;

// The swing strategy's own config, shared between the entry path
// (considerSwingEntries) and the exit path (applySwingExit) — one instance, so the
// two can never disagree about, say, which EMA period counts as "the slow one".
// Function-local static variables, not namespace-scope ones: cert-err58-cpp is enabled
// in this project's .clang-tidy specifically because a namespace-scope global's
// construction can throw before main() with nothing able to catch it — a function-local
// variable's first call happens inside ordinary control flow instead.
const trading::SwingPullbackConfig &swingConfig()
{
    static const trading::SwingPullbackConfig config;
    return config;
}

const trading::SwingPullbackStrategyV1 &swingStrategy()
{
    static const trading::SwingPullbackStrategyV1 strategy(swingConfig());
    return strategy;
}

qsizetype indexOfTrade(const QList<PaperTrade> &trades, qint64 id)
{
    for (qsizetype i = 0; i < trades.size(); ++i) {
        if (trades.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

// The fields EVERY decision line shares, in one place (REQ-F-029). The traded path and
// the refused path record the same evaluation, so they must not drift apart in what they
// say about it — and writing the eight common fields twice is exactly how they would.
// The caller sets only what distinguishes the two: `traded`, the refusal `code`, and the
// geometry a trade has and a refusal does not.
trading::DecisionNote decisionNoteFor(const QString &symbol, qint32 dir, double confidence,
                                      const trading::CandidateInput &in, const QDateTime &at)
{
    trading::DecisionNote note;
    note.at = at;
    note.symbol = symbol;
    note.dir = dir;
    note.confidence = confidence;
    note.leadStrength = in.leadStrength;
    note.leadMeasured = in.leadMeasured;
    note.leadUnknowns = in.leadUnknowns;
    return note;
}

// One advisor pick as the domain's plain proposal: the side as ±1, and the model's
// spelling mapped onto a tradable instrument (kept unresolved when it is not one,
// so the gate can refuse it with that reason instead of dropping it silently).
trading::AiProposal normalizePick(const AiDecision &pick, const QString &source,
                                  const QStringList &known)
{
    trading::AiProposal proposal;
    proposal.ok = true;
    proposal.symbol = pick.symbol;
    proposal.confidence = pick.confidence;
    proposal.leverage = pick.leverage;
    proposal.rationale = pick.rationale;
    proposal.source = source;
    if (pick.action == QStringLiteral("BUY")) {
        proposal.dir = 1;
    } else if (pick.action == QStringLiteral("SELL")) {
        proposal.dir = -1;
    } else if (pick.action == QStringLiteral("CLOSE")) {
        // A verdict about a position the bot HOLDS, not a new trade (REQ-F-032):
        // dir stays 0, so the entry gate ignores it and only the hold review acts.
        proposal.exitNow = true;
    }
    proposal.resolvedSymbol = trading::matchProposalSymbol(proposal.symbol, known);
    return proposal;
}

// "BUY SPX500 (conf 71, lev x5); SELL GOLD (conf 44, lev x2)" — the whole answer on
// one line, for the log and the window.
QString describePicks(const QList<trading::AiProposal> &picks)
{
    QStringList lines;
    for (const trading::AiProposal &p : picks) {
        lines << QStringLiteral("%1 %2%3 (conf %4, lev x%5)")
                     .arg(p.exitNow ? QStringLiteral("CLOSE")
                                    : ((p.dir == 0) ? QStringLiteral("HOLD")
                                                    : ((p.dir > 0) ? QStringLiteral("BUY")
                                                                   : QStringLiteral("SELL"))),
                          p.resolvedSymbol.isEmpty() ? p.symbol : p.resolvedSymbol,
                          p.resolvedSymbol.isEmpty() ? QStringLiteral(" [not tradable here]")
                                                     : QString())
                     .arg(p.confidence, 0, 'f', 0)
                     .arg(p.leverage);
    }
    return lines.join(u"; ");
}


} // namespace

BotSimRunner::BotSimRunner(EtoroClient *client, OllamaAdvisor *ai, QObject *parent,
                           QString storeFileName)
    : QObject(parent), m_client(client), m_ai(ai), m_timer(new QTimer(this)),
      m_storeFile(std::move(storeFileName))
{
    load();
    acquireBookLock();
    loadModel();
    // What the bot has learned so far, and how much say it gets. Off by default:
    // a model that has never been trained must not quietly change what trades.
    m_netMode = trading::botNetModeFromWord(qEnvironmentVariable("TRADINGAPP_BOT_NET"));
    static_cast<void>(connect(&m_training, &QFutureWatcher<trading::TrainResult>::finished, this,
                              &BotSimRunner::onTrainingDone));
    // A machine running this unattended has no one to press the button, so it can
    // be asked to refit once at start-up as well.
    if (qEnvironmentVariableIsSet("TRADINGAPP_BOT_TRAIN")) {
        QTimer::singleShot(0, this, [this]() { trainFromExperience(); });
    }
    m_timer->setInterval(kTickMs);
    static_cast<void>(connect(m_timer, &QTimer::timeout, this, &BotSimRunner::tick));
    if (m_ownsBook) {
        m_timer->start();   // marking runs always: the books must stay current even
                            // while disarmed, or a restart would report stale P/L.
    }
    // …unless another process holds the book: then IT marks, and the timer never starts
    // here — the minimal guard, since tick() is the only path that marks and exits
    // between scans (onDecisions guards itself the same way).

    // load() ran before any consumer could connect, so its verdict is reported from
    // the event loop instead of from the constructor — otherwise the one line that
    // says whether a multi-day experiment is still running would be emitted into
    // the void. The same goes for the book being held elsewhere.
    if (!m_restoreNote.isEmpty() || !m_ownsBook) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_restoreNote.isEmpty()) {
                emit log(m_restoreNote, false);
            }
            if (!m_ownsBook) {
                emit log(bookHolderLine(), true);
            }
            emit changed();
        });
    }

    // Persist on shutdown rather than in a destructor: a long experiment must
    // survive the app being closed mid-session, and aboutToQuit runs while the
    // object graph is still intact.
    static_cast<void>(connect(qApp, &QCoreApplication::aboutToQuit, this,
                              [this]() { save(); }));

    if (m_client != nullptr) {
        static_cast<void>(connect(m_client, &EtoroClient::fxRateUpdated, this,
                                  [this](double eurPerUsd) { m_eurPerUsd = eurPerUsd; }));
        static_cast<void>(connect(m_client, &EtoroClient::tradeabilityUpdated, this,
                                  [this](const QSet<QString> &open) {
                                      m_tradeable = open;
                                      m_tradeabilityKnown = true;
                                  }));
    }
    if (m_ai != nullptr) {
        static_cast<void>(
            connect(m_ai, &OllamaAdvisor::proposalsReady, this, &BotSimRunner::onProposals));
        // How long the model took and how much of its context the prompt used: on a CPU a
        // 7B model's answer time is what decides whether it stays inside a scan cycle, and a
        // prompt close to the context window is one evidence line away from being cut.
        static_cast<void>(connect(
            m_ai, &OllamaAdvisor::generationUsage, this,
            [this](qint32 promptTokens, qint32 answerTokens, double seconds, bool nearLimit) {
                emit log(QStringLiteral("Local model: %1 s, prompt %2 + answer %3 tokens "
                                        "(context %4)%5")
                             .arg(seconds, 0, 'f', 1)
                             .arg(promptTokens)
                             .arg(answerTokens)
                             .arg(OllamaAdvisor::kContextTokens)
                             .arg(nearLimit ? QStringLiteral(" — prompt near the context "
                                                             "limit, a longer one would be cut")
                                            : QString()),
                         nearLimit);
            }));
        static_cast<void>(connect(m_ai, &OllamaAdvisor::availability, this,
                                  [this](bool ok, const QString &detail, const QStringList &) {
                                      m_aiStatus = detail;
                                      emit log(QStringLiteral("Local model (Ollama): %1").arg(detail),
                                               !ok);
                                      emit changed();
                                  }));
        checkAi();  // state the model's availability up front, not on first use
    }
    syncQuoteInterest();
}

trading::PaperPerformance BotSimRunner::performance() const
{
    return trading::paperPerformance(m_book.closedTrades(), m_book.config().startEquity,
                                     m_book.config().dailyProfitTarget);
}

trading::LiveReadiness BotSimRunner::liveReadiness() const
{
    return trading::paperLiveReadiness(performance(), trading::LiveGateConfig{});
}

void BotSimRunner::checkAi()
{
    if (m_ai != nullptr) {
        m_ai->checkAvailability();
    }
}

void BotSimRunner::applyDailyRules(double target, double lossLimit)
{
    trading::BotConfig cfg = m_book.config();
    if (qFuzzyCompare(cfg.dailyProfitTarget, target)
        && qFuzzyCompare(cfg.dailyLossLimit, lossLimit)) {
        return;
    }
    cfg.dailyProfitTarget = target;
    cfg.dailyLossLimit = lossLimit;
    m_book.setConfig(cfg);
    // The rules the day is judged by belong in the record, like the AI mode does.
    emit log(QStringLiteral("BOT SIM daily rules: target %1 EUR, loss limit %2 EUR%3")
                 .arg(target, 0, 'f', 2)
                 .arg(lossLimit, 0, 'f', 2)
                 .arg(((target <= 0.0) || (lossLimit <= 0.0))
                          ? QStringLiteral(" (a rule set to 0 is switched off)")
                          : QString()),
             false);
    save();   // a rule change is structural: it must not wait for the next mark save
}

void BotSimRunner::setAiMode(trading::BotAiMode mode)
{
    trading::BotConfig cfg = m_book.config();
    if (cfg.aiMode == mode) {
        return;
    }
    cfg.aiMode = mode;
    m_book.setConfig(cfg);
    // Worth a log line: this changes what the experiment measures, so the record
    // has to show when the rules changed.
    emit log(QStringLiteral("BOT SIM AI mode: %1 (%2)")
                 .arg(trading::botAiModeWord(mode),
                      (mode == trading::BotAiMode::Off)
                          ? QStringLiteral("the composite decides; a proposal is logged only")
                          : ((mode == trading::BotAiMode::Confirm)
                                 ? QStringLiteral("only the model's pick, and only while the "
                                                  "composite agrees")
                                 : QStringLiteral("the model's pick and side are traded"))),
             false);
    save();   // the mode is persisted (a restart must not change what the experiment measures)
    emit changed();
}

QString BotSimRunner::storePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(m_storeFile.isEmpty() ? QLatin1String(kStoreFile)
                                                    : QLatin1String(m_storeFile.toLatin1()));
}

void BotSimRunner::setFocusSymbols(const QStringList &symbols)
{
    trading::BotConfig cfg = m_book.config();
    if (cfg.focusSymbols == symbols) {
        return;
    }
    cfg.focusSymbols = symbols;
    m_book.setConfig(cfg);
    emit log(QStringLiteral("BOT SIM focus: %1")
                 .arg(symbols.isEmpty() ? QStringLiteral("whole catalog")
                                        : symbols.join(QStringLiteral(", "))),
             false);
    save();
}

void BotSimRunner::setCoreFocusSymbols(const QStringList &symbols)
{
    trading::BotConfig cfg = m_book.config();
    if (cfg.coreFocusSymbols == symbols) {
        return;
    }
    cfg.coreFocusSymbols = symbols;
    m_book.setConfig(cfg);
    emit log(QStringLiteral("BOT SIM core focus (normal bar): %1")
                 .arg(symbols.isEmpty() ? QStringLiteral("none — every focus symbol equal")
                                        : symbols.join(QStringLiteral(", "))),
             false);
    save();
}

void BotSimRunner::setArmed(bool armed)
{
    // Every way of arming goes through here — the button, TRADINGAPP_BOT_ARM at start-up,
    // the console's launch — and none of them may arm a book another process is running.
    if (armed && !m_ownsBook) {
        emit log(bookHolderLine() + QStringLiteral(" Arming refused."), true);
        // The GUI's arm button is checkable and only re-reads armed() on `changed`;
        // without this it stays pressed on a runner that just refused, until the next
        // scan happens to emit it — an armed-looking control over a disarmed bot.
        emit changed();
        return;
    }
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    const PaperStats s = m_book.stats();
    if (m_armed) {
        emit log(QStringLiteral("BOT SIM ARMED — simulated money only; the simulation itself "
                                "never sends an order to eToro. Equity %1, %2 open, %3 closed.")
                     .arg(botPlain(s.equity))
                     .arg(s.openTrades)
                     .arg(s.closedTrades),
                 false);
    } else {
        emit log(QStringLiteral("BOT SIM disarmed — no new simulated trades; the %1 open one(s) "
                                "keep being marked.")
                     .arg(s.openTrades),
                 false);
    }
    // The armed flag is persisted so an unattended experiment survives a restart — and
    // it has to reach the disk NOW, not on the next mark save: those are throttled to
    // once a minute (saveAfterMarks), and a bot armed 50 s before a power loss that
    // restarts DISARMED is exactly the silent stop the persistence exists to prevent.
    save();
    emit changed();
}

void BotSimRunner::tick()
{
    markAndExit();
}

BotSimRunner::Mark BotSimRunner::markFor(const PaperTrade &trade) const
{
    Mark mark;
    if (m_client == nullptr) {
        return mark;
    }
    // The MID, matching how the entry was priced: the closing half-spread is
    // charged as its own line item when the trade closes, so marking at the bid (a
    // long's real close rate) would bill that crossing a second time. Only a fresh
    // quote counts as a live mark, exactly as in the real open-trades table.
    const QHash<qint64, Quote> &quotes = m_client->quotes();
    const auto it = quotes.constFind(trade.instrumentId);
    if (it != quotes.constEnd() && it->isValid()) {
        mark.rate = (it->bid + it->ask) / 2.0;
        // A stalled per-tick quote still marks the position (its last print is the best
        // rate there is) but is not a LIVE mark — the window shows it as a stale row.
        mark.live = perTickQuoteFresh(*it);
        if (mark.rate > 0.0) {
            return mark;
        }
    }
    // No per-tick quote yet: the last bulk snapshot's mid is better than not
    // marking at all, but it is not a live mark and the window says so.
    mark.rate = m_client->lastRateFor(trade.instrumentId);
    if (mark.rate <= 0.0) {
        // Crypto has no warm eToro rate here, so an open crypto position would otherwise
        // never mark or exit off price. Fall back to its 1-minute candle close from the scan
        // sweep (m_symbolSeries, keyed by app symbol) — the same source its entry was priced
        // from. Deliberately still not a live mark (candle-derived, fromCandle semantics).
        const QList<double> series = m_symbolSeries.value(trade.symbol);
        if (!series.isEmpty() && (series.constLast() > 0.0)) {
            mark.rate = series.constLast();
        }
    }
    mark.live = false;
    return mark;
}

void BotSimRunner::markAndExit()
{
    if (m_book.openTrades().isEmpty()) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    // Copy the ids first: closing mutates the list this loop walks.
    QList<qint64> ids;
    ids.reserve(m_book.openTrades().size());
    for (const PaperTrade &t : m_book.openTrades()) {
        ids.append(t.id);
    }
    // The book's SHAPE before the pass: a close, a partial (one more closed record) or a
    // harvest changes one of these counts, and that is what must reach the disk at once.
    const qsizetype openBefore = m_book.openTrades().size();
    const qsizetype closedBefore = m_book.closedTrades().size();

    bool moved = false;
    for (const qint64 id : ids) {
        const qsizetype idx = indexOfTrade(m_book.openTrades(), id);
        if (idx < 0) {
            continue;
        }
        // A COPY, deliberately: closing removes the entry from the book, so a
        // reference into that list would dangle the moment an exit fires.
        const PaperTrade trade = m_book.openTrades().at(idx);
        const Mark mark = markFor(trade);
        if (mark.rate > 0.0) {
            m_book.mark(id, mark.rate, mark.live, now);
            moved = true;
        }
        // Rollover: charged from the instrument's own fee table, fetched once.
        const InstrumentFees fees =
            (m_client != nullptr) ? m_client->feesFor(trade.symbol) : InstrumentFees{};
        if (!fees.isValid() && (m_client != nullptr) && !m_feesRequested.contains(trade.symbol)) {
            static_cast<void>(m_feesRequested.insert(trade.symbol));
            m_client->requestFees(trade.symbol);
        }
        m_book.accrueRollover(id, fees, m_eurPerUsd, now);

        // A position the swing strategy opened is managed ENTIRELY by its own exit path
        // (REQ-F-031's redesign, item 6) — never by paperCloseDecision below, whose
        // SignalFade/GiveBack are tuned for the composite's own conviction signal, which
        // this strategy was never scored against.
        if (!trade.strategyVersion.isEmpty()) {
            static_cast<void>(applySwingExit(trade, mark.rate, now));
            moved = true;   // the rollover accrual above already changed the book
            continue;
        }

        // The exit rules read only fields the accrual above cannot change (side,
        // stop/target, open time), so the copy is as current as the book.
        trading::ExitContext ctx;
        ctx.markRate = mark.rate;
        ctx.dirNow = m_dirBySymbol.value(trade.symbol, 0);
        ctx.confNow = m_confBySymbol.value(trade.symbol, 0.0);
        ctx.now = now;
        // What holding on costs, so the exit can be an economic decision too: the
        // live spread it must still cross and the instrument's own rollover table
        // (REQ-F-029). Unknown fees leave the carry rules silent rather than
        // guessing — the same policy the entry gate applies to an unknown spread.
        ctx.spreadPct = effectiveSpreadPct(trade.symbol);
        ctx.fees = fees;
        ctx.feesKnown = fees.isValid();
        ctx.eurPerUsd = m_eurPerUsd;
        // An ACTIVE keep from the model (not silence) lets a conviction trade ride through
        // carry it has not earned — see ExitContext::aiBacksHold. The stop still protects it
        // and the rollover is still charged.
        ctx.aiBacksHold =
            (m_holdOpinions.value(trade.id).opinion == trading::HoldOpinion::Hold);
        CloseReason reason = trading::paperCloseDecision(trade, ctx, m_book.config());
        if (reason == CloseReason::None) {
            reason = aiExitFor(trade);
        }
        if (reason != CloseReason::None) {
            closeTrade(trade, reason);
            moved = true;
        }
    }
    if (harvestDayTarget(now)) {
        moved = true;
    }
    if (moved) {
        saveAfterMarks(now, openBefore, closedBefore);
        emit changed();   // the window still refreshes every tick; only the DISK waits
    }
}

void BotSimRunner::saveAfterMarks(const QDateTime &now, qsizetype openBefore,
                                  qsizetype closedBefore)
{
    // A structural change is saved where it happens — the open (considerEntries), the
    // reset, the focus/AI-mode/daily-rules setters and setArmed each call save() — and a
    // close, partial or harvest inside THIS pass is caught by the shape check below; the
    // throttle is ONLY for the pass that moved marks and rollover accrual and nothing
    // else. (The arm/AI-mode/daily-rules saves are NOT as they were before the throttle:
    // those changes used to ride on the next 5-second mark save, which the throttle
    // would have stretched to a minute.) The swing strategy's day-granular
    // state (trailing stop, sessions held) rides on the same throttle: it changes at most
    // once a day per position, so a crash inside the minute after it costs one day's
    // trailing step, not a position.
    const bool structural = (m_book.openTrades().size() != openBefore)
                            || (m_book.closedTrades().size() != closedBefore);
    const bool due =
        !m_lastMarkSave.isValid() || (m_lastMarkSave.secsTo(now) >= kMarkSaveIntervalSecs);
    if (!structural && !due) {
        return;
    }
    save();
    m_lastMarkSave = now;
}

bool BotSimRunner::harvestDayTarget(const QDateTime &now)
{
    // What each open position would BOOK if it closed right now: its net so far
    // minus the half-spread it still has to cross. A position whose exit cost is
    // unknown is left out rather than guessed at — the same policy the entry gate
    // and the carry rules apply.
    QList<trading::HarvestOption> options;
    for (const PaperTrade &trade : m_book.openTrades()) {
        // A swing-strategy position is managed by swingExitDecision ONLY — the same
        // dispatch markAndExit applies before paperCloseDecision. The daily target is
        // the composite bot's stopping rule; cutting a multi-session swing trade for
        // it would apply one strategy's discipline to another's book.
        if (!trade.strategyVersion.isEmpty()) {
            continue;
        }
        const double spreadPct = effectiveSpreadPct(trade.symbol);
        if (spreadPct <= 0.0) {
            continue;
        }
        options.append({trade.id,
                        trade.netPnl()
                            - trading::paperHalfSpreadCost(trade.stake, trade.leverage, spreadPct)});
    }
    const qint64 pick = trading::paperHarvestPick(options, m_book.day(), now, m_book.config());
    if (pick == 0) {
        return false;
    }
    const qsizetype idx = indexOfTrade(m_book.openTrades(), pick);
    if (idx < 0) {
        return false;
    }
    closeTrade(m_book.openTrades().at(idx), CloseReason::DayTarget);
    return true;
}

void BotSimRunner::announceOpened(qint64 id, const QString &symbol)
{
    emit tradeOpened(symbol);
    for (const PaperTrade &trade : m_book.openTrades()) {
        if (trade.id == id) {
            emit tradeOpenedDetail(trade);
            return;
        }
    }
}

void BotSimRunner::closeTrade(const PaperTrade &trade, CloseReason reason)
{
    const Mark mark = markFor(trade);
    const double spreadPct = effectiveSpreadPct(trade.symbol);
    const PaperClosedTrade done = m_book.close(trade.id, mark.rate, spreadPct, reason,
                                               QDateTime::currentDateTime());
    if (done.id == 0) {
        return;
    }
    emit tradeClosed(done);
    static_cast<void>(m_holdOpinions.remove(done.id));
    recordExperience(done);
    // Retrain on a cadence rather than on every close: the model only moves once
    // there are new trades in it, and a retrain per trade would be noise.
    if ((m_experienceCount > 0) && ((m_experienceCount % kRetrainEvery) == 0)) {
        trainFromExperience();
    }
    emit log(QStringLiteral("SIM CLOSE %1 %2 @ %3 (%4): net %5 after %6 costs, held %7 h")
                 .arg(done.symbol)
                 .arg(done.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                 .arg(botRate(done.closeRate))
                 .arg(trading::closeReasonWord(done.reason))
                 .arg(botMoney(done.netPnl))
                 .arg(botPlain(done.totalCost()))
                 .arg(done.heldHours(), 0, 'f', 1),
             false);
    syncQuoteInterest();
}

BotSimRunner::Sides BotSimRunner::sidesFor(const QString &symbol, qint64 instrumentId,
                                          const QList<double> &closes) const
{
    Sides sides;
    if (m_client == nullptr) {
        return sides;
    }
    sides.spreadPct = effectiveSpreadPct(symbol);
    const QHash<qint64, Quote> &quotes = m_client->quotes();
    const auto it = quotes.constFind(instrumentId);
    // A per-tick quote exists only for an instrument the bot already holds, and it is
    // the one price here that carries the venue's own stamp. Fresh: it prices the
    // candidate and is live. Stale: the venue says this instrument stopped printing,
    // so the candidate is still priced off the bulk mid below (the scan just priced
    // it) but is NOT live — the gate then refuses it as `no-live-quote`, which is the
    // countable answer; a line per candidate here would be noise (see the Sides note).
    bool stalled = false;
    if (it != quotes.constEnd() && it->isValid()) {
        if (perTickQuoteFresh(*it)) {
            sides.bid = it->bid;
            sides.ask = it->ask;
            sides.ok = true;
            sides.live = true;
            return sides;
        }
        stalled = true;
    }
    // Most instruments have no per-tick quote (nothing is held in them), but the
    // tradeability poll keeps a mid AND a spread warm for every resolved one — so
    // widen the mid by that spread rather than pretending the fill is at the mid.
    double mid = m_client->lastRateFor(instrumentId);
    // CRYPTO (and anything else the eToro id/rate path does not warm here) has no such mid:
    // it never resolves a non-zero instrumentId in this build, so lastRateFor is 0. Fall back
    // to its candle close — which equals the eToro bid to the cent (see the candle-vs-rate
    // note) — so a priced-and-tradable instrument is not refused `no-live-quote` purely for
    // lacking an eToro rate row. The 1-minute series (m_symbolSeries, the Yahoo <TICKER>-USD
    // sweep) comes FIRST because it is the same book markFor marks the position off: an entry
    // filled at the scan row's HOURLY close would open with up to an hour of the coin's move
    // already booked as P/L against a 1-minute mark. The row's own closes stand in only when
    // there is no session series. Still widened by the effective spread, which for crypto is
    // already the modelled 1% floor, so the fill is never at the untouched mid.
    if (mid <= 0.0) {
        const QList<double> session = m_symbolSeries.value(symbol);
        if (!session.isEmpty() && (session.constLast() > 0.0)) {
            mid = session.constLast();
        } else if (!closes.isEmpty() && (closes.constLast() > 0.0)) {
            mid = closes.constLast();
        }
    }
    if ((mid <= 0.0) || (sides.spreadPct <= 0.0)) {
        return sides;
    }
    const double half = mid * (sides.spreadPct / 100.0) / 2.0;
    sides.bid = mid - half;
    sides.ask = mid + half;
    sides.ok = true;
    // Neither fallback carries a stamp, so only a dated stall above can deny liveness.
    sides.live = !stalled;
    return sides;
}

// Which 1-minute series are LIVE, judged the only way this runner can: MarketSnapshot
// carries no stamp for them, and neither producer (the window's map, the console's books)
// ever clears an entry — a sweep that fails, or answers an empty close array (the failure
// mode MarketFeeds documents for equities), leaves the last GOOD series in place, and the
// two indices' series are the CASH index, which Yahoo freezes at the New York close while
// the CFD is scanned for hours more. Both look exactly like a live series to a presence
// test. What separates them is that a live 1-minute feed ALWAYS has new bars between two
// scans minutes apart, so a series identical to the previous scan's has stopped, whatever
// the reason. A series seen for the first time has nothing to be compared with and is not
// yet known live — unknown, the same answer every other unmeasurable read gives — which
// costs the structure read and the baseline of one scan after a restart, and saves an
// evening of a frozen index counted as fresh. Two scans inside one minute (a manual
// refresh) can judge a live series still, and lose one read; that is the cheap side.
void BotSimRunner::adoptSymbolSeries(const QHash<QString, QList<double>> &series)
{
    m_liveSeries.clear();
    for (auto it = series.cbegin(); it != series.cend(); ++it) {
        const auto previous = m_symbolSeries.constFind(it.key());
        if (!it.value().isEmpty() && (previous != m_symbolSeries.cend())
            && (previous.value() != it.value())) {
            m_liveSeries.insert(it.key());
        }
    }
    m_symbolSeries = series;
}

QList<double> BotSimRunner::liveSessionSeries(const QString &symbol) const
{
    return m_liveSeries.contains(symbol) ? m_symbolSeries.value(symbol) : QList<double>{};
}

void BotSimRunner::onDecisions(const QList<trading::DecisionRow> &rows,
                               const trading::MarketSnapshot &snap)
{
    for (const trading::DecisionRow &row : rows) {
        static_cast<void>(m_dirBySymbol.insert(row.symbol, row.dir));
        static_cast<void>(m_confBySymbol.insert(row.symbol, row.confidence));
    }
    // The other two books the reads need, taken from the SAME snapshot the ranking was
    // computed from (REQ-F-070): the volume bars behind the VWAP and up/down-volume
    // reads, and the app's own per-symbol series the futures reads live in. Keyed by
    // APP SYMBOL, not by Yahoo ticker — passing the ticker book here is exactly the
    // mistake that left the futures lead permanently unknown.
    m_referenceVolumes = snap.referenceVolumes;
    adoptSymbolSeries(snap.intradayBySymbol);
    // The read-only views above are all a non-owned book gets: the process holding the
    // book marks, exits and enters on it, and doing so here as well would rewrite its
    // book and ledgers over its own (the entry paths below are unreachable anyway, since
    // such a runner cannot be armed — this stops the exits).
    if (!m_ownsBook) {
        emit changed();
        return;
    }
    // Exits first: a position the ranking has turned against should go before the
    // capital it frees is committed elsewhere.
    markAndExit();

    m_pendingRows = rows;
    m_pendingScan = snap.screenerRows;
    // The swing strategy's own entry path (2026-08-12 redesign, item 5's live wiring) —
    // independent of the composite/AI gate below, and of `m_armed`'s AI-proposal
    // machinery: it has no proposal to wait for, so it runs whether or not a local
    // model is configured. Still gated on the SAME arming switch as every other entry.
    if (m_armed) {
        considerSwingEntries(QDateTime::currentDateTime());
    }
    if (!m_armed) {
        emit changed();
        return;
    }

    // With a local model in play, the entries wait for its answer — it may take
    // tens of seconds, which is why this is a two-step tick rather than a blocking
    // call (REQ-F-030). The evidence is the SAME prompt the decision window builds.
    const bool wantAi = (m_book.config().aiMode != trading::BotAiMode::Off)
                        && (m_ai != nullptr) && m_ai->isConfigured();
    if (wantAi) {
        // Every instrument with a directional call, up to the answer cap: the bot's
        // window reports the model's read per instrument, so it must be shown them
        // (REQ-F-034). The decision window keeps its own shorter list.
        m_evidence = trading::buildDecisionEvidence(rows, snap, kAskedCandidates)
                     + holdEvidence() + crowdEvidenceBlock();
        m_askedSymbols.clear();
        const QStringList &focus = m_book.config().focusSymbols;
        for (const trading::DecisionRow &row : rows) {
            // Only the focus instruments are put in front of the model, so it cannot spend
            // its one answer naming OIL.24-7 while the indices go unread — the `ai-other-pick`
            // refusals that dominated the ledger were exactly that.
            const bool inFocus = focus.isEmpty() || focus.contains(row.symbol);
            if (inFocus && (row.dir != 0) && (m_askedSymbols.size() < kAskedCandidates)) {
                m_askedSymbols.append(row.symbol);
            }
        }
        requestProposal();
        emit changed();
        return;
    }
    considerEntriesForScan();
    emit changed();
}

trading::CloseReason BotSimRunner::aiExitFor(const PaperTrade &trade)
{
    // The model's opinion about HOLDING this position, which is fresh exactly as
    // long as an entry proposal is (REQ-F-032). Silence keeps the trade.
    if (!aiProposalsFresh()) {
        return CloseReason::None;
    }
    const trading::HoldVerdict hold = trading::paperAiHold(
        trade, m_proposals,
        {m_book.config().aiMode, QDateTime::currentDateTime(), effectiveSpreadPct(trade.symbol)},
        m_book.config());
    // Recorded for EVERY open position on every pass, not just the ones being
    // closed: the window shows hold / close / no-opinion per trade, and "the model
    // did not mention this one" has to be visible as its own answer.
    m_holdOpinions.insert(trade.id, hold);
    if (!hold.close) {
        return CloseReason::None;
    }
    emit log(QStringLiteral("SIM EXIT %1: %2").arg(trade.symbol, hold.why), false);
    return CloseReason::AiExit;
}

trading::CandidateInput BotSimRunner::candidateFor(const trading::DecisionRow &row,
                                                  const trading::AiGate &gate,
                                                  const QList<double> &closes,
                                                  const QDateTime &now) const
{
    const qint64 id = m_client->instrumentIdFor(row.symbol);
    const Sides sides = sidesFor(row.symbol, id, closes);
    trading::CandidateInput in;
    in.symbol = row.symbol;
    in.instrumentId = id;
    in.dir = gate.dir;
    // In Lead the matched pick's own conviction decides whether the trade clears
    // the confidence floor; otherwise the composite's does.
    const trading::AiProposal pick =
        (gate.pick >= 0) ? m_proposals.at(gate.pick) : trading::AiProposal{};
    in.confidence = (m_book.config().aiMode == trading::BotAiMode::Lead) ? pick.confidence
                                                                        : row.confidence;
    in.closes = closes;
    in.bid = sides.bid;
    in.ask = sides.ask;
    in.spreadPct = sides.spreadPct;
    // Only the instrument on screen publishes its leverage ladder, so the
    // domain's default ladder stands in — capped by the row's own maximum.
    in.maxLeverage = row.maxLev;
    in.now = now;   // the daily target / loss limit judge TODAY
    // Adding to a position the bot already holds needs the model's own initiative.
    in.aiBacked = (gate.pick >= 0);
    // The churn inputs (REQ-F-034): when this instrument last closed a position,
    // and how many the whole book has opened in the past hour.
    in.lastClosedAt = lastCloseFor(row.symbol);
    // The session's own structure, from the SAME 1-minute series the window's decision
    // engine reads (MarketSnapshot::intradayBySymbol, kept as m_symbolSeries). Never from
    // `closes`: those are the eToro scan's HOURLY candles, and an "opening range" over
    // their first thirty points is a thirty-HOUR range — the gate then refused a fresh
    // opposite break the window never showed. Only a LIVE series (see liveSessionSeries):
    // the two indices' series are the CASH index, frozen from the New York close while the
    // CFD is scanned for hours more, and a break that is hours old is not the FRESH one
    // REQ-F-056 refuses into. A symbol without a live 1-minute series reads 0 (no read),
    // the honest answer; the hourly closes are the volatility source only.
    in.rangeBreakDir = trading::openingRange(liveSessionSeries(row.symbol)).breakDir;
    // How many independent reference reads agree with this side (REQ-F-069). Built once
    // and used for both the count and the combined indication below — the two must be
    // computed from identical inputs or the window contradicts itself. readInputsFor
    // files the instrument's own 1-minute series as `ownSeries` from that same book, so
    // nothing here overrides it — the hourly scan closes once stood in and turned the
    // structure read into the thirty-hour range described above.
    const trading::ReadInputs inputs =
        trading::readInputsFor(row.symbol, m_referenceSeries, m_referenceVolumes, m_symbolSeries);
    const trading::IndexReads reads = trading::indexReads(row.symbol, inputs);
    const trading::Confluence score = trading::confluenceFor(reads, gate.dir);
    in.agreeingReads = score.met;
    in.measuredReads = score.measured();
    // The combined indication (REQ-F-036): the same reads plus the constituent field,
    // the session phase and the regime, scored into one answer. It can only ever
    // reduce what the bot does — veto a trade the evidence contradicts, and bound the
    // leverage — which is why it is computed for every candidate rather than only for
    // the two indices whose window shows it.
    trading::LeadInputs lead;
    lead.symbol = row.symbol;
    lead.reads = reads;
    lead.pulse = trading::heavyweightPulse(row.symbol, m_referenceSeries);
    lead.now = now;
    lead.vixValid = m_vixValid;
    lead.vix = m_vix;
    lead.eventRisk = m_eventRisk;
    lead.term = trading::termStructure(m_referenceSeries);
    lead.compositeDir = gate.dir;
    lead.compositeConfidence = in.confidence;
    const trading::LeadSignal signal = trading::leadSignal(lead);
    in.leadDir = signal.actionable() ? signal.dir : 0;
    in.leadStrength = signal.strength;
    in.leadMaxLeverage = signal.actionable() ? signal.suggestedLeverage : 0;
    in.leadMeasured = signal.measured;
    in.leadUnknowns = signal.unknowns;
    in.opensLastHour = opensInLastHour(now);
    // A 24/7 instrument (crypto) is never "market closed", and the eToro tradeability set does
    // not cover it here — so it would be wrongly excluded by the membership test. Its market is
    // always open; whether it can actually be PRICED is still gated by sides.ok below, so this
    // never opens a crypto trade for which there is no candle.
    in.marketOpen = !m_tradeabilityKnown || m_tradeable.contains(row.symbol)
                    || trading::tradesOnWeekend(row.symbol);
    // Priced AND fresh (a stalled per-tick quote prices the row but is not live — see
    // sidesFor) AND in an open market.
    in.quoteLive = sides.ok && sides.live && in.marketOpen;
    // What the fee table says holding it will cost — the entry prices the round
    // trip, not just the spread (REQ-F-032).
    in.fees = (m_client != nullptr) ? m_client->feesFor(row.symbol) : InstrumentFees{};
    in.feesKnown = in.fees.isValid();
    in.eurPerUsd = m_eurPerUsd;
    return in;
}

QDateTime BotSimRunner::lastCloseFor(const QString &symbol) const
{
    // The record is append-ordered, so the last matching close is the newest one.
    const QList<PaperClosedTrade> &closed = m_book.closedTrades();
    const auto hit = std::find_if(closed.crbegin(), closed.crend(),
                                  [&symbol](const PaperClosedTrade &c) {
                                      return c.symbol == symbol;
                                  });
    return (hit != closed.crend()) ? hit->closeTime : QDateTime{};
}

qint32 BotSimRunner::opensInLastHour(const QDateTime &now) const
{
    // Counted over BOTH books: a position opened and already closed within the hour
    // still spent its spread, which is exactly what the pace limit is about.
    const auto withinTheHour = [&now](const QDateTime &opened) {
        return opened.isValid() && (opened.secsTo(now) < 3600);
    };
    const auto open = m_book.openTrades();
    const auto closed = m_book.closedTrades();
    const qsizetype count =
        std::count_if(open.cbegin(), open.cend(),
                      [&withinTheHour](const PaperTrade &t) { return withinTheHour(t.openTime); })
        + std::count_if(closed.cbegin(), closed.cend(),
                        [&withinTheHour](const PaperClosedTrade &c) {
                            return withinTheHour(c.openTime);
                        });
    return static_cast<qint32>(count);
}

trading::NetVerdict BotSimRunner::applyNetGate(const QString &symbol,
                                              const trading::EntryFeatures &features,
                                              trading::EntrySignal &sig)
{
    // The WHOLE verdict is returned, not just its code: the decision log has to state
    // WHY the learned model stood in the way, and a code alone ("net-refused") explains
    // nothing to the person reading the file later.
    const trading::NetVerdict verdict =
        trading::paperNetGate(m_net, features, m_netMode, trading::NetGateConfig{});
    if (!verdict.allow) {
        emit log(QStringLiteral("SIM SKIP %1: %2").arg(symbol, verdict.why), false);
        return verdict;
    }
    // A scored-but-allowed candidate carries the number into its basis line, so the
    // record shows what the model thought of a trade it did not stop.
    if (verdict.scored) {
        sig.basis += QStringLiteral(" [net %1]").arg(verdict.score, 0, 'f', 2);
    }
    return verdict;
}

trading::EntryFeatures BotSimRunner::featuresFor(const trading::CandidateInput &in,
                                                const trading::EntrySignal &sig, double stake,
                                                const QDateTime &now)
{
    // Everything that was true about this entry, as numbers — the input half of the
    // training example the trade becomes when it closes (REQ-F-033).
    trading::EntryFeatures f;
    f.confidence = in.confidence;
    f.volPct = sig.volPct;
    f.stopPct = (sig.fillRate > 0.0)
                    ? (qAbs(sig.fillRate - sig.slRate) / sig.fillRate * 100.0)
                    : 0.0;
    f.targetPct = (sig.fillRate > 0.0)
                      ? (qAbs(sig.tpRate - sig.fillRate) / sig.fillRate * 100.0)
                      : 0.0;
    f.spreadPct = sig.spreadPct;
    f.edgeOverCost = trading::paperEntryEconomics(sig, stake, in, m_book.config()).ratio;
    f.leverage = sig.leverage;
    f.dir = sig.isBuy ? 1 : -1;
    const QDateTime utc = now.toUTC();
    f.hourUtc = utc.time().hour();
    f.dayOfWeek = utc.date().dayOfWeek();
    f.aiBacked = in.aiBacked;
    return f;
}

trading::EntryFeatures BotSimRunner::swingFeaturesFor(const trading::EntrySignal &sig,
                                                      const QList<trading::DailyBar> &bars,
                                                      double stake, const QDateTime &now)
{
    // The swing entry never set features, and EntryFeatures::isValid() is `volPct > 0`,
    // so recordExperience dropped EVERY swing record — the one-label-per-position rule
    // there had no producer to act on, and the strategy's closes taught the network
    // nothing. The same vector as featuresFor, from what this entry actually knew:
    // "per bar" is per SESSION here (the daily bars the strategy read, over its own ATR
    // window, exactly as heldHours labels the hold in hours either way), the target is
    // the partial's 2R level (the swing has no fixed target; that is where profit is
    // first taken), and the edge is priced over the spread round trip alone — the
    // swing's carry over a hold of days is its exit rules' business, and the composite's
    // intraday horizon is the only one paperEntryEconomics has. The composite's
    // contemporaneous conviction is recorded as the confidence (0 when the scan had no
    // row for the symbol): the swing does not consult it, but it WAS true about the entry.
    QList<double> closes;
    closes.reserve(bars.size());
    for (const trading::DailyBar &bar : bars) {
        closes.append(bar.close);
    }
    trading::EntrySignal probe = sig;
    probe.volPct = trading::volatilityPct(
        closes, std::min<qsizetype>(swingConfig().atrPeriod, closes.size() - 1));
    const double stopDistance = qAbs(sig.fillRate - sig.slRate);
    probe.tpRate = sig.isBuy ? (sig.fillRate + (swingConfig().partialTargetR * stopDistance))
                             : (sig.fillRate - (swingConfig().partialTargetR * stopDistance));
    trading::CandidateInput in;
    in.symbol = sig.symbol;
    in.confidence = m_confBySymbol.value(sig.symbol, 0.0);
    in.spreadPct = sig.spreadPct;
    in.now = now;
    in.feesKnown = false;
    return featuresFor(in, probe, stake, now);
}

void BotSimRunner::recordExperience(const PaperClosedTrade &done)
{
    // One JSON line per closed POSITION, appended and never rewritten: the bot's own
    // history is the training set, and a file that is only ever appended to cannot
    // lose it to a crash mid-write (REQ-F-033).
    //
    // A partial close is not a closed setup. The position it came from is still open
    // under the SAME id with the SAME entry features, so writing it as an example would
    // teach one setup twice — and label each copy with a fraction of the result, the
    // remainder's share being NOT the trade's net (a 2R partial banked in profit followed
    // by a trailing stop below entry is one winning trade, not one win and one loss).
    // Bank its net here and label the final close with the whole.
    if (done.partial) {
        m_partialNetById[done.id] += done.netPnl;
        return;
    }
    const double label = done.netPnl + m_partialNetById.take(done.id);
    if (!done.features.isValid()) {
        return;   // a trade from before the features existed teaches nothing
    }
    QFile file(experiencePath());
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QJsonObject rec;
    rec.insert(QStringLiteral("symbol"), done.symbol);
    rec.insert(QStringLiteral("closedAt"), done.closeTime.toString(Qt::ISODate));
    rec.insert(QStringLiteral("heldHours"), done.heldHours());
    // The label is the position's net over every leg; `costs` stays the final
    // leg's own record (the trainers read netPnl and the features only).
    rec.insert(QStringLiteral("netPnl"), label);
    rec.insert(QStringLiteral("costs"), done.totalCost());
    rec.insert(QStringLiteral("reason"), trading::closeReasonWord(done.reason));
    QJsonObject features;
    const QStringList names = trading::entryFeatureNames();
    const QList<double> values = trading::entryFeatureValues(done.features);
    for (qsizetype i = 0; (i < names.size()) && (i < values.size()); ++i) {
        features.insert(names.at(i), values.at(i));
    }
    rec.insert(QStringLiteral("features"), features);
    static_cast<void>(file.write(QJsonDocument(rec).toJson(QJsonDocument::Compact) + "\n"));
    file.close();
    ++m_experienceCount;
}

// The three books share ONE base name so a second runner (the advise console's focused
// instrument) never writes the main bot's decision or experience logs — the store-file
// override was pointless if these stayed fixed.
QString BotSimRunner::siblingPath(const QString &defaultName, const QString &suffix) const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (m_storeFile.isEmpty()) {
        return QDir(dir).filePath(defaultName);
    }
    QString base = m_storeFile;
    if (base.endsWith(QLatin1String(".json"))) {
        base.chop(5);
    }
    return QDir(dir).filePath(base + suffix);
}

QString BotSimRunner::experiencePath() const
{
    return siblingPath(QStringLiteral("botsim-experience.jsonl"),
                       QStringLiteral("-experience.jsonl"));
}

QString BotSimRunner::ledgerPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QStringLiteral("prediction-ledger.jsonl"));
}

QString BotSimRunner::decisionLogPath() const
{
    // .log, not .jsonl: this one is meant to be opened, tailed and grepped by a person.
    return siblingPath(QStringLiteral("botsim-decisions.log"), QStringLiteral("-decisions.log"));
}

QString BotSimRunner::modelPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QStringLiteral("botnet.json"));
}

QList<trading::TrainingExample> BotSimRunner::readExperience() const
{
    // The append-only log, oldest first — the order matters: the trainer holds back
    // the LATEST trades to score itself on (REQ-F-033).
    QList<trading::TrainingExample> examples;
    QFile file(experiencePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return examples;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const std::optional<trading::TrainingExample> example =
            trading::experienceExampleFrom(QJsonDocument::fromJson(line).object());
        if (example.has_value()) {
            examples.append(*example);
        }
    }
    file.close();
    return examples;
}

void BotSimRunner::trainFromExperience()
{
    // Training is seconds of arithmetic at most, but it is arithmetic proportional
    // to the whole record — so it runs off the GUI thread like the other heavy work
    // (REQ-N-006), and only ever one at a time.
    if (m_training.isRunning()) {
        emit log(QStringLiteral("Training already running"), false);
        return;
    }
    const QList<trading::TrainingExample> examples = readExperience();
    emit log(QStringLiteral("Training the outcome model on %1 recorded trades…")
                 .arg(examples.size()),
             false);
    m_training.setFuture(QtConcurrent::run([examples]() {
        return trading::trainBotNet(examples);
    }));
}

void BotSimRunner::onTrainingDone()
{
    const trading::TrainResult result = m_training.result();
    if (!result.ok) {
        emit log(QStringLiteral("No model yet: %1").arg(result.message), false);
        emit changed();
        return;
    }
    QSaveFile file(modelPath());
    if (file.open(QIODevice::WriteOnly)) {
        static_cast<void>(
            file.write(QJsonDocument(trading::botNetToJson(result.net)).toJson()));
        static_cast<void>(file.commit());
    }
    m_net = result.net;
    emit log(QStringLiteral("Outcome model updated: %1 (%2)")
                 .arg(result.message, trading::botNetSummary(m_net, m_netMode,
                                                             trading::NetGateConfig{})),
             false);
    emit changed();
}

void BotSimRunner::loadModel()
{
    // Read once at start-up: a model file is produced offline by
    // tools/train_bot_net.py, so re-reading it per scan would buy nothing.
    QFile file(modelPath());
    if (!file.open(QIODevice::ReadOnly)) {
        m_net = trading::botNetFromJson({});
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    m_net = trading::botNetFromJson(doc.object());
}

bool BotSimRunner::aiProposalsFresh() const
{
    // The answer's own age, measured from when it was ASKED — the same bound the
    // entry side applies. An overtaken answer is fine (a CPU model is regularly
    // overtaken); reasoning older than a scan cycle is not.
    if (m_proposals.isEmpty() || !m_askedAt.isValid()) {
        return false;
    }
    return m_askedAt.msecsTo(QDateTime::currentDateTime()) <= kProposalMaxAgeMs;
}

void BotSimRunner::setCrowdEvidence(const QString &instrument, const QString &line)
{
    if (line.isEmpty()) {
        m_crowdEvidence.remove(instrument);   // absent evidence is NO line, never a stale one
    } else {
        m_crowdEvidence.insert(instrument, line);
    }
}

// The crowd lines as one prompt block (REQ-F-046). Evidence only: the model reads it like the
// technical lines; no refusal code, no sizing rule and no exit rule consults it.
QString BotSimRunner::crowdEvidenceBlock() const
{
    if (m_crowdEvidence.isEmpty()) {
        return {};
    }
    QStringList lines;
    for (auto it = m_crowdEvidence.constBegin(); it != m_crowdEvidence.constEnd(); ++it) {
        lines.append(it.value());
    }
    lines.sort();   // deterministic prompt order, independent of hash order
    return QLatin1Char('\n') + lines.join(QLatin1Char('\n'));
}

QString BotSimRunner::holdEvidence() const
{
    // The model is asked about what is ALREADY open, not just what to open next
    // (REQ-F-032) — a bot that can only be talked into trades and never out of them
    // holds its mistakes to the stop.
    QList<trading::OpenPositionBrief> open;
    const QDateTime now = QDateTime::currentDateTime();
    for (const PaperTrade &t : m_book.openTrades()) {
        trading::OpenPositionBrief brief;
        brief.symbol = t.symbol;
        brief.isBuy = t.isBuy;
        brief.netPnl = t.netPnl();
        brief.heldHours = t.openTime.isValid()
                              ? (static_cast<double>(t.openTime.secsTo(now)) / 3600.0)
                              : 0.0;
        brief.entryConfidence = t.entryConfidence;
        open.append(brief);
    }
    return trading::paperHoldEvidence(open);
}

void BotSimRunner::requestProposal()
{
    if ((m_ai == nullptr) || m_evidence.isEmpty()) {
        return;
    }
    if (m_ai->busy()) {
        // The previous answer is still being generated. Skipping is the honest
        // outcome: a queue of stale prompts would trade on old evidence.
        emit log(QStringLiteral("Local model still busy — this scan's proposal is skipped"), false);
        return;
    }
    m_aiPending = true;
    m_askedAt = QDateTime::currentDateTime();
    emit log(QStringLiteral("Asking the local model (%1) for a proposal…").arg(m_ai->model()),
             false);
    m_ai->requestDecision(m_evidence);
}

void BotSimRunner::onProposals(const QList<AiDecision> &picks, const QString &error)
{
    m_aiPending = false;
    const QString source = (m_ai != nullptr) ? QStringLiteral("ollama / %1").arg(m_ai->model())
                                             : QStringLiteral("ai");
    // Map each pick's spelling onto a tradable instrument (models answer in prose as
    // happily as in symbols); an unresolvable pick is kept and refused later with
    // that reason, rather than silently dropped.
    //
    // Resolved against the whole CATALOG, not just this scan's decision rows. "Tradable
    // here" means "an instrument this app lists", not "one that happened to be scored this
    // cycle" — otherwise a crypto pick (BTCUSDT -> BTC) was annotated "not tradable here"
    // whenever crypto had no row that scan, which is exactly the confusing output reported.
    // Whether it is opened is still decided later by the focus, market and risk gates.
    const QStringList known = trading::tradableSymbols();
    m_proposals.clear();
    for (const AiDecision &pick : picks) {
        m_proposals.append(normalizePick(pick, source, known));
    }

    if (m_proposals.isEmpty()) {
        emit log(QStringLiteral("Local model gave no usable proposal: %1").arg(error), true);
    } else {
        emit log(QStringLiteral("PROPOSAL %1: %2 pick(s) — %3. Rationale: %4")
                     .arg(source)
                     .arg(m_proposals.size())
                     .arg(describePicks(m_proposals),
                          m_proposals.constFirst().rationale.isEmpty()
                              ? QStringLiteral("none given")
                              : m_proposals.constFirst().rationale),
                 false);
    }

    emit proposalsUpdated(m_proposals);

    // Apply it to the NEWEST scan's candidates as long as the reasoning is still
    // fresh; the entry gate re-checks the live quote, spread and market state for
    // whichever instrument it names, so newer rows are an improvement, not a risk.
    const qint64 ageMs = m_askedAt.isValid() ? m_askedAt.msecsTo(QDateTime::currentDateTime()) : 0;
    if (!m_armed) {
        // nothing to do: disarmed books only get marked
    } else if (ageMs > kProposalMaxAgeMs) {
        emit log(QStringLiteral("Proposal took %1 s — older than one scan cycle, so it is "
                                "dropped rather than traded on stale reasoning")
                     .arg(ageMs / 1000),
                 false);
        m_proposals.clear();  // …and they must not linger as "the" proposals
    } else {
        considerEntriesForScan();
    }
    emit changed();
}

void BotSimRunner::considerEntriesForScan()
{
    considerEntries(m_pendingRows, m_pendingScan);
}

trading::AiGate BotSimRunner::gateFor(const trading::DecisionRow &row) const
{
    return trading::paperAiGate(row.symbol, row.dir, m_proposals, m_book.config().aiMode,
                                aiSourceState());
}

// What is known about the model's answer, so a refusal can name the CAUSE rather than the
// symptom. Assembled here because this is where the answer and its timing actually live.
// The spread the cost model should use, with the crypto floor applied. All spread reads in
// the runner go through here so eToro's ~1% crypto cost is charged consistently — see
// trading::minSpreadPctFor. A wider real spread still wins (it is a floor).
double BotSimRunner::effectiveSpreadPct(const QString &symbol) const
{
    const double raw = (m_client != nullptr) ? m_client->spreadPctFor(symbol) : 0.0;
    return std::max(raw, trading::minSpreadPctFor(symbol));
}

trading::AiSource BotSimRunner::aiSourceState() const
{
    trading::AiSource source;
    // Asked of the advisor itself, not of a config field: the model name can also come
    // from OLLAMA_MODEL, and the advisor is the one object that knows which won.
    source.configured = (m_ai != nullptr) && m_ai->isConfigured();
    source.asked = m_askedAt.isValid();
    source.received = m_proposals.size();
    source.usable = std::count_if(m_proposals.cbegin(), m_proposals.cend(),
                                  [](const trading::AiProposal &p) { return p.ok; });
    source.ageMs = m_askedAt.isValid() ? m_askedAt.msecsTo(QDateTime::currentDateTime()) : -1;
    source.maxAgeMs = kProposalMaxAgeMs;
    source.leadFallback = m_book.config().aiLeadFallbackToComposite;
    return source;
}

// The swing strategy's own entry path (2026-08-12 redesign, item 5's live wiring):
// evaluated per scan, entirely separate from the composite/AI gate in tryOpen below.
// Kept separate rather than folded into considerEntries/tryOpen because the two paths
// disagree about almost everything that matters: this one is daily-bar-based (not the
// hourly closes DecisionRow carries), sizes by the EXPLICIT risk-per-trade model rather
// than paperEntryVerdict's stakeFraction target, and skips the AI gate entirely — the
// swing strategy IS the decision, not a candidate for the model to confirm or override.
// It still shares the SAME book, so it caps its own stake against paperStakeCeiling —
// the exact function paperStakeRoom itself is built from — rather than a second copy of
// the portfolio/margin/correlation arithmetic that could drift from it.
// One symbol's own swing-entry attempt, split out of considerSwingEntries purely to
// keep that a loop (and both stay inside the complexity budget) — mirroring tryOpen's
// own split out of considerEntries above.
void BotSimRunner::trySwingOpen(const QString &symbol, const QDateTime &now)
{
    const trading::BotConfig &cfg = m_book.config();
    QString preWhy;
    if (!preTradeRefusal(symbol, &preWhy).isEmpty()) {
        return;   // the SAME focus/market obstacles the composite path refuses on
    }
    if (m_book.exposureFor(symbol).count > 0) {
        return;   // one swing position per symbol at a time, matching the backtester
    }
    const QList<trading::DailyBar> bars = m_dailyBars.value(symbol);
    if (bars.size() < swingConfig().slowEmaPeriod) {
        return;   // not enough history for the longest EMA the strategy uses
    }
    trading::StrategySnapshot snap;
    snap.symbol = symbol;
    snap.bars = bars;
    snap.eventRiskImminent = m_eventRisk;
    snap.termStructure = trading::termStructure(m_referenceSeries);
    const trading::StrategyDecision decision = swingStrategy().evaluate(snap);
    if (!decision.enter) {
        return;
    }
    const double fillPrice = bars.constLast().close;
    if ((fillPrice <= 0.0) || (decision.stopFraction <= 0.0)) {
        return;
    }
    const double stopPrice = decision.isBuy ? (fillPrice * (1.0 - decision.stopFraction))
                                             : (fillPrice * (1.0 + decision.stopFraction));
    const double riskPerTrade = trading::riskPerTradeFor(cfg, symbol);
    const trading::ExplicitRiskSizing sizing = trading::sizeByExplicitRisk(
        m_book.state().equity, riskPerTrade, decision.stopFraction, cfg.swingLeverage);
    if (!sizing.valid) {
        return;
    }
    const double riskPerStake = (sizing.margin > 0.0) ? (sizing.riskAmount / sizing.margin) : 0.0;
    const trading::StakeRoom ceiling =
        trading::paperStakeCeiling(m_book.state(), cfg, riskPerStake, symbol);
    const double margin = std::min(sizing.margin, ceiling.stake);
    if (margin < cfg.minStake) {
        emit entryDecision(symbol, false,
                           ceiling.limit.isEmpty() ? QStringLiteral("sizing-refused")
                                                   : ceiling.limit,
                           QStringLiteral("swing entry sized below the minimum stake"));
        return;
    }
    const qint64 instrumentId = m_client->instrumentIdFor(symbol);
    trading::EntrySignal sig;
    sig.valid = true;
    sig.symbol = symbol;
    sig.instrumentId = instrumentId;
    sig.isBuy = decision.isBuy;
    sig.fillRate = fillPrice;
    sig.spreadPct = effectiveSpreadPct(symbol);
    sig.leverage = cfg.swingLeverage;
    sig.slRate = stopPrice;
    sig.basis = QStringLiteral("swing %1: %2").arg(swingStrategy().version(), decision.why);
    const qint64 openedId = (instrumentId == 0) ? 0 : m_book.open(sig, margin, now);
    if (openedId == 0) {
        emit entryDecision(symbol, false, QStringLiteral("instrument-unresolved"), decision.why);
        return;
    }
    m_book.setStrategyVersion(openedId, swingStrategy().version());
    m_book.setSwingInitialStop(openedId, stopPrice);
    m_book.setSwingState(openedId, stopPrice, false, 0);
    // The input half of the example this position becomes when it closes (REQ-F-033);
    // a record without it is dropped by recordExperience, partial and final alike.
    m_book.setFeatures(openedId, swingFeaturesFor(sig, bars, margin, now));
    m_swingLastEvalDate.insert(openedId, now.date());
    announceOpened(openedId, symbol);
    emit entryDecision(symbol, true, QStringLiteral("opened"), decision.why);
    emit log(QStringLiteral("SWING OPEN %1 %2 %3 @ %4 x%5 — SL %6 — %7")
                 .arg(symbol, decision.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                      botPlain(margin), botRate(fillPrice))
                 .arg(cfg.swingLeverage)
                 .arg(botRate(stopPrice))
                 .arg(decision.why),
             false);
}

void BotSimRunner::considerSwingEntries(const QDateTime &now)
{
    const trading::BotConfig &cfg = m_book.config();
    if (!cfg.useSwingStrategy || (m_client == nullptr)) {
        return;
    }
    for (const QString &symbol : cfg.swingStrategySymbols) {
        trySwingOpen(symbol, now);
    }
}

// The swing strategy's own exit path for ONE position it manages (2026-08-12 redesign,
// item 6). The stop-barrier check runs every mark tick, exactly like every other open
// trade's — barrierHit is the SAME function paperCloseDecision itself calls, shared
// rather than duplicated. The day-granular rules (time-stop, the 2R partial, the
// trailing stop) run at most once per calendar day, matching swingExitDecision's own
// day-by-day state; a 5-second mark tick must not race ahead of the bar the strategy
// is actually reasoning over. NEVER routed through paperCloseDecision itself: that
// bundle's SignalFade/GiveBack are tuned for the composite's own conviction signal,
// which this strategy was never scored against, and folding them in would contaminate
// a winning trade riding to 2R with an exit rule that was never meant to judge it.
bool BotSimRunner::applySwingExit(const trading::PaperTrade &trade, double markRate,
                                  const QDateTime &now)
{
    if (markRate > 0.0) {
        if (const CloseReason hit = trading::barrierHit(trade, markRate); hit != CloseReason::None) {
            closeTrade(trade, hit);
            return true;
        }
    }
    const QDate todayDate = now.date();
    if (!todayDate.isValid() || (m_swingLastEvalDate.value(trade.id) == todayDate)) {
        return false;   // already ran today's day-granular rules for this position
    }
    const QList<trading::DailyBar> &bars = m_dailyBars.value(trade.symbol);
    if (bars.size() < 6) {
        return false;   // not enough bars yet for the 5-day-low/EMA10 inputs
    }
    m_swingLastEvalDate.insert(trade.id, todayDate);

    trading::SwingPositionState state;
    state.partialTaken = trade.swingPartialTaken;
    state.stopPrice = trade.slRate;
    state.sessionsHeld = trade.swingSessionsHeld;

    QList<double> closes;
    closes.reserve(bars.size());
    for (const trading::DailyBar &bar : bars) {
        closes.append(bar.close);
    }
    const qsizetype lowStart = std::max<qsizetype>(0, bars.size() - 5);
    double fiveDayLow = bars.at(lowStart).low;
    for (qsizetype i = lowStart; i < bars.size(); ++i) {
        fiveDayLow = std::min(fiveDayLow, bars.at(i).low);
    }
    const QList<double> ema10Series = trading::emaSeries(closes, 10);

    trading::SwingExitInputs in;
    in.entryPrice = trade.openRate;
    in.initialStopPrice = trade.swingInitialStopRate;
    in.todayClose = bars.constLast().close;
    in.fiveDayLow = fiveDayLow;
    in.ema10 = ema10Series.isEmpty() ? 0.0 : ema10Series.constLast();

    const trading::SwingExitAction action = trading::swingExitDecision(state, in, swingConfig());
    if (action.fullClose) {
        // Same convention StrategyBacktest uses for this exact rule, so a live run and a
        // backtest over the same bars attribute the close to the same reason.
        closeTrade(trade, CloseReason::AiExit);
        return true;
    }
    if (action.partialClose) {
        const double exitRate = (markRate > 0.0) ? markRate : bars.constLast().close;
        const trading::PaperBook::ExitPricing pricing{exitRate, effectiveSpreadPct(trade.symbol),
                                                       CloseReason::TakeProfit, now};
        // The partial's net goes to the experience log's ledger for this id — never
        // as an example of its own (see recordExperience).
        recordExperience(m_book.partialClose(trade.id, action.partialFraction, pricing));
        m_book.setSwingState(trade.id, action.nextState.stopPrice, action.nextState.partialTaken,
                            action.nextState.sessionsHeld);
        emit log(QStringLiteral("SWING PARTIAL %1: closed %2% — %3")
                     .arg(trade.symbol)
                     .arg(action.partialFraction * 100.0, 0, 'f', 0)
                     .arg(action.why),
                 false);
        return false;   // the remainder stays open under the same id
    }
    m_book.setSwingState(trade.id, action.nextState.stopPrice, action.nextState.partialTaken,
                        action.nextState.sessionsHeld);
    return false;
}

void BotSimRunner::considerEntries(const QList<trading::DecisionRow> &rows,
                                  const QList<ScreenerRow> &scan)
{
    if (m_client == nullptr) {
        return;
    }
    QHash<QString, ScreenerRow> bySymbol;
    for (const ScreenerRow &r : scan) {
        static_cast<void>(bySymbol.insert(r.symbol, r));
    }
    const QDateTime now = QDateTime::currentDateTime();
    qint32 openedCount = 0;
    QMap<QString, qint32> skips;  // sorted, so the summary line is stable
    // The rows arrive sorted by confidence, so the boldest calls get the capital
    // first — and the bot keeps taking them until the RISK budget, not a count,
    // says stop.
    for (const trading::DecisionRow &row : rows) {
        QString code;
        if (tryOpen(row, bySymbol.value(row.symbol).closes, now, &code)) {
            ++openedCount;
        } else if (!code.isEmpty()) {
            skips[code] += 1;
        }
    }
    if (openedCount > 0) {
        syncQuoteInterest();
        save();
    }
    // ONE line per scan, always — silence is indistinguishable from a broken bot,
    // and "26 candidates, 0 opened, 21x market-closed" answers the question the
    // window otherwise leaves open.
    QStringList parts;
    for (auto it = skips.cbegin(); it != skips.cend(); ++it) {
        parts << QStringLiteral("%1x %2").arg(it.value()).arg(it.key());
    }
    const trading::BookState st = m_book.state();
    // Where the risk actually SITS, biggest bucket first: "risk at stop 5665 of 9973"
    // reads like diversification even when every euro of it is one long index bet.
    QStringList buckets;
    QList<QPair<double, QString>> byRisk;
    for (auto it = st.riskByGroup.cbegin(); it != st.riskByGroup.cend(); ++it) {
        byRisk.append({it.value(), it.key()});
    }
    std::sort(byRisk.begin(), byRisk.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    const double groupCap = st.equity * m_book.config().maxGroupRiskFraction;
    for (const auto &[risk, name] : byRisk) {
        buckets << QStringLiteral("%1 %2/%3").arg(name, botPlain(risk)).arg(groupCap, 0, 'f', 0);
    }
    // Both limits, because either can be the one that stopped the bot and the log
    // must not leave the reader guessing which: risk at stop, and margin committed.
    emit log(QStringLiteral("SCAN: %1 candidates, %2 opened, %3 open in total — risk at stop %4 of "
                            "%5 EUR allowed · margin %6 of %7 EUR · cash %8%9")
                 .arg(rows.size())
                 .arg(openedCount)
                 .arg(st.openCount)
                 .arg(botPlain(st.openRisk))
                 .arg(st.equity * m_book.config().maxPortfolioRiskFraction, 0, 'f', 2)
                 .arg(botPlain(st.invested))
                 .arg(st.equity * m_book.config().maxExposureFraction, 0, 'f', 2)
                 .arg(botPlain(st.cash),
                      (buckets.isEmpty() ? QString()
                                         : QStringLiteral(" · by view: %1").arg(buckets.join(u", ")))
                          + (parts.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" · skipped: %1").arg(parts.join(u", ")))),
             false);
    reportForecast(rows);
}

void BotSimRunner::reportForecast(const QList<trading::DecisionRow> &rows)
{
    // What the RECORD says about calls like the ones just made (REQ-F-037), for the
    // leading candidate: the probability per horizon, or an explicit refusal to quote one.
    // Logged once per scan rather than per candidate — one line a reader will actually
    // read beats twenty-six they will not.
    if (rows.isEmpty()) {
        return;
    }
    const trading::DecisionRow &lead = rows.constFirst();
    const QList<double> closes = m_symbolSeries.value(lead.symbol);
    // This instrument's rows of THIS strategy version only (see kCompositeStrategyVersion):
    // a row from before the switch was priced off another series and carries a five-HOUR
    // baseline, so scoring it here would pair an old row against a new row's price from a
    // different feed and count its baseline in the five-minute one's hit rate. The filter
    // covers both consumers below — the pairing walks only what it is handed.
    QList<trading::Prediction> forSymbol;
    for (const trading::Prediction &row : ledgerRows()) {
        if ((row.symbol == lead.symbol)
            && (row.strategyVersion == QLatin1String(kCompositeStrategyVersion))) {
            forSymbol.append(row);
        }
    }
    trading::Prediction now;
    now.at = QDateTime::currentDateTimeUtc();
    now.symbol = lead.symbol;
    now.dir = lead.dir;
    now.strength = m_lastLeadStrength.value(lead.symbol, 0.0);
    now.price = closes.isEmpty() ? 0.0 : closes.constLast();
    if (!now.isValid()) {
        return;   // nothing to forecast from, and a forecast from nothing is the lie
    }
    QStringList lines;
    for (const trading::HorizonProbability &answer :
         trading::horizonProbabilities(now, forSymbol, closes)) {
        lines << answer.sentence;
    }
    // The record's own verdict on itself, at the horizon a CFD can actually be traded on:
    // whether the app's calls have beaten the cheap baselines yet. It is allowed to say no.
    const trading::HorizonScore score = trading::scoreHorizon(forSymbol, trading::Horizon::M15);
    emit log(QStringLiteral("FORECAST %1: %2 · record: %3")
                 .arg(lead.symbol, lines.join(QStringLiteral(" · ")), score.headline()),
             false);
}

const QList<trading::Prediction> &BotSimRunner::ledgerRows()
{
    if (!m_ledgerLoaded) {
        m_ledger = trading::loadPredictions(ledgerPath());
        m_ledgerLoaded = true;
    }
    return m_ledger;
}

// One ledger row per evaluated candidate, whatever the outcome (REQ-F-037). The rows
// that STAY OUT are the reason the ledger is worth keeping: a record of the trades
// actually taken can only measure the gate in front of it, never the signal — it cannot
// see the calls that were right and skipped.
//
// Its own function rather than a lambda inside tryOpen: recording what was decided is a
// different job from deciding, and folding it in pushed tryOpen past the complexity gate.
void BotSimRunner::recordPrediction(const trading::DecisionRow &row, const QList<double> &closes,
                                    const QDateTime &now, const trading::CandidateInput &in,
                                    const QString &refusal)
{
    // The instrument's own 1-minute series, when LIVE (liveSessionSeries); `closes` are the
    // eToro scan's HOURLY candles, kept for the regime (persistence needs a session) and as
    // the price of last resort. Looked up here rather than passed, so the signature stays
    // within the parameter limit.
    const QList<double> session = liveSessionSeries(row.symbol);
    trading::Prediction entry;
    entry.at = now.toUTC();
    entry.symbol = row.symbol;
    entry.dir = in.leadDir;
    entry.strength = in.leadStrength;
    entry.measured = in.leadMeasured;
    entry.unknowns = in.leadUnknowns;
    entry.strategyVersion = QLatin1String(kCompositeStrategyVersion);
    // The mark the call was made at, by the SAME rule the simulated fill is priced by
    // (sidesFor: the per-tick quote, else the venue's bulk mid, else — crypto — its 1-minute
    // close): an outcome is resolved against a LATER row's price, so every row of one
    // instrument must be priced off one feed, and it must be a feed that MOVES whenever the
    // instrument is called. The 1-minute series is not that for the two indices — theirs is
    // the CASH index (^GSPC / ^NDX), which Yahoo freezes at the New York close while the CFD
    // keeps being scanned for hours; rows priced off it paired into a "no move" outcome and a
    // miss for every directional call of the evening. The hourly last close stands in only
    // when nothing prices the candidate (the row is then dropped unless it has a price).
    const qint64 id = (m_client != nullptr) ? m_client->instrumentIdFor(row.symbol) : 0;
    const Sides sides = sidesFor(row.symbol, id, closes);
    if (sides.ok) {
        entry.price = (sides.bid + sides.ask) / 2.0;
    } else {
        entry.price = closes.isEmpty() ? 0.0 : closes.constLast();
    }
    trading::RegimeInputs regime;
    // Persistence needs a session to measure; below that it stays UNKNOWN rather than
    // defaulting to a comfortable "range".
    if (closes.size() >= 32) {
        regime.hurstKnown = true;
        regime.hurst = trading::hurstExponent(closes);
    }
    regime.vixValid = m_vixValid;
    regime.vix = m_vix;
    regime.eventWindow = m_eventRisk;
    entry.regime = trading::regimeFor(regime);
    // No refusal code IS the taken case — one parameter fewer, and the two can no longer
    // be set inconsistently (a "taken" row carrying a refusal was expressible before).
    entry.taken = refusal.isEmpty();
    entry.refusal = refusal;
    // The baseline this app CAN measure at decision time: the previous five minutes'
    // direction — the last six points of the 1-MINUTE series, never of the hourly scan
    // closes, whose last six points span five hours and are a different rival for the
    // signal than the one the ledger names. Fewer than six points, or a series that is not
    // live (frozen at the cash close, or a sweep that failed and left the last good one in
    // place — the sign of its last five minutes is then a stale rival, not this call's),
    // leave it 0: a baseline that could not be measured is not scored (the ledger treats 0
    // as "no side"). The session-VWAP side stays 0 — unknown — because the instrument's
    // own candles carry no volume, and an unmeasurable baseline must not be scored.
    if (session.size() > 5) {
        const double then = session.at(session.size() - 6);
        const double last = session.constLast();
        entry.priorMoveDir = (last > then) ? 1 : ((last < then) ? -1 : 0);
    }
    // The file first, the cache only when the file took the row: the in-memory ledger is
    // a copy of what is on disk, and a row the disk refused must not be scored as if it
    // had been recorded. Loaded before the append so the copy never misses what the
    // file already held.
    static_cast<void>(ledgerRows());
    if (trading::appendPrediction(ledgerPath(), entry)) {
        m_ledger.append(entry);
    }
}

// The two obstacles that outrank everything, in order, so a refusal names one a reader can
// act on. FOCUS first: an instrument outside the configured universe is never traded,
// whatever the model or the market says — this is what removes the OIL.24-7 / USDOLLAR losses
// the ledger showed. MARKET next: on a Saturday the cash market is shut, and blaming that on
// the model (as the old ordering did, refusing every closed instrument as "no AI proposal")
// sends the reader to fix the wrong thing. Empty return = neither applies.
QString BotSimRunner::preTradeRefusal(const QString &symbol, QString *why) const
{
    const QStringList &focus = m_book.config().focusSymbols;
    if (!focus.isEmpty() && !focus.contains(symbol)) {
        *why = QStringLiteral("outside the bot's focus set (%1)").arg(focus.join(u", "));
        return QStringLiteral("not-focus");
    }
    // The venue's tradeable set covers only the instruments it lists with a session — a
    // 24/7 instrument (crypto) is never in it, so the membership test alone read every
    // coin as `market-closed` the moment the first tradeability poll answered, one gate
    // before candidateFor's own 24/7 exemption could ever run. Its market has no
    // closing hours; whether it can be PRICED is still candidateFor's `sides.ok`.
    if (m_tradeabilityKnown && !m_tradeable.contains(symbol) && !trading::tradesOnWeekend(symbol)) {
        *why = QStringLiteral("market closed for this instrument — the broker does not list it "
                              "as tradable right now");
        return QStringLiteral("market-closed");
    }
    return {};
}

bool BotSimRunner::tryOpen(const trading::DecisionRow &row, const QList<double> &closes,
                           const QDateTime &now, QString *skipCode)
{
    const auto skip = [skipCode](const QString &code) {
        if (skipCode != nullptr) {
            *skipCode = code;
        }
        return false;
    };
    // Every refusal goes through here, so a path cannot record a prediction while
    // forgetting to say WHY it refused (REQ-F-029). The ledger row is for measuring the
    // decision later; the decision-log line is for a person asking what happened.
    const auto refuse = [&](const QString &code, const QString &why,
                            const trading::CandidateInput &input) {
        recordPrediction(row, closes, now, input, code);
        trading::DecisionNote note =
            decisionNoteFor(row.symbol, input.dir, row.confidence, input, now);
        note.traded = false;
        note.code = code;
        note.why = why;
        static_cast<void>(trading::appendDecision(decisionLogPath(), note));
        emit entryDecision(row.symbol, false, code, why);
        return skip(code);
    };
    trading::BookState state = m_book.state();
    state.symbol = m_book.exposureFor(row.symbol);

    // The two most fundamental obstacles first — focus and market — before the model or any
    // risk rule is consulted. Extracted so tryOpen stays within the metrics ratchet; the
    // ORDER is the point (see preTradeRefusal): a refusal must name the obstacle a reader can
    // actually act on, and no model configuration opens a shut market or trades an instrument
    // the bot is not allowed to hold.
    QString preWhy;
    const QString preCode = preTradeRefusal(row.symbol, &preWhy);
    if (!preCode.isEmpty()) {
        return refuse(preCode, preWhy, trading::CandidateInput{});
    }

    // Then the AI gate (REQ-F-030): in Off it just passes the composite's direction
    // through, in Confirm it vetoes, in Lead it supplies the side.
    const trading::AiGate gate = gateFor(row);
    if (!gate.allow) {
        // Only an instrument the model actually named earns its own line: "not
        // among the AI's picks" repeated for every other row is noise, and the
        // scan summary counts those anyway.
        const bool aboutTheAiPick = (gate.pick >= 0);
        if (aboutTheAiPick && !gate.why.isEmpty()) {
            emit log(QStringLiteral("SIM SKIP %1: %2").arg(row.symbol, gate.why), false);
        }
        // Nothing was evaluated yet, so the row carries no evidence — only the refusal.
        return refuse(gate.code, gate.why, trading::CandidateInput{});
    }
    const qint64 id = m_client->instrumentIdFor(row.symbol);
    const trading::AiProposal pick =
        (gate.pick >= 0) ? m_proposals.at(gate.pick) : trading::AiProposal{};
    const trading::CandidateInput in = candidateFor(row, gate, closes, now);
    // Kept for the scan's forecast line, so it quotes the SAME strength the ledger
    // recorded for this instrument rather than recomputing it from newer inputs.
    static_cast<void>(m_lastLeadStrength.insert(row.symbol, in.leadStrength));

    trading::EntrySignal sig = trading::buildEntrySignal(in, m_book.config());
    // A model leading the trade may ask for LESS leverage than the risk budget
    // allows, and that request is honoured; it can never ask for more.
    sig.leverage = trading::paperLeverageWithAi(sig.leverage, pick.leverage,
                                               m_book.config().aiMode);
    // Name the brain behind the trade in its own record: with an AI mode active
    // the direction (Lead) or the veto (Confirm) came from the model, and the
    // books should say so — the pure geometry line cannot know.
    if (m_book.config().aiMode != trading::BotAiMode::Off) {
        sig.basis += QStringLiteral(" [AI %1: %2]")
                         .arg(trading::botAiModeWord(m_book.config().aiMode), pick.source);
    }
    const trading::EntryVerdict verdict =
        trading::paperEntryVerdict(in, sig, state, m_book.config());
    if (!verdict.take) {
        // Per instrument this is noise in the WINDOW; COUNTED over the scan it is the
        // answer to "why did the bot open nothing?", which considerEntries logs once. In
        // the ledger it is a full row, and in the decision log a sentence a person can
        // read months later: the evidence measured, and the rule that refused it.
        return refuse(verdict.code, verdict.why, in);
    }
    // Last: what the bot has LEARNED from its own record about setups like this one
    // (REQ-F-033). It rides along until the model has earned the right to refuse.
    const trading::EntryFeatures features = featuresFor(in, sig, verdict.stake, now);
    // Named netGate, not net: BotSimRunner::net() is the model accessor, and a local
    // that shadows a member function reads as if the gate WERE the model.
    if (const trading::NetVerdict netGate = applyNetGate(row.symbol, features, sig);
        !netGate.allow) {
        return refuse(netGate.code, netGate.why, in);
    }
    // An instrument without a venue id cannot be opened — EXCEPT crypto, which is priced and
    // marked off its candle close by design (sidesFor / markFor), so the simulation's whole
    // geometry — fill, stop, target, marks, exits — never needs the id; it reached this line
    // priced, or the gate would have refused it `no-live-quote`. A real order path (which does
    // not exist here, REQ-N-005) would need the id, and that is where this exemption ends.
    // Everything else keeps the refusal: the sidesFor/markFor candle fallbacks were written
    // for crypto precisely because its id never resolves in this build, and refusing it here
    // only moved the refusal one step along.
    if ((id == 0) && !trading::isCryptoSymbol(row.symbol)) {
        return refuse(QStringLiteral("instrument-unresolved"),
                      QStringLiteral("the venue's instrument id for %1 is not known, so no "
                                     "order geometry could be attached to it")
                          .arg(row.symbol),
                      in);
    }
    const qint64 openedId = m_book.open(sig, verdict.stake, now);
    if (openedId == 0) {
        // Unreachable after a taken verdict (it guarantees a valid signal and a positive
        // stake), kept so a book that declines is named rather than reported as opened.
        return refuse(QStringLiteral("book-refused"),
                      QStringLiteral("the book declined to open %1: no valid geometry or stake")
                          .arg(row.symbol),
                      in);
    }
    recordPrediction(row, closes, now, in, QString{});
    m_book.setFeatures(openedId, features);
    // The COMPOSITE's conviction, whatever decided the trade: the fade rule compares
    // like with like (REQ-F-032).
    m_book.setEntryCompositeConf(openedId, row.confidence);
    announceOpened(openedId, sig.symbol);
    emit entryDecision(sig.symbol, true, QStringLiteral("opened"), verdict.why);
    appendOpenedNote(sig, in, verdict, row.confidence, now);
    emit log(QStringLiteral("SIM OPEN %1 %2 %3 @ %4 x%5 — SL %6 / TP %7, spread cost %8 — %9")
                 .arg(sig.symbol)
                 .arg(sig.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                 .arg(botPlain(verdict.stake))
                 .arg(botRate(sig.fillRate))
                 .arg(sig.leverage)
                 .arg(botRate(sig.slRate))
                 .arg(botRate(sig.tpRate))
                 .arg(botPlain(trading::paperHalfSpreadCost(verdict.stake, sig.leverage,
                                                         sig.spreadPct)))
                 .arg(verdict.why),
             false);
    return true;
}

void BotSimRunner::appendOpenedNote(const trading::EntrySignal &sig,
                                    const trading::CandidateInput &in,
                                    const trading::EntryVerdict &verdict,
                                    double compositeConfidence, const QDateTime &now) const
{
    // The same event as the SIM OPEN line, in the persistent log with the geometry beside
    // the evidence, so the file answers "why WAS this traded?" as fully as it answers why
    // the others were not.
    trading::DecisionNote note =
        decisionNoteFor(sig.symbol, sig.isBuy ? 1 : -1, compositeConfidence, in, now);
    note.traded = true;
    note.why = verdict.why;
    note.stake = verdict.stake;
    note.leverage = sig.leverage;
    note.fillRate = sig.fillRate;
    note.slRate = sig.slRate;
    note.tpRate = sig.tpRate;
    note.openCost = trading::paperHalfSpreadCost(verdict.stake, sig.leverage, sig.spreadPct);
    static_cast<void>(trading::appendDecision(decisionLogPath(), note));
}

void BotSimRunner::syncQuoteInterest()
{
    if (m_client == nullptr) {
        return;
    }
    QSet<qint64> ids;
    for (const PaperTrade &t : m_book.openTrades()) {
        if (t.instrumentId != 0) {
            static_cast<void>(ids.insert(t.instrumentId));
        }
    }
    m_client->setExtraQuoteInstruments(ids);
}

void BotSimRunner::resetBooks()
{
    const QDateTime now = QDateTime::currentDateTime();
    // Close what is open at its current mark first, so the discarded history at
    // least ends consistently (and a reset shows up as a reason, not as a gap).
    // Each close removes the trade, so this always takes the first remaining one.
    while (!m_book.openTrades().isEmpty()) {
        const PaperTrade trade = m_book.openTrades().constFirst();
        const Mark mark = markFor(trade);
        const double spreadPct =
            effectiveSpreadPct(trade.symbol);
        static_cast<void>(m_book.close(trade.id, mark.rate, spreadPct, CloseReason::Reset, now));
    }
    m_book.reset();
    m_armed = false;
    m_dirBySymbol.clear();
    m_confBySymbol.clear();
    // The reset closes above wrote no examples; a fresh book reuses ids, so a partial
    // banked for a discarded position must not label a new one.
    m_partialNetById.clear();
    syncQuoteInterest();
    save();
    emit log(QStringLiteral("BOT SIM reset — back to %1, no positions, no history.")
                 .arg(botPlain(m_book.config().startEquity)),
             false);
    emit changed();
}

void BotSimRunner::save() const
{
    // The ONE place the book reaches the disk, so this is where a non-owned book is kept
    // off it: aboutToQuit's save, a focus/AI-mode/daily-rules change and the mark saves
    // all end here, and a read-only viewer must leave the owner's file exactly as the
    // owner wrote it.
    if (!m_ownsBook) {
        return;
    }
    const QString path = storePath();
    static_cast<void>(QDir().mkpath(QFileInfo(path).absolutePath()));
    QSaveFile file(path);  // atomic: a crash mid-write must not truncate the books
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    // The books belong to the PaperBook; whether the experiment is RUNNING, and on
    // which decision source, is session state the runner owns — and it has to
    // survive a restart too, or a multi-day experiment silently stops the first
    // time the app is reopened and nobody notices for hours.
    QJsonObject root = m_book.toJson();
    root.insert(QStringLiteral("armed"), m_armed);
    root.insert(QStringLiteral("aiMode"), static_cast<int>(m_book.config().aiMode));
    static_cast<void>(file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)));
    static_cast<void>(file.commit());
}

void BotSimRunner::load()
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;  // first run: the configured starting capital stands
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject root = doc.object();
    if (!m_book.fromJson(root)) {
        return;
    }
    // Resume the experiment exactly as it was left, including whether it was
    // running and on which decision source.
    trading::BotConfig cfg = m_book.config();
    if (root.contains(QStringLiteral("aiMode"))) {
        cfg.aiMode = static_cast<trading::BotAiMode>(root.value(QStringLiteral("aiMode")).toInt());
        m_book.setConfig(cfg);
    }
    m_armed = root.value(QStringLiteral("armed")).toBool();
    // Provisional: acquireBookLock() rewrites this when the book turns out to be held
    // elsewhere, because "resumed" is only true of the process that runs it.
    m_restoreNote =
        restoreNoteFor(m_armed ? QStringLiteral("RESUMED ARMED, the experiment continues")
                               : QStringLiteral("DISARMED — press \"Arm the bot\" to continue"));
}

QString BotSimRunner::restoreNoteFor(const QString &state) const
{
    const PaperStats s = m_book.stats();
    return QStringLiteral(
               "BOT SIM books restored: equity %1, %2 open, %3 closed — %4 (AI mode: %5)")
        .arg(botPlain(s.equity))
        .arg(s.openTrades)
        .arg(s.closedTrades)
        .arg(state, trading::botAiModeWord(m_book.config().aiMode));
}

void BotSimRunner::acquireBookLock()
{
    const QString path = storePath();
    // QLockFile cannot create its file in a directory that does not exist yet, and on a
    // first run nothing has created the config dir before this point.
    static_cast<void>(QDir().mkpath(QFileInfo(path).absolutePath()));
    m_bookLock = std::make_unique<QLockFile>(path + QStringLiteral(".lock"));
    // The age rule is OFF (see m_bookLock): a book is held for as long as the bot runs,
    // and QLockFile's default judges a live holder's lock stale by its 30 s age alone.
    m_bookLock->setStaleLockTime(std::chrono::milliseconds::zero());
    // One attempt, no waiting: a held book is an answer, not a condition to wait out. A
    // lock whose process is gone (or that predates a reboot) is stale by QLockFile's
    // pid/boot-id rule and is taken over; a live holder's is refused, however old.
    m_ownsBook = m_bookLock->tryLock(std::chrono::milliseconds::zero());
    if (m_ownsBook) {
        return;
    }
    // load() may have restored "armed" from a book the OTHER process is running; the
    // experiment continues there, not here, so the flag is dropped rather than resumed
    // (setArmed would refuse it too — this keeps armed() honest from the first call).
    // The restore note follows the same fact: what it announced before this point was
    // the FILE's flag, and "resumed, the experiment continues" beside the read-only line
    // would say the experiment runs in two processes.
    if (!m_restoreNote.isEmpty()) {
        m_restoreNote = restoreNoteFor(
            m_armed ? QStringLiteral("saved ARMED — the holder runs it, not this process")
                    : QStringLiteral("saved DISARMED — only the holder can arm it"));
    }
    m_armed = false;
}

QString BotSimRunner::bookHolderLine() const
{
    qint64 pid = 0;
    QString host;
    QString app;
    const bool known = (m_bookLock != nullptr) && m_bookLock->getLockInfo(&pid, &host, &app);
    return QStringLiteral("BOT SIM read-only: the book %1 is held by %2 — this process shows "
                          "it but cannot be armed and never marks, trades or saves; one "
                          "process runs the bot on one book.")
        .arg(storePath(), known ? QStringLiteral("pid %1 (%2 on %3)").arg(pid).arg(app, host)
                                : QStringLiteral("another process"));
}

// ---------------------------------------------------------------------------
// BotSimDialog
// ---------------------------------------------------------------------------
