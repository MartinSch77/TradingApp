// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_BOTSIMRUNNER_H
#define TRADINGAPP_UI_BOTSIMRUNNER_H

#include "domain/BotNet.h"
#include "domain/DecisionEngine.h"
#include "domain/Models.h"
#include "domain/PaperTrader.h"
#include "domain/DecisionLog.h"
#include "domain/PredictionLedger.h"

#include <QHash>
#include <QList>
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QString>

class EtoroClient;
class OllamaAdvisor;
QT_FORWARD_DECLARE_CLASS(QTimer)

// The three money/price formatters the bot's views share. Free functions here so the
// Widgets dialog AND the console front end print byte-identical figures rather than each
// rolling its own — which is also what keeps PMD CPD from seeing a clone.
[[nodiscard]] QString botMoney(double value);
[[nodiscard]] QString botPlain(double value);
[[nodiscard]] QString botRate(double value);

// The paper-trading bot (REQ-F-029): runs the app's own composite decision over
// every instrument with SIMULATED money on LIVE prices, so a strategy can be
// measured over days without risking anything.
//
// It reads from EtoroClient and NEVER writes: no openPosition, no closePosition,
// no order of any kind. The only thing it asks the client for is to keep quoting
// the instruments it simulates (setExtraQuoteInstruments), which places no order
// and moves no money. That is why REQ-N-005's double-press gate does not apply —
// there is nothing to confirm; arming is one visible action.
//
// The books, the cost model and every entry/exit rule live in the domain layer
// (trading::PaperBook and friends); this class is the plumbing that feeds them
// live quotes, spreads, fees and decisions, and persists them across restarts.
class BotSimRunner : public QObject
{
    Q_OBJECT
public:
    // `ai` is optional (may be null): the LOCAL model advisor whose proposal the
    // simulator can trade on (REQ-F-030). The runner owns no advisor logic — it
    // asks, waits, logs, and lets the pure gate decide what the answer means.
    // `storeFileName` overrides the persisted book's file name (default botsim.json): a
    // second runner in another process — the advise console's one-instrument experiment —
    // must never write the main bot's book.
    BotSimRunner(EtoroClient *client, OllamaAdvisor *ai, QObject *parent = nullptr,
                 const QString &storeFileName = QString());

    // The single explicit step that turns the experiment on. While disarmed the
    // bot keeps marking and reporting its existing positions (the books stay
    // honest) but opens nothing new.
    void setArmed(bool armed);
    // Narrow (or widen) the traded focus set (REQ-F-034); empty = the whole catalog. The
    // advise console's --trade mode focuses the runner on exactly its one instrument.
    void setFocusSymbols(const QStringList &symbols);
    // One crowd-evidence line per instrument (REQ-F-046): appended to the model's prompt as
    // EVIDENCE beside the technical lines — it gates, sizes and stops NOTHING (the
    // deterministic risk rules never read it), and an empty line clears the entry so absent
    // stays absent. Optional: the console front end never calls it and behaves identically.
    void setCrowdEvidence(const QString &instrument, const QString &line);
    [[nodiscard]] bool armed() const { return m_armed; }

    // How the AI proposal is used (REQ-F-030). Changing it is logged: it changes
    // what the running experiment measures.
    void setAiMode(trading::BotAiMode mode);
    // Refit the outcome model from the experience log, off the GUI thread. Also runs
    // itself every kRetrainEvery closed trades (REQ-F-033).
    void trainFromExperience();
    // The reference series the confluence read is computed from (REQ-F-035).
    void setReferenceSeries(const QHash<QString, QList<double>> &series)
    {
        m_referenceSeries = series;
    }
    // The regime the combined indication is judged in (REQ-F-036). Handed over by the
    // window that measures it, so the bot and the signals row cannot disagree about
    // what the market is doing. Unset means UNKNOWN, which the signal reports as such
    // rather than treating as calm.
    void setRegime(bool vixValid, double vix, bool eventRisk)
    {
        m_vixValid = vixValid;
        m_vix = vix;
        m_eventRisk = eventRisk;
    }
    // The daily target / loss limit from configuration (REQ-F-031). Logged, because
    // changing what a day must earn changes what the record means.
    void applyDailyRules(double target, double lossLimit);
    [[nodiscard]] trading::BotAiMode aiMode() const { return m_book.config().aiMode; }
    // The picks of the last answer, best first, for the window's AI line (empty =
    // none yet). The model may name as many instruments as it thinks worthwhile —
    // the risk budget, not this list, limits how many become trades.
    [[nodiscard]] const QList<trading::AiProposal> &lastProposals() const & { return m_proposals; }
    // The instruments the model was SHOWN in the last request. An instrument that is
    // not in here could not have had an opinion, which is a different answer from
    // having looked at it and passed.
    [[nodiscard]] const QStringList &lastAskedSymbols() const & { return m_askedSymbols; }
    // One-line state of the local model service ("qwen2.5:1.5b ready at …",
    // "not reachable …"), refreshed by the probe. Empty = never checked.
    [[nodiscard]] QString aiStatus() const { return m_aiStatus; }
    // Ask the daemon whether it is there and serves the configured model.
    void checkAi();

    [[nodiscard]] const trading::PaperBook &book() const & { return m_book; }
    [[nodiscard]] trading::PaperStats stats() const { return m_book.stats(); }
    // What the record says about the strategy, and whether that is enough evidence
    // to risk real money (REQ-F-031). Measured, never assumed.
    [[nodiscard]] trading::PaperPerformance performance() const;
    [[nodiscard]] trading::LiveReadiness liveReadiness() const;
    [[nodiscard]] trading::BotDay today() const { return m_book.day(); }
    [[nodiscard]] const trading::BotNet &net() const & { return m_net; }
    // What the model last said about one open position (hold / close / no opinion).
    [[nodiscard]] trading::HoldVerdict holdOpinion(qint64 tradeId) const
    {
        return m_holdOpinions.value(tradeId);
    }
    [[nodiscard]] trading::BotNetMode netMode() const { return m_netMode; }
    // Where the books are persisted (shown in the window, so the file behind a
    // multi-day experiment is never a mystery). One location per installation,
    // hence static.
    [[nodiscard]] QString storePath() const;
    // The append-only training set, and the model trained from it.
    [[nodiscard]] QString experiencePath() const;
    [[nodiscard]] QString siblingPath(const QString &defaultName,
                                      const QString &suffix) const;
    [[nodiscard]] static QString modelPath();
    // The append-only prediction ledger: every evaluation, including the ones that
    // stayed out, and what the market then did (REQ-F-037).
    [[nodiscard]] static QString ledgerPath();
    // The human-readable decision log: one line per instrument CONSIDERED, saying
    // whether it was traded and why (REQ-F-029). The ledger above is the machine's
    // copy for measuring; this one is the one a person opens after a long weekend.
    [[nodiscard]] QString decisionLogPath() const;

    // Fed by the main window after every all-instruments scan: the composite
    // decision per instrument plus the snapshot behind it (its screenerRows carry
    // the close series, and it is also what the AI evidence prompt is built from).
    // This is the bot's decision tick — it closes what turned against it and opens
    // what the ranking, or the model, now favours.
    void onDecisions(const QList<trading::DecisionRow> &rows, const trading::MarketSnapshot &snap);

    // Start over at the configured capital, closing the open simulated trades at
    // their current mark first so the closed list explains the reset.
    void resetBooks();

signals:
    void log(const QString &message, bool isError);
    // A simulated position was just opened. The window is not always open, so the
    // notice is raised by whoever owns the runner rather than from here.
    void tradeOpened(const QString &symbol);
    // One line PER evaluated candidate each scan (REQ-F-034 visibility): traded or refused,
    // the countable code and the human reason. The GUI need not connect it (its decision
    // window shows this already); the advise console prints it so a person watching one
    // instrument sees exactly why it traded or was refused, every cycle.
    void entryDecision(const QString &symbol, bool traded, const QString &code,
                       const QString &why);
    // The local model's latest picks, for the views that show it as a SOURCE
    // (the signals panel and the decision window, REQ-F-034) rather than as the
    // bot's decision.
    void proposalsUpdated(const QList<trading::AiProposal> &picks);
    void changed();  // books moved (or the AI state did) — refresh the window

private:
    void tick();                 // periodic: mark, accrue rollover, apply exits
    // The AI leg of a decision tick: ask the local model, then run the entries
    // with whatever it answered (or without it, when it fails or times out).
    void requestProposal();
    // The open book as the model is shown it, appended to the evidence prompt.
    [[nodiscard]] QString holdEvidence() const;
    [[nodiscard]] QString crowdEvidenceBlock() const;
    // One decision row as the domain's candidate: quotes, leverage ladder, market
    // state, fee table and whether the model backed it.
    [[nodiscard]] trading::CandidateInput candidateFor(const trading::DecisionRow &row,
                                                       const trading::AiGate &gate,
                                                       const QList<double> &closes,
                                                       const QDateTime &now) const;
    // When this instrument last closed a position, and how many positions the book
    // opened in the past hour — the churn inputs of REQ-F-034.
    [[nodiscard]] QDateTime lastCloseFor(const QString &symbol) const;
    [[nodiscard]] qint32 opensInLastHour(const QDateTime &now) const;
    // The learned model's say on one candidate. The WHOLE verdict comes back, because
    // the decision log needs the sentence and not only the code; `allow` is the answer
    // to "may this trade proceed". Annotates the basis line when it scored but allowed.
    [[nodiscard]] trading::NetVerdict applyNetGate(const QString &symbol,
                                                   const trading::EntryFeatures &features,
                                                   trading::EntrySignal &sig);
    // The entry's own numbers, captured for the experience log (REQ-F-033).
    [[nodiscard]] trading::EntryFeatures featuresFor(const trading::CandidateInput &in,
                                                     const trading::EntrySignal &sig, double stake,
                                                     const QDateTime &now);
    // Append one training example for a trade that just closed.
    void recordExperience(const trading::PaperClosedTrade &done);
    void loadModel();
    void onTrainingDone();
    [[nodiscard]] QList<trading::TrainingExample> readExperience() const;
    // The model's verdict on holding this position: AiExit or None (never on
    // silence). Logs the reason when it closes.
    [[nodiscard]] trading::CloseReason aiExitFor(const trading::PaperTrade &trade);
    // Is the model's current answer young enough to act on? Same bound as entries.
    [[nodiscard]] bool aiProposalsFresh() const;
    // Why the model has (or has not) an answer — so a refusal names the cause.
    [[nodiscard]] trading::AiSource aiSourceState() const;
    // Spread with the crypto cost floor applied (trading::minSpreadPctFor).
    [[nodiscard]] double effectiveSpreadPct(const QString &symbol) const;
    // Focus + market obstacles, in order; empty code = neither applies.
    [[nodiscard]] QString preTradeRefusal(const QString &symbol, QString *why) const;
    void onProposals(const QList<AiDecision> &picks, const QString &error);
    void considerEntriesForScan();
    // One line per scan: what the RECORD says about calls like the ones just made
    // (REQ-F-037) — a measured probability per horizon or an explicit refusal to quote
    // one, plus whether the app's own calls have beaten the cheap baselines yet.
    void reportForecast(const QList<trading::DecisionRow> &rows);
    // Append one row to the prediction ledger for an evaluated candidate — taken or
    // refused, and the refusals are the point (REQ-F-037). A default-constructed `in`
    // means nothing was evaluated yet, so the row carries the refusal and no evidence.
    // An EMPTY refusal means the trade was taken — the two cannot then disagree.
    void recordPrediction(const trading::DecisionRow &row, const QList<double> &closes,
                          const QDateTime &now, const trading::CandidateInput &in,
                          const QString &refusal) const;  // entries for the stored scan, with m_proposal
    void markAndExit();          // one pass over the open simulated positions
    // The rate a simulated position closes at right now (bid for a long, ask for
    // a short), plus whether that came from a live quote. 0 = unknown.
    struct Mark {
        double rate = 0.0;
        bool live = false;
    };
    [[nodiscard]] Mark markFor(const trading::PaperTrade &trade) const;
    // Two-sided quote for a candidate instrument: the per-tick quote book when the
    // bot already holds it, otherwise the mid of the last bulk snapshot widened by
    // the instrument's live spread. ok=false = not priced, so not tradable.
    struct Sides {
        bool ok = false;
        double bid = 0.0;
        double ask = 0.0;
        double spreadPct = 0.0;
    };
    [[nodiscard]] Sides sidesFor(const QString &symbol, qint64 instrumentId,
                                 const QList<double> &closes) const;
    void considerEntries(const QList<trading::DecisionRow> &rows, const QList<ScreenerRow> &scan);
    // One candidate, all the way from the AI gate to an opened simulated trade;
    // true = a trade was opened, and `skipCode` receives the countable reason when
    // it was not. Split out of considerEntries so that stays a loop (and both stay
    // inside the complexity budget).
    bool tryOpen(const trading::DecisionRow &row, const QList<double> &closes,
                 const QDateTime &now, QString *skipCode);
    // One candidate's direction under the current AI mode, refusal reason included.
    [[nodiscard]] trading::AiGate gateFor(const trading::DecisionRow &row) const;
    // Keep the client quoting exactly the instruments the bot simulates.
    void syncQuoteInterest();
    // `trade` MUST be a caller-owned copy, never a reference into
    // PaperBook::openTrades(): closing removes that entry, so such a reference
    // would dangle halfway through this call. Both call sites hold a local copy.
    // Book the day when one open winner already completes the target (REQ-F-031).
    // True when it closed a position.
    bool harvestDayTarget();
    void closeTrade(const trading::PaperTrade &trade, trading::CloseReason reason);
    void save() const;
    void load();

    EtoroClient *m_client = nullptr;
    OllamaAdvisor *m_ai = nullptr;          // optional local-model advisor (may be null)
    QTimer *m_timer = nullptr;
    trading::PaperBook m_book;
    bool m_armed = false;
    double m_eurPerUsd = 0.0;               // 0 = unknown; fees then stay in their own currency
    QSet<QString> m_tradeable;              // symbols whose market is open right now
    bool m_tradeabilityKnown = false;
    // Latest composite call per symbol, so the exit check between scans judges an
    // open position against the newest signal it has.
    QHash<QString, qint32> m_dirBySymbol;
    QHash<QString, double> m_confBySymbol;
    QSet<QString> m_feesRequested;          // one fee fetch per symbol, not one per tick

    // AI leg (REQ-F-030). The newest scan's rows/snapshot are kept, and a proposal
    // is applied to THOSE — a local model thinks for tens of seconds, so a newer
    // scan landing meanwhile is the normal case, not a reason to throw the answer
    // away. What does disqualify it is AGE: past m_askedAt + the staleness bound
    // the evidence it reasoned over is no longer the market, and it is dropped.
    // (The entry itself is re-validated against live quotes, spread and market
    // state at the moment it opens, so a fresh proposal on newer rows is sound.)
    QList<trading::AiProposal> m_proposals;
    QStringList m_askedSymbols;             // what the last prompt actually listed
    // The reference series (^VIX / ^VXN / ^TNX / per-index heavyweights) the confluence read
    // needs, handed over by the window that fetches them.
    QHash<QString, QList<double>> m_referenceSeries;
    // Both taken from the scan's own snapshot (see onDecisions): the volume bars keyed
    // by Yahoo ticker, and the per-instrument series keyed by APP symbol.
    QHash<QString, trading::VolumeSeries> m_referenceVolumes;
    QHash<QString, QList<double>> m_symbolSeries;
    // The combined indication's strength per instrument from the last evaluation, so the
    // forecast line quotes the SAME number the ledger recorded rather than recomputing it.
    QHash<QString, double> m_lastLeadStrength;
    // The regime the combined indication is judged in (REQ-F-036); unset = unknown.
    bool m_vixValid = false;
    double m_vix = 0.0;
    bool m_eventRisk = false;
    QFutureWatcher<trading::TrainResult> m_training;
    // The model's latest verdict per open trade, refreshed on every review pass.
    QHash<qint64, trading::HoldVerdict> m_holdOpinions;
    trading::BotNet m_net;                  // what the record has taught it so far
    trading::BotNetMode m_netMode = trading::BotNetMode::Off;
    qint64 m_experienceCount = 0;           // training examples written this session
    QString m_evidence;                     // prompt of the scan being decided
    QHash<QString, QString> m_crowdEvidence; // instrument -> evidence line (REQ-F-046)
    QString m_storeFile;   // book file override (empty = botsim.json)
    QList<trading::DecisionRow> m_pendingRows;
    QList<ScreenerRow> m_pendingScan;
    QDateTime m_askedAt;                    // when the in-flight request went out
    bool m_aiPending = false;
    QString m_aiStatus;                     // last availability line, for the window
    // What load() found, emitted once the object graph is connected: a signal from
    // the CONSTRUCTOR reaches nobody, and "RESUMED ARMED" is precisely the line a
    // multi-day experiment must not lose.
    QString m_restoreNote;
};

#endif // TRADINGAPP_UI_BOTSIMRUNNER_H
