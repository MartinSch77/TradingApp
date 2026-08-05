#include "ui/BotSimPanel.h"

#include "domain/PositionMath.h"   // priceDecimals + the quote-freshness bound
#include "services/EtoroClient.h"
#include "services/OllamaAdvisor.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

using trading::CloseReason;
using trading::PaperClosedTrade;
using trading::PaperStats;
using trading::PaperTrade;

namespace {

// How often the bot re-marks its simulated positions and re-checks their exits.
// The entry side runs on the all-instruments scan instead (~5 min), which is what
// produces new decisions; there is nothing to gain from evaluating entries faster
// than the data behind them changes.
constexpr int kTickMs = 5000;
// How old a local model's proposal may be when it lands and still be acted on.
// A CPU model legitimately takes tens of seconds, and another scan may well have
// completed meanwhile — that is fine, because the entry is re-validated against
// live quotes when it opens. What is NOT fine is reasoning that predates the
// market by more than a scan cycle, so the answer is dropped past this bound.
constexpr qint64 kProposalMaxAgeMs = qint64{5} * 60 * 1000;
// Persisted books (REQ-F-029: an experiment spans days, not one session).
constexpr auto kStoreFile = "botsim.json";
// How many newly closed trades it takes before the outcome model is refitted. Low
// enough that a running experiment keeps learning, high enough that the fit is not
// repeated for a single new example (REQ-F-033).
constexpr qint64 kRetrainEvery = 25;

QString money(double value)
{
    return QStringLiteral("%1%2 EUR").arg((value > 0.0) ? QStringLiteral("+") : QString())
        .arg(value, 0, 'f', 2);
}

QString plain(double value)
{
    return QStringLiteral("%1 EUR").arg(value, 0, 'f', 2);
}

QString rate(double value)
{
    return QStringLiteral("%1").arg(value, 0, 'f', trading::priceDecimals(value));
}

// One table cell, right-aligned for numbers and coloured by sign where asked.
QTableWidgetItem *cell(const QString &text, bool numeric = false, double signValue = 0.0,
                       bool colour = false)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (numeric) {
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (colour) {
        item->setForeground((signValue >= 0.0) ? QColor(0x1B, 0x8A, 0x3A)
                                               : QColor(0xC0, 0x39, 0x2B));
    }
    return item;
}

// Index of `id` in a trade list, or -1. The list holds at most maxOpenTrades
// entries, so a linear scan is both the simplest and the fastest thing here.
qsizetype indexOfTrade(const QList<PaperTrade> &trades, qint64 id)
{
    for (qsizetype i = 0; i < trades.size(); ++i) {
        if (trades.at(i).id == id) {
            return i;
        }
    }
    return -1;
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

void configureTable(QTableWidget *table, const QStringList &headers)
{
    table->setColumnCount(static_cast<int>(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
}

} // namespace

// ---------------------------------------------------------------------------
// BotSimRunner
// ---------------------------------------------------------------------------

BotSimRunner::BotSimRunner(EtoroClient *client, OllamaAdvisor *ai, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_ai(ai)
    , m_timer(new QTimer(this))
{
    load();
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
    m_timer->start();  // marking runs always: the books must stay current even
                       // while disarmed, or a restart would report stale P/L.

    // load() ran before any consumer could connect, so its verdict is reported from
    // the event loop instead of from the constructor — otherwise the one line that
    // says whether a multi-day experiment is still running would be emitted into
    // the void.
    if (!m_restoreNote.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            emit log(m_restoreNote, false);
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
    emit changed();
}

QString BotSimRunner::storePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QLatin1String(kStoreFile));
}

void BotSimRunner::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    const PaperStats s = m_book.stats();
    if (m_armed) {
        emit log(QStringLiteral("BOT SIM ARMED — simulated money only, no order ever reaches "
                                "eToro. Equity %1, %2 open, %3 closed.")
                     .arg(plain(s.equity))
                     .arg(s.openTrades)
                     .arg(s.closedTrades),
                 false);
    } else {
        emit log(QStringLiteral("BOT SIM disarmed — no new simulated trades; the %1 open one(s) "
                                "keep being marked.")
                     .arg(s.openTrades),
                 false);
    }
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
        const qint64 age = it->ageMs(QDateTime::currentDateTimeUtc());
        mark.live = (age >= 0) && (age < trading::kQuoteStaleMs);
        if (mark.rate > 0.0) {
            return mark;
        }
    }
    // No per-tick quote yet: the last bulk snapshot's mid is better than not
    // marking at all, but it is not a live mark and the window says so.
    mark.rate = m_client->lastRateFor(trade.instrumentId);
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
        ctx.spreadPct = (m_client != nullptr) ? m_client->spreadPctFor(trade.symbol) : 0.0;
        ctx.fees = fees;
        ctx.feesKnown = fees.isValid();
        ctx.eurPerUsd = m_eurPerUsd;
        CloseReason reason = trading::paperCloseDecision(trade, ctx, m_book.config());
        if (reason == CloseReason::None) {
            reason = aiExitFor(trade);
        }
        if (reason != CloseReason::None) {
            closeTrade(trade, reason);
            moved = true;
        }
    }
    if (harvestDayTarget()) {
        moved = true;
    }
    if (moved) {
        save();
        emit changed();
    }
}

bool BotSimRunner::harvestDayTarget()
{
    // What each open position would BOOK if it closed right now: its net so far
    // minus the half-spread it still has to cross. A position whose exit cost is
    // unknown is left out rather than guessed at — the same policy the entry gate
    // and the carry rules apply.
    QList<trading::HarvestOption> options;
    for (const PaperTrade &trade : m_book.openTrades()) {
        const double spreadPct =
            (m_client != nullptr) ? m_client->spreadPctFor(trade.symbol) : 0.0;
        if (spreadPct <= 0.0) {
            continue;
        }
        options.append({trade.id,
                        trade.netPnl()
                            - trading::paperHalfSpreadCost(trade.stake, trade.leverage, spreadPct)});
    }
    const qint64 pick = trading::paperHarvestPick(options, m_book.day(), m_book.config());
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

void BotSimRunner::closeTrade(const PaperTrade &trade, CloseReason reason)
{
    const Mark mark = markFor(trade);
    const double spreadPct =
        (m_client != nullptr) ? m_client->spreadPctFor(trade.symbol) : 0.0;
    const PaperClosedTrade done = m_book.close(trade.id, mark.rate, spreadPct, reason,
                                               QDateTime::currentDateTime());
    if (done.id == 0) {
        return;
    }
    recordExperience(done);
    // Retrain on a cadence rather than on every close: the model only moves once
    // there are new trades in it, and a retrain per trade would be noise.
    if ((m_experienceCount > 0) && ((m_experienceCount % kRetrainEvery) == 0)) {
        trainFromExperience();
    }
    emit log(QStringLiteral("SIM CLOSE %1 %2 @ %3 (%4): net %5 after %6 costs, held %7 h")
                 .arg(done.symbol)
                 .arg(done.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                 .arg(rate(done.closeRate))
                 .arg(trading::closeReasonWord(done.reason))
                 .arg(money(done.netPnl))
                 .arg(plain(done.totalCost()))
                 .arg(done.heldHours(), 0, 'f', 1),
             false);
    syncQuoteInterest();
}

BotSimRunner::Sides BotSimRunner::sidesFor(const QString &symbol, qint64 instrumentId) const
{
    Sides sides;
    if (m_client == nullptr) {
        return sides;
    }
    sides.spreadPct = m_client->spreadPctFor(symbol);
    const QHash<qint64, Quote> &quotes = m_client->quotes();
    const auto it = quotes.constFind(instrumentId);
    if (it != quotes.constEnd() && it->isValid()) {
        sides.bid = it->bid;
        sides.ask = it->ask;
        sides.ok = true;
        return sides;
    }
    // Most instruments have no per-tick quote (nothing is held in them), but the
    // tradeability poll keeps a mid AND a spread warm for every resolved one — so
    // widen the mid by that spread rather than pretending the fill is at the mid.
    const double mid = m_client->lastRateFor(instrumentId);
    if ((mid <= 0.0) || (sides.spreadPct <= 0.0)) {
        return sides;
    }
    const double half = mid * (sides.spreadPct / 100.0) / 2.0;
    sides.bid = mid - half;
    sides.ask = mid + half;
    sides.ok = true;
    return sides;
}

void BotSimRunner::onDecisions(const QList<trading::DecisionRow> &rows,
                               const trading::MarketSnapshot &snap)
{
    for (const trading::DecisionRow &row : rows) {
        static_cast<void>(m_dirBySymbol.insert(row.symbol, row.dir));
        static_cast<void>(m_confBySymbol.insert(row.symbol, row.confidence));
    }
    // Exits first: a position the ranking has turned against should go before the
    // capital it frees is committed elsewhere.
    markAndExit();

    m_pendingRows = rows;
    m_pendingScan = snap.screenerRows;
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
        m_evidence = trading::buildDecisionEvidence(rows, snap) + holdEvidence();
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
    const trading::HoldVerdict hold =
        trading::paperAiHold(trade, m_proposals, m_book.config().aiMode);
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
    const Sides sides = sidesFor(row.symbol, id);
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
    in.marketOpen = !m_tradeabilityKnown || m_tradeable.contains(row.symbol);
    in.quoteLive = sides.ok && in.marketOpen;
    // What the fee table says holding it will cost — the entry prices the round
    // trip, not just the spread (REQ-F-032).
    in.fees = (m_client != nullptr) ? m_client->feesFor(row.symbol) : InstrumentFees{};
    in.feesKnown = in.fees.isValid();
    in.eurPerUsd = m_eurPerUsd;
    return in;
}

QString BotSimRunner::applyNetGate(const QString &symbol,
                                   const trading::EntryFeatures &features,
                                   trading::EntrySignal &sig)
{
    // Empty = nothing in the way. A scored-but-allowed candidate carries the number
    // into its basis line, so the record shows what the model thought of a trade it
    // did not stop.
    const trading::NetVerdict verdict =
        trading::paperNetGate(m_net, features, m_netMode, trading::NetGateConfig{});
    if (!verdict.allow) {
        emit log(QStringLiteral("SIM SKIP %1: %2").arg(symbol, verdict.why), false);
        return verdict.code;
    }
    if (verdict.scored) {
        sig.basis += QStringLiteral(" [net %1]").arg(verdict.score, 0, 'f', 2);
    }
    return {};
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

void BotSimRunner::recordExperience(const PaperClosedTrade &done)
{
    // One JSON line per closed trade, appended and never rewritten: the bot's own
    // history is the training set, and a file that is only ever appended to cannot
    // lose it to a crash mid-write (REQ-F-033).
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
    rec.insert(QStringLiteral("netPnl"), done.netPnl);
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

QString BotSimRunner::experiencePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QStringLiteral("botsim-experience.jsonl"));
}

QString BotSimRunner::modelPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QStringLiteral("botnet.json"));
}

QList<trading::TrainingExample> BotSimRunner::readExperience()
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
    QStringList known;
    known.reserve(m_pendingRows.size());
    for (const trading::DecisionRow &row : m_pendingRows) {
        known << row.symbol;
    }
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
    return trading::paperAiGate(row.symbol, row.dir, m_proposals, m_book.config().aiMode);
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
        buckets << QStringLiteral("%1 %2/%3").arg(name, plain(risk)).arg(groupCap, 0, 'f', 0);
    }
    // Both limits, because either can be the one that stopped the bot and the log
    // must not leave the reader guessing which: risk at stop, and margin committed.
    emit log(QStringLiteral("SCAN: %1 candidates, %2 opened, %3 open in total — risk at stop %4 of "
                            "%5 EUR allowed · margin %6 of %7 EUR · cash %8%9")
                 .arg(rows.size())
                 .arg(openedCount)
                 .arg(st.openCount)
                 .arg(plain(st.openRisk))
                 .arg(st.equity * m_book.config().maxPortfolioRiskFraction, 0, 'f', 2)
                 .arg(plain(st.invested))
                 .arg(st.equity * m_book.config().maxExposureFraction, 0, 'f', 2)
                 .arg(plain(st.cash),
                      (buckets.isEmpty() ? QString()
                                         : QStringLiteral(" · by view: %1").arg(buckets.join(u", ")))
                          + (parts.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" · skipped: %1").arg(parts.join(u", ")))),
             false);
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
    trading::BookState state = m_book.state();
    state.symbol = m_book.exposureFor(row.symbol);
    // The AI gate first (REQ-F-030): in Off it just passes the composite's
    // direction through, in Confirm it vetoes, in Lead it supplies the side.
    const trading::AiGate gate = gateFor(row);
    if (!gate.allow) {
        // Only an instrument the model actually named earns its own line: "not
        // among the AI's picks" repeated for every other row is noise, and the
        // scan summary counts those anyway.
        const bool aboutTheAiPick = (gate.pick >= 0);
        if (aboutTheAiPick && !gate.why.isEmpty()) {
            emit log(QStringLiteral("SIM SKIP %1: %2").arg(row.symbol, gate.why), false);
        }
        return skip(gate.code);
    }
    const qint64 id = m_client->instrumentIdFor(row.symbol);
    const trading::AiProposal pick =
        (gate.pick >= 0) ? m_proposals.at(gate.pick) : trading::AiProposal{};
    const trading::CandidateInput in = candidateFor(row, gate, closes, now);

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
        // Per instrument this is noise; COUNTED over the scan it is the answer to
        // "why did the bot open nothing?", which considerEntries logs once.
        return skip(verdict.code);
    }
    // Last: what the bot has LEARNED from its own record about setups like this one
    // (REQ-F-033). It rides along until the model has earned the right to refuse.
    const trading::EntryFeatures features = featuresFor(in, sig, verdict.stake, now);
    if (const QString netCode = applyNetGate(row.symbol, features, sig); !netCode.isEmpty()) {
        return skip(netCode);
    }
    const qint64 openedId = (id == 0) ? 0 : m_book.open(sig, verdict.stake, now);
    if (openedId == 0) {
        return skip(QStringLiteral("instrument-unresolved"));
    }
    m_book.setFeatures(openedId, features);
    emit log(QStringLiteral("SIM OPEN %1 %2 %3 @ %4 x%5 — SL %6 / TP %7, spread cost %8 — %9")
                 .arg(sig.symbol)
                 .arg(sig.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                 .arg(plain(verdict.stake))
                 .arg(rate(sig.fillRate))
                 .arg(sig.leverage)
                 .arg(rate(sig.slRate))
                 .arg(rate(sig.tpRate))
                 .arg(plain(trading::paperHalfSpreadCost(verdict.stake, sig.leverage,
                                                         sig.spreadPct)))
                 .arg(verdict.why),
             false);
    return true;
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
            (m_client != nullptr) ? m_client->spreadPctFor(trade.symbol) : 0.0;
        static_cast<void>(m_book.close(trade.id, mark.rate, spreadPct, CloseReason::Reset, now));
    }
    m_book.reset();
    m_armed = false;
    m_dirBySymbol.clear();
    m_confBySymbol.clear();
    syncQuoteInterest();
    save();
    emit log(QStringLiteral("BOT SIM reset — back to %1, no positions, no history.")
                 .arg(plain(m_book.config().startEquity)),
             false);
    emit changed();
}

void BotSimRunner::save() const
{
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
    const PaperStats s = m_book.stats();
    m_restoreNote =
        QStringLiteral("BOT SIM books restored: equity %1, %2 open, %3 closed — %4 (AI mode: %5)")
                 .arg(plain(s.equity))
                 .arg(s.openTrades)
                 .arg(s.closedTrades)
            .arg(m_armed ? QStringLiteral("RESUMED ARMED, the experiment continues")
                         : QStringLiteral("DISARMED — press \"Arm the bot\" to continue"))
            .arg(trading::botAiModeWord(cfg.aiMode));
}

// ---------------------------------------------------------------------------
// BotSimDialog
// ---------------------------------------------------------------------------

BotSimDialog::BotSimDialog(BotSimRunner *runner, QWidget *parent)
    : QDialog(parent)
    , m_runner(runner)
{
    setWindowTitle(QStringLiteral("Trading-bot simulation (paper money)"));
    resize(1180, 760);
    buildUi();
    static_cast<void>(connect(m_runner, &BotSimRunner::changed, this, &BotSimDialog::rebuild));
    static_cast<void>(connect(m_runner, &BotSimRunner::log, this, &BotSimDialog::appendLog));
    rebuild();
}

void BotSimDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    buildAccountBox(layout);
    buildTables(layout);
}

// The header: the account, the day, the record, the real-money verdict, the model,
// and the controls that change what the bot does.
void BotSimDialog::buildAccountBox(QVBoxLayout *layout)
{
    auto *account = new QGroupBox(QStringLiteral("Simulated account"), this);
    auto *accountLayout = new QVBoxLayout(account);
    m_accountLabel = new QLabel(account);
    m_pnlLabel = new QLabel(account);
    m_statsLabel = new QLabel(account);
    m_dayLabel = new QLabel(account);
    m_recordLabel = new QLabel(account);
    m_recordLabel->setWordWrap(true);
    m_liveLabel = new QLabel(account);
    m_liveLabel->setWordWrap(true);
    m_modelLabel = new QLabel(account);
    m_modelLabel->setWordWrap(true);
    m_storeLabel = new QLabel(account);
    m_storeLabel->setStyleSheet(QStringLiteral("color: #666;"));
    for (QLabel *label : {m_accountLabel, m_pnlLabel, m_statsLabel, m_dayLabel, m_recordLabel,
                          m_liveLabel, m_modelLabel, m_storeLabel}) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        accountLayout->addWidget(label);
    }

    auto *buttons = new QHBoxLayout;
    m_armButton = new QPushButton(QStringLiteral("Arm the bot"), account);
    m_armButton->setCheckable(true);
    m_armButton->setToolTip(QStringLiteral(
        "Start / stop the simulation. The bot trades SIMULATED money on live prices — "
        "it never places an order at eToro and never moves real funds."));
    static_cast<void>(connect(m_armButton, &QPushButton::toggled, this, [this](bool on) {
        m_runner->setArmed(on);
    }));
    m_resetButton = new QPushButton(QStringLiteral("Reset to start capital"), account);
    static_cast<void>(
        connect(m_resetButton, &QPushButton::clicked, this, &BotSimDialog::confirmReset));
    buttons->addWidget(m_armButton);
    buttons->addWidget(m_resetButton);
    buttons->addStretch(1);
    accountLayout->addLayout(buttons);

    // Local-model row (REQ-F-030): what the model service is doing, how its
    // proposal is used, and a re-probe.
    auto *aiRow = new QHBoxLayout;
    aiRow->addWidget(new QLabel(QStringLiteral("Local model (Ollama):"), account));
    m_aiModeBox = new QComboBox(account);
    m_aiModeBox->addItem(QStringLiteral("off — composite decides"),
                         static_cast<int>(trading::BotAiMode::Off));
    m_aiModeBox->addItem(QStringLiteral("confirm — model must agree"),
                         static_cast<int>(trading::BotAiMode::Confirm));
    m_aiModeBox->addItem(QStringLiteral("lead — trade the model's pick"),
                         static_cast<int>(trading::BotAiMode::Lead));
    m_aiModeBox->setToolTip(QStringLiteral(
        "How the local model's proposal is used.\n"
        "off:     the multi-source composite decides; the proposal is only logged.\n"
        "confirm: only the model's pick may be opened, and only while the composite "
        "agrees with its side (the model is a veto).\n"
        "lead:    the model's own pick and side are traded.\n"
        "In every setting the risk rules still apply — the model can never exceed the "
        "stake, exposure, leverage or ruin limits."));
    static_cast<void>(connect(m_aiModeBox, &QComboBox::currentIndexChanged, this, [this](int idx) {
        m_runner->setAiMode(static_cast<trading::BotAiMode>(m_aiModeBox->itemData(idx).toInt()));
    }));
    aiRow->addWidget(m_aiModeBox);
    m_aiCheckButton = new QPushButton(QStringLiteral("Check model"), account);
    static_cast<void>(connect(m_aiCheckButton, &QPushButton::clicked, this,
                              [this]() { m_runner->checkAi(); }));
    aiRow->addWidget(m_aiCheckButton);
    m_trainButton = new QPushButton(QStringLiteral("Train from experience"), account);
    m_trainButton->setToolTip(QStringLiteral(
        "Refit the outcome model on every trade this bot has closed (REQ-F-033). Runs in "
        "the app itself — no Python needed — and happens automatically every 25 closed "
        "trades as well. The model only gets to refuse trades once it has seen enough of "
        "them and has beaten a coin flip on ones it never saw."));
    static_cast<void>(connect(m_trainButton, &QPushButton::clicked, this,
                              [this]() { m_runner->trainFromExperience(); }));
    aiRow->addWidget(m_trainButton);
    aiRow->addStretch(1);
    accountLayout->addLayout(aiRow);
    m_aiStatusLabel = new QLabel(account);
    m_aiStatusLabel->setWordWrap(true);
    m_aiStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    accountLayout->addWidget(m_aiStatusLabel);
    layout->addWidget(account);
}

// The three lists: what is open, what closed, and why the bot did what it did.
void BotSimDialog::buildTables(QVBoxLayout *layout)
{
    auto *openBox = new QGroupBox(QStringLiteral("Open simulated trades"), this);
    auto *openLayout = new QVBoxLayout(openBox);
    m_openTable = new QTableWidget(openBox);
    configureTable(m_openTable, {QStringLiteral("Instrument"), QStringLiteral("Side"),
                                 QStringLiteral("Invested"), QStringLiteral("Lev"),
                                 QStringLiteral("Entry"), QStringLiteral("Now"),
                                 QStringLiteral("Stop"), QStringLiteral("Target"),
                                 QStringLiteral("Costs"), QStringLiteral("P/L"),
                                 QStringLiteral("Opened / why")});
    openLayout->addWidget(m_openTable);
    layout->addWidget(openBox, 2);

    auto *closedBox = new QGroupBox(QStringLiteral("Closed simulated trades"), this);
    auto *closedLayout = new QVBoxLayout(closedBox);
    m_closedTable = new QTableWidget(closedBox);
    configureTable(m_closedTable, {QStringLiteral("Instrument"), QStringLiteral("Side"),
                                   QStringLiteral("Invested"), QStringLiteral("Lev"),
                                   QStringLiteral("Entry"), QStringLiteral("Exit"),
                                   QStringLiteral("Held (h)"), QStringLiteral("Costs"),
                                   QStringLiteral("Net P/L"), QStringLiteral("Closed because")});
    closedLayout->addWidget(m_closedTable);
    layout->addWidget(closedBox, 2);

    auto *logBox = new QGroupBox(QStringLiteral("Bot decisions"), this);
    auto *logLayout = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    logLayout->addWidget(m_log);
    layout->addWidget(logBox, 1);
}

void BotSimDialog::rebuild()
{
    if (m_armButton->isChecked() != m_runner->armed()) {
        const QSignalBlocker block(m_armButton);
        m_armButton->setChecked(m_runner->armed());
    }
    m_armButton->setText(m_runner->armed() ? QStringLiteral("Stop the bot")
                                           : QStringLiteral("Arm the bot"));
    rebuildAccount();
    rebuildAi();
    rebuildOpenTable();
    rebuildClosedTable();
}

void BotSimDialog::rebuildAi()
{
    const int wanted = m_aiModeBox->findData(static_cast<int>(m_runner->aiMode()));
    if ((wanted >= 0) && (wanted != m_aiModeBox->currentIndex())) {
        const QSignalBlocker block(m_aiModeBox);
        m_aiModeBox->setCurrentIndex(wanted);
    }
    const QString status = m_runner->aiStatus().isEmpty()
                               ? QStringLiteral("not checked yet")
                               : m_runner->aiStatus();
    // Every pick the model named, in its own order — the window has to show the
    // whole answer, since the bot may act on all of them.
    const QList<trading::AiProposal> &picks = m_runner->lastProposals();
    QStringList lines;
    for (const trading::AiProposal &p : picks) {
        const QString side = (p.dir == 0) ? QStringLiteral("HOLD")
                                          : ((p.dir > 0) ? QStringLiteral("BUY")
                                                         : QStringLiteral("SELL"));
        lines << QStringLiteral("<b>%1 %2</b> (conf %3, lev x%4)")
                     .arg(side,
                          p.resolvedSymbol.isEmpty()
                              ? QStringLiteral("%1 [not tradable here]").arg(p.symbol)
                              : p.resolvedSymbol)
                     .arg(p.confidence, 0, 'f', 0)
                     .arg(p.leverage);
    }
    const QString rationale = picks.isEmpty() || picks.constFirst().rationale.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" — %1").arg(picks.constFirst().rationale);
    m_aiStatusLabel->setText(
        QStringLiteral("Service: %1<br>Last picks (%2): %3%4")
            .arg(status)
            .arg(picks.size())
            .arg(picks.isEmpty() ? QStringLiteral("none yet") : lines.join(u" · "), rationale));
}

void BotSimDialog::rebuildAccount()
{
    const PaperStats s = m_runner->stats();
    m_accountLabel->setText(
        QStringLiteral("<b>Start</b> %1 &nbsp;|&nbsp; <b>Equity</b> %2 &nbsp;|&nbsp; "
                       "<b>Cash</b> %3 &nbsp;|&nbsp; <b>Invested</b> %4")
            .arg(plain(s.startEquity), plain(s.equity), plain(s.cash), plain(s.invested)));
    const QString colour = (s.totalPnl >= 0.0) ? QStringLiteral("#1b8a3a") : QStringLiteral("#c0392b");
    m_pnlLabel->setText(
        QStringLiteral("Open P/L %1 &nbsp;|&nbsp; Realised %2 &nbsp;|&nbsp; "
                       "<b style='color:%3'>Total %4 (%5%)</b>")
            .arg(money(s.openPnl), money(s.realized), colour, money(s.totalPnl))
            .arg(s.totalPnlPct, 0, 'f', 2));
    const trading::BotDay day = m_runner->today();
    const trading::PaperPerformance perf = m_runner->performance();
    const trading::LiveReadiness live = m_runner->liveReadiness();
    m_dayLabel->setText(
        QStringLiteral("<b>Today</b> %1 of %2 target · %3 opened, %4 closed · %5")
            .arg(money(day.realized),
                 plain(m_runner->book().config().dailyProfitTarget))
            .arg(day.opened)
            .arg(day.closed)
            .arg((day.realized >= m_runner->book().config().dailyProfitTarget)
                     ? QStringLiteral("<b style='color:#1b8a3a'>target reached — done for today</b>")
                     : ((day.realized <= -m_runner->book().config().dailyLossLimit)
                            ? QStringLiteral("<b style='color:#c0392b'>loss limit reached — done "
                                             "for today</b>")
                            : QStringLiteral("trading"))));
    m_recordLabel->setText(
        QStringLiteral("<b>Record</b> %1 closed over %2 day(s) · net %3 (%4/day, last %5 day(s) "
                       "%6) · profit factor %7 · win rate %8% · max drawdown %9 (%10%) · "
                       "shorts %11 (net %12)")
            .arg(perf.closedTrades)
            .arg(perf.tradingDays)
            .arg(money(perf.netTotal), money(perf.netPerDay))
            .arg(perf.rollingDays)
            .arg(money(perf.netLastDays))
            .arg(perf.profitFactor, 0, 'f', 2)
            .arg(perf.winRate, 0, 'f', 0)
            .arg(plain(perf.maxDrawdown))
            .arg(perf.maxDrawdownPct, 0, 'f', 1)
            .arg(perf.shortTrades)
            .arg(money(perf.shortNet)));
    m_modelLabel->setText(
        QStringLiteral("%1 · experience log: %2")
            .arg(trading::botNetSummary(m_runner->net(), m_runner->netMode(),
                                        trading::NetGateConfig{}),
                 BotSimRunner::experiencePath()));
    m_liveLabel->setText(
        live.ready
            ? QStringLiteral("<b style='color:#1b8a3a'>Real-money criteria met</b> — the paper "
                             "record clears every threshold. Live execution is NOT wired in this "
                             "build: the bot still trades simulated money only.")
            : QStringLiteral("<b style='color:#c0392b'>Not ready for real money</b> — %1. "
                             "Live execution is not wired in this build either; the bot trades "
                             "simulated money only.")
                  .arg(live.blockers.join(u"; ")));
    m_statsLabel->setText(
        QStringLiteral("%1 open · %2 closed (%3 won / %4 lost, win rate %5%) · "
                       "costs paid %6 · best %7 · worst %8")
            .arg(s.openTrades)
            .arg(s.closedTrades)
            .arg(s.wins)
            .arg(s.losses)
            .arg(s.winRate, 0, 'f', 0)
            .arg(plain(s.costsPaid), money(s.bestTrade), money(s.worstTrade)));
    m_storeLabel->setText(QStringLiteral("Simulated money only — no order ever reaches eToro. "
                                         "Books: %1")
                              .arg(BotSimRunner::storePath()));
}

void BotSimDialog::rebuildOpenTable()
{
    const QList<PaperTrade> &open = m_runner->book().openTrades();
    m_openTable->setRowCount(static_cast<int>(open.size()));
    for (qsizetype i = 0; i < open.size(); ++i) {
        const PaperTrade &t = open.at(i);
        const int row = static_cast<int>(i);
        m_openTable->setItem(row, 0, cell(t.symbol));
        m_openTable->setItem(row, 1, cell(t.isBuy ? QStringLiteral("BUY")
                                                  : QStringLiteral("SELL")));
        m_openTable->setItem(row, 2, cell(plain(t.stake), true));
        m_openTable->setItem(row, 3, cell(QStringLiteral("x%1").arg(t.leverage), true));
        m_openTable->setItem(row, 4, cell(rate(t.openRate), true));
        // A mark that is not live is flagged, exactly as the real open-trades
        // table flags one (a stale quote must never look like a live P/L).
        m_openTable->setItem(row, 5,
                             cell(t.markLive ? rate(t.effectiveRate())
                                             : QStringLiteral("%1 (not live)")
                                                   .arg(rate(t.effectiveRate())),
                                  true));
        m_openTable->setItem(row, 6, cell(rate(t.slRate), true));
        m_openTable->setItem(row, 7, cell(rate(t.tpRate), true));
        m_openTable->setItem(row, 8,
                             cell(QStringLiteral("%1 (+%2 nights)")
                                      .arg(plain(t.costsSoFar()))
                                      .arg(t.nightsCharged),
                                  true));
        m_openTable->setItem(row, 9, cell(money(t.netPnl()), true, t.netPnl(), true));
        m_openTable->setItem(row, 10,
                             cell(QStringLiteral("%1 — %2")
                                      .arg(t.openTime.toString(QStringLiteral("MM-dd HH:mm")),
                                           t.entryBasis)));
    }
}

void BotSimDialog::rebuildClosedTable()
{
    const QList<PaperClosedTrade> &closed = m_runner->book().closedTrades();
    m_closedTable->setRowCount(static_cast<int>(closed.size()));
    // Newest first: the interesting end of a multi-day experiment is the recent one.
    for (qsizetype i = 0; i < closed.size(); ++i) {
        const PaperClosedTrade &c = closed.at(closed.size() - 1 - i);
        const int row = static_cast<int>(i);
        m_closedTable->setItem(row, 0, cell(c.symbol));
        m_closedTable->setItem(row, 1, cell(c.isBuy ? QStringLiteral("BUY")
                                                    : QStringLiteral("SELL")));
        m_closedTable->setItem(row, 2, cell(plain(c.stake), true));
        m_closedTable->setItem(row, 3, cell(QStringLiteral("x%1").arg(c.leverage), true));
        m_closedTable->setItem(row, 4, cell(rate(c.openRate), true));
        m_closedTable->setItem(row, 5, cell(rate(c.closeRate), true));
        m_closedTable->setItem(row, 6, cell(QStringLiteral("%1").arg(c.heldHours(), 0, 'f', 1),
                                            true));
        m_closedTable->setItem(row, 7, cell(plain(c.totalCost()), true));
        m_closedTable->setItem(row, 8, cell(money(c.netPnl), true, c.netPnl, true));
        m_closedTable->setItem(row, 9, cell(trading::closeReasonWord(c.reason)));
    }
}

void BotSimDialog::appendLog(const QString &message, bool isError)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_log->appendPlainText(QStringLiteral("%1  %2%3")
                               .arg(stamp, isError ? QStringLiteral("ERROR: ") : QString(),
                                    message));
}

void BotSimDialog::confirmReset()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Reset the simulation?"),
        QStringLiteral("Close the open simulated trades at their current mark and start over "
                       "at the configured capital?\n\nThe recorded history is discarded — the "
                       "experiment starts from zero."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes) {
        m_runner->resetBooks();
    }
}
