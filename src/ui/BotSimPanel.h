#ifndef TRADINGAPP_UI_BOTSIMPANEL_H
#define TRADINGAPP_UI_BOTSIMPANEL_H

#include "domain/BotNet.h"
#include "domain/DecisionEngine.h"
#include "domain/Models.h"
#include "domain/PaperTrader.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QString>

class EtoroClient;
class OllamaAdvisor;
class QVBoxLayout;
QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPlainTextEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTableWidget)
QT_FORWARD_DECLARE_CLASS(QTimer)

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
    BotSimRunner(EtoroClient *client, OllamaAdvisor *ai, QObject *parent = nullptr);

    // The single explicit step that turns the experiment on. While disarmed the
    // bot keeps marking and reporting its existing positions (the books stay
    // honest) but opens nothing new.
    void setArmed(bool armed);
    [[nodiscard]] bool armed() const { return m_armed; }

    // How the AI proposal is used (REQ-F-030). Changing it is logged: it changes
    // what the running experiment measures.
    void setAiMode(trading::BotAiMode mode);
    // Refit the outcome model from the experience log, off the GUI thread. Also runs
    // itself every kRetrainEvery closed trades (REQ-F-033).
    void trainFromExperience();
    // The daily target / loss limit from configuration (REQ-F-031). Logged, because
    // changing what a day must earn changes what the record means.
    void applyDailyRules(double target, double lossLimit);
    [[nodiscard]] trading::BotAiMode aiMode() const { return m_book.config().aiMode; }
    // The picks of the last answer, best first, for the window's AI line (empty =
    // none yet). The model may name as many instruments as it thinks worthwhile —
    // the risk budget, not this list, limits how many become trades.
    [[nodiscard]] const QList<trading::AiProposal> &lastProposals() const & { return m_proposals; }
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
    [[nodiscard]] trading::BotNetMode netMode() const { return m_netMode; }
    // Where the books are persisted (shown in the window, so the file behind a
    // multi-day experiment is never a mystery). One location per installation,
    // hence static.
    [[nodiscard]] static QString storePath();
    // The append-only training set, and the model trained from it.
    [[nodiscard]] static QString experiencePath();
    [[nodiscard]] static QString modelPath();

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
    void changed();  // books moved (or the AI state did) — refresh the window

private:
    void tick();                 // periodic: mark, accrue rollover, apply exits
    // The AI leg of a decision tick: ask the local model, then run the entries
    // with whatever it answered (or without it, when it fails or times out).
    void requestProposal();
    // The open book as the model is shown it, appended to the evidence prompt.
    [[nodiscard]] QString holdEvidence() const;
    // One decision row as the domain's candidate: quotes, leverage ladder, market
    // state, fee table and whether the model backed it.
    [[nodiscard]] trading::CandidateInput candidateFor(const trading::DecisionRow &row,
                                                       const trading::AiGate &gate,
                                                       const QList<double> &closes,
                                                       const QDateTime &now) const;
    // The learned model's say on one candidate: "" to proceed, else the refusal
    // code. Annotates the basis line when it scored but allowed.
    [[nodiscard]] QString applyNetGate(const QString &symbol,
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
    [[nodiscard]] static QList<trading::TrainingExample> readExperience();
    // The model's verdict on holding this position: AiExit or None (never on
    // silence). Logs the reason when it closes.
    [[nodiscard]] trading::CloseReason aiExitFor(const trading::PaperTrade &trade);
    // Is the model's current answer young enough to act on? Same bound as entries.
    [[nodiscard]] bool aiProposalsFresh() const;
    void onProposals(const QList<AiDecision> &picks, const QString &error);
    void considerEntriesForScan();  // entries for the stored scan, with m_proposal
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
    [[nodiscard]] Sides sidesFor(const QString &symbol, qint64 instrumentId) const;
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
    QFutureWatcher<trading::TrainResult> m_training;
    trading::BotNet m_net;                  // what the record has taught it so far
    trading::BotNetMode m_netMode = trading::BotNetMode::Off;
    qint64 m_experienceCount = 0;           // training examples written this session
    QString m_evidence;                     // prompt of the scan being decided
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

// The bot's window: the simulated account, its open and closed simulated trades,
// and the decision log. A view over the runner — no trading logic here.
class BotSimDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BotSimDialog(BotSimRunner *runner, QWidget *parent = nullptr);

private:
    void buildUi();
    void buildAccountBox(QVBoxLayout *layout);
    void buildTables(QVBoxLayout *layout);
    void rebuild();                 // refresh everything from the runner
    void rebuildAccount();          // the header figures
    void rebuildAi();               // the local-model row: status, mode, last proposal
    void rebuildOpenTable();
    void rebuildClosedTable();
    void appendLog(const QString &message, bool isError);
    void confirmReset();

    BotSimRunner *m_runner = nullptr;
    QPushButton *m_armButton = nullptr;     // checkable: the explicit ARM step
    QPushButton *m_resetButton = nullptr;
    QLabel *m_accountLabel = nullptr;       // start / cash / invested / equity
    QLabel *m_pnlLabel = nullptr;           // open / realised / total P/L
    QLabel *m_statsLabel = nullptr;         // trades, win rate, costs paid
    QLabel *m_dayLabel = nullptr;           // today against the daily target / limit
    QLabel *m_recordLabel = nullptr;        // the measured record (REQ-F-031)
    QLabel *m_liveLabel = nullptr;          // the real-money verdict and its blockers
    QPushButton *m_trainButton = nullptr;   // refit the outcome model on demand
    QLabel *m_modelLabel = nullptr;         // the learned model and the experience log
    QLabel *m_storeLabel = nullptr;         // where the books live
    QComboBox *m_aiModeBox = nullptr;       // off / confirm / lead (REQ-F-030)
    QPushButton *m_aiCheckButton = nullptr; // re-probe the local model service
    QLabel *m_aiStatusLabel = nullptr;      // service state + the latest proposal
    QTableWidget *m_openTable = nullptr;
    QTableWidget *m_closedTable = nullptr;
    QPlainTextEdit *m_log = nullptr;
};

#endif // TRADINGAPP_UI_BOTSIMPANEL_H
