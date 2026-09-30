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
#include "domain/TradingStrategy.h"

#include <QHash>
#include <QList>
#include <QFutureWatcher>
#include <QLockFile>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>

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
                 QString storeFileName = QString());

    // The single explicit step that turns the experiment on. While disarmed the
    // bot keeps marking and reporting its existing positions (the books stay
    // honest) but opens nothing new.
    void setArmed(bool armed);
    // Narrow (or widen) the traded focus set (REQ-F-034); empty = the whole catalog. The
    // advise console's --trade mode focuses the runner on exactly its one instrument.
    void setFocusSymbols(const QStringList &symbols);
    // Within the focus set, the symbols exempt from the extra reluctance bar (REQ-F-034);
    // empty = no distinction, every focus symbol trades at the normal bar. The GUI sets this
    // to the two headline indices once it widens the focus set to the whole catalogue, so a
    // peripheral name still has to bring more conviction than SPX500/NSDQ100 do.
    void setCoreFocusSymbols(const QStringList &symbols);
    // One crowd-evidence line per instrument (REQ-F-046): appended to the model's prompt as
    // EVIDENCE beside the technical lines — it gates, sizes and stops NOTHING (the
    // deterministic risk rules never read it), and an empty line clears the entry so absent
    // stays absent. Optional: the console front end never calls it and behaves identically.
    void setCrowdEvidence(const QString &instrument, const QString &line);
    [[nodiscard]] bool armed() const { return m_armed; }
    // Whether THIS process holds the book (a QLockFile beside the store file). The GUI
    // and the console share one config dir and one book by design, and nothing else
    // stopped both from running the bot at once — each marking, opening and rewriting
    // botsim.json and the ledgers over the other. A runner that does not own its book
    // still loads and shows it (examining is the console's purpose) but cannot be armed,
    // never marks, trades or saves, and says so in the log; the views may show it.
    [[nodiscard]] bool ownsBook() const { return m_ownsBook; }

    // How the AI proposal is used (REQ-F-030). Changing it is logged: it changes
    // what the running experiment measures.
    void setAiMode(trading::BotAiMode mode);
    // Refit the outcome model from the experience log, off the GUI thread. Also runs
    // itself every kRetrainEvery closed trades (REQ-F-033).
    void trainFromExperience();
    // The reference series the confluence read is computed from (REQ-F-059..-067).
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
    // The daily bars SwingPullbackStrategyV1 reasons over (2026-08-12 redesign, item 5's
    // live wiring), fed by whoever fetches them (a daily-interval Yahoo series is enough —
    // this needs no live quote). `symbol` must be one of BotConfig::swingStrategySymbols
    // for the bars to be used; harmless otherwise. Replaces, never appends: the caller
    // owns re-fetching, this class owns none of that plumbing.
    void setDailyBars(const QString &symbol, const QList<trading::DailyBar> &bars)
    {
        m_dailyBars.insert(symbol, bars);
    }
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
    // The swing entry's counterpart of featuresFor: the same feature vector, measured over
    // the DAILY bars the strategy reasoned over. Set at open, because recordExperience
    // writes nothing for a record whose features are invalid — without it every swing
    // close, partial or final, silently left the training set.
    [[nodiscard]] trading::EntryFeatures swingFeaturesFor(const trading::EntrySignal &sig,
                                                          const QList<trading::DailyBar> &bars,
                                                          double stake, const QDateTime &now);
    // Append one training example for a trade that just closed. A PARTIAL record
    // (PaperBook::partialClose) appends nothing: its net is banked in m_partialNetById
    // and folded into the label of the final close of the same id.
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
    // `closes` are the scan's HOURLY candles (the regime's input and the price of last
    // resort); the row's price is the mid the fill would be priced at (sidesFor), its
    // five-minute baseline comes from the instrument's 1-minute series when that is LIVE
    // (liveSessionSeries), and every row is tagged with the composite bot's strategy
    // version. Not const: the row goes to the on-disk ledger AND to the in-memory copy
    // of it.
    void recordPrediction(const trading::DecisionRow &row, const QList<double> &closes,
                          const QDateTime &now, const trading::CandidateInput &in,
                          const QString &refusal);   // entries for the stored scan, with m_proposal
    // The prediction ledger as this process knows it: read from disk ONCE, on first use,
    // then kept in step with every row recordPrediction appends. Re-reading the file per
    // scan was O(file) work on a file that grows by a scan's worth of rows every few
    // minutes — unbounded over the weeks a Pi is left running. The copy is PER PROCESS:
    // a row another process appends to the shared file (the advise console) is not seen
    // here until restart, where the per-scan re-read used to pick it up.
    [[nodiscard]] const QList<trading::Prediction> &ledgerRows();
    void markAndExit();          // one pass over the open simulated positions
    // The save at the end of a mark pass: immediately when the pass changed the book's
    // SHAPE (a close, a partial, a harvest — the open/closed counts differ from the ones
    // taken before the pass), otherwise at most once per kMarkSaveIntervalSecs, because
    // a pure mark moved every 5-second tick and rewrote the whole book ~17 000 times a
    // day while one position was open.
    void saveAfterMarks(const QDateTime &now, qsizetype openBefore, qsizetype closedBefore);
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
    //
    // `live` is the honest part, and only ONE of the three price paths is actually
    // time-checked: a per-tick quote carries the venue's own stamp, so it is live only
    // while trading::quoteIsFresh says so — a stalled one still prices the candidate
    // (it falls through to the bulk mid, since the scan just priced the instrument)
    // but is NOT live, because the venue's own stamp says this instrument stopped
    // printing and an unstamped mid from the same venue cannot overrule that. The
    // bulk-snapshot mid and the candle-close fallback have NO stamp of their own; they
    // count as live when nothing dated contradicts them — the behaviour they always had,
    // stated here rather than invented a timestamp for. The ledger row of a candidate is
    // priced off the same mid (recordPrediction), so the record and the fill never
    // disagree about what the instrument cost at the moment of the call.
    struct Sides {
        bool ok = false;
        bool live = false;
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
    // The opened trade's line in the persistent decision log, geometry beside evidence
    // (see DES-DOM-DECLOG). Split out of tryOpen to keep it inside the metrics ratchet.
    void appendOpenedNote(const trading::EntrySignal &sig, const trading::CandidateInput &in,
                          const trading::EntryVerdict &verdict, double compositeConfidence,
                          const QDateTime &now) const;
    // One candidate's direction under the current AI mode, refusal reason included.
    [[nodiscard]] trading::AiGate gateFor(const trading::DecisionRow &row) const;
    // The swing strategy's own entry path (2026-08-12 redesign, item 5's live wiring):
    // evaluated per scan, independent of the composite/AI gate above — see BotConfig::
    // useSwingStrategy's own comment for why this is a SEPARATE path rather than a slot
    // in tryOpen. One call per configured symbol; each names its own refusal.
    void considerSwingEntries(const QDateTime &now);
    // One symbol's own attempt, split out so considerSwingEntries stays a loop —
    // mirrors tryOpen's own split out of considerEntries.
    void trySwingOpen(const QString &symbol, const QDateTime &now);
    // The swing strategy's own exit path for ONE open position it manages (identified by
    // PaperTrade::strategyVersion being non-empty): the shared stop-barrier check first
    // (barrierHit, the same one every other exit path uses), then — at most once per
    // calendar day, since swingExitDecision's own state is day-granular — the strategy's
    // time-stop/partial/trailing rules. Returns true when the position closed outright
    // (a partial leaves it open, at the reduced stake, under the same id).
    bool applySwingExit(const trading::PaperTrade &trade, double markRate, const QDateTime &now);
    // Keep the client quoting exactly the instruments the bot simulates.
    void syncQuoteInterest();
    // `trade` MUST be a caller-owned copy, never a reference into
    // PaperBook::openTrades(): closing removes that entry, so such a reference
    // would dangle halfway through this call. Both call sites hold a local copy.
    // Book the day when one open winner already completes the target (REQ-F-031).
    // `now` is the mark clock, so "today" is judged against the same time the
    // marks were taken at. True when it closed a position.
    bool harvestDayTarget(const QDateTime &now);
    void closeTrade(const trading::PaperTrade &trade, trading::CloseReason reason);
    void save() const;
    void load();
    // Try the book's lock once, right after load(): sets m_ownsBook, and when the book is
    // held elsewhere drops a RESTORED armed flag — the process that holds the book is the
    // one running the experiment, this one only looks at it.
    void acquireBookLock();
    // The one sentence every refusal of a non-owned book uses, naming the holder
    // (QLockFile::getLockInfo: pid, host, application).
    [[nodiscard]] QString bookHolderLine() const;
    // The "books restored" line over the loaded book, with `state` saying what became of
    // the file's armed flag — composed by load() and composed AGAIN by acquireBookLock()
    // once it is known whether this process runs the book or only looks at it.
    [[nodiscard]] QString restoreNoteFor(const QString &state) const;

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
    // The symbols whose series in m_symbolSeries CHANGED between the previous scan and this
    // one — the runner's only test of a live 1-minute feed (see adoptSymbolSeries). The
    // structure read and the ledger baseline use a series only while it is in here.
    QSet<QString> m_liveSeries;
    // Store the scan's per-symbol series and judge which of them are live.
    void adoptSymbolSeries(const QHash<QString, QList<double>> &series);
    // The symbol's 1-minute series when it is live, else empty (= no read).
    [[nodiscard]] QList<double> liveSessionSeries(const QString &symbol) const;
    // The swing strategy's own daily bars (2026-08-12 redesign, item 5's live wiring),
    // keyed by APP symbol like every other per-instrument series here — set via
    // setDailyBars, read by considerSwingEntries/applySwingExit only.
    QHash<QString, QList<trading::DailyBar>> m_dailyBars;
    // The last calendar date swingExitDecision's day-granular rules ran for a given open
    // trade id, so a 5-second mark tick does not increment sessionsHeld/re-run the
    // time-stop/partial/trailing checks dozens of times before the day's bar even closes.
    // Not persisted: at most one extra day-count tick is lost across a restart, which is
    // a smaller error than the state it is protecting.
    QHash<qint64, QDate> m_swingLastEvalDate;
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
    // Net already booked by PARTIAL closes of a still-open position, per trade id, so the
    // one example written when the remainder finally closes is labelled with the WHOLE
    // position's net rather than the remainder's share (REQ-F-033: one setup, one label).
    // Not persisted: a restart mid-position loses at most the partial's share of that
    // label, which is a smaller error than a second example with the same entry
    // features would be.
    QHash<qint64, double> m_partialNetById;
    // The in-memory ledger behind ledgerRows(); `m_ledgerLoaded` distinguishes "not read
    // yet" from "read, and empty" — a fresh install has no file at all.
    QList<trading::Prediction> m_ledger;
    bool m_ledgerLoaded = false;
    // When markAndExit last wrote the book for marks alone (see saveAfterMarks). Not
    // persisted: a crash loses at most that interval's worth of mark/peakNet/rollover
    // state, and aboutToQuit still saves on a clean exit.
    QDateTime m_lastMarkSave;
    QString m_evidence;                     // prompt of the scan being decided
    QHash<QString, QString> m_crowdEvidence; // instrument -> evidence line (REQ-F-046)
    QString m_storeFile;   // book file override (empty = botsim.json)
    // One process per book: `storePath() + ".lock"`, held for the runner's lifetime and
    // released by QLockFile's own destructor. Its stale time is set to 0 — the age rule
    // OFF — because QLockFile's default treats a lock file older than 30 s as stale EVEN
    // WHEN its holder is alive (isApparentlyStale checks the pid and the boot id, then
    // falls through to the age; the modification time is never refreshed while held),
    // and a bot holds its book for weeks. Measured: a lock naming a live pid with the
    // recorded application name is refused while fresh, and TAKEN OVER once its file is
    // 45 s old — the earlier "a live holder's lock is never stale by age" claim was
    // wrong; that measurement was refused by the holder's flock on the file, which only
    // a local filesystem enforces, not by the pid check. With the age rule off a lock is
    // stale exactly when its pid is gone or the machine has rebooted (both free it at
    // once, TS-BOTSIM-009), and a live holder keeps it however old (also on a
    // filesystem without flock); the native lock is a second guard on top.
    std::unique_ptr<QLockFile> m_bookLock;
    bool m_ownsBook = false;
    QList<trading::DecisionRow> m_pendingRows;
    QList<ScreenerRow> m_pendingScan;
    QDateTime m_askedAt;                    // when the in-flight request went out
    bool m_aiPending = false;
    QString m_aiStatus;                     // last availability line, for the window
    // What load() found, emitted once the object graph is connected: a signal from
    // the CONSTRUCTOR reaches nobody, and "RESUMED ARMED" is precisely the line a
    // multi-day experiment must not lose. A book held by another process never says
    // "resumed" here (acquireBookLock rewrites it): the experiment continues THERE.
    QString m_restoreNote;
};

#endif // TRADINGAPP_UI_BOTSIMRUNNER_H
