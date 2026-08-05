#ifndef TRADINGAPP_DOMAIN_PAPERTRADER_H
#define TRADINGAPP_DOMAIN_PAPERTRADER_H

#include "domain/InstrumentCatalog.h"
#include "domain/Models.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

// The paper-trading bot's books and rules (REQ-F-029): a simulated account that
// trades the app's own composite decision across every instrument on LIVE market
// data, with simulated money. This module is the PURE half — sizing, the cost
// model, marking, and the entry/exit predicates over plain data. The ui layer owns
// the runner that feeds it quotes and signals.
//
// Two properties are deliberate and load-bearing:
//
//  * NO BROKER. Nothing here can reach the network or an order endpoint. The bot
//    cannot move real money because the code that would do it is not in its
//    reach — not because a flag is off.
//  * COSTS ARE CHARGED LIKE THE REAL PATH — ONCE. Half the live spread on opening
//    AND on closing (the identity the trade panel and the closed-trades view use),
//    plus per-night rollover from the instrument's own fee table with the tripled
//    weekend night. A simulation that skips the costs measures nothing: on the
//    stakes and leverage used here they are the difference between a strategy that
//    looks profitable and one that is.
//
//    Which is why fills and marks are at the MID, not at the ask/bid: paying half
//    the spread on top of a mid fill IS the ask fill (and marking a long at the mid
//    minus the closing charge IS the bid mark), but it keeps the transaction cost a
//    visible line item instead of hiding it inside the rates. Filling at the ask
//    AND charging the half-spread would bill the spread twice — the simulation
//    would then read pessimistically for a reason nobody could see in the numbers.
//
// Money is in EUR (the app's display currency) throughout. P/L uses the FX-free
// identity stake × leverage × (mark − open) / open, signed by the side — the same
// reasoning as trading::accountValuePerPoint: the account-currency notional moves
// 1:1 with the RELATIVE price change, so no quote-currency FX rate is needed.
namespace trading {

// How the simulator uses an AI advisor's proposal (REQ-F-030). An explicit,
// visible choice, because the three answer very different questions:
//   Off      the composite decides (REQ-F-029); a proposal is logged, not acted on
//   Confirm  only the proposal's instrument may be opened, and only while the
//            composite agrees with the model's side — the model is a VETO
//   Lead     the model's own pick and side are traded
// No setting lets the model past the risk rules; it only ever supplies direction.
enum class BotAiMode : qint8 { Off, Confirm, Lead };

[[nodiscard]] QString botAiModeWord(BotAiMode mode);

// A normalised proposal from an AI advisor — the local Ollama model or the cloud
// one; the simulator does not care which brain answered.
struct AiProposal {
    bool ok = false;          // a usable answer arrived (not: a tradable one)
    QString symbol;           // as the MODEL spelled it ("SPX500 composite" happens)
    QString resolvedSymbol;   // …mapped onto one of the app's instruments, or empty
    qint32 dir = 0;           // +1 BUY / −1 SELL / 0 HOLD or unreadable
    double confidence = 0.0;  // 0..100
    qint32 leverage = 0;      // what the model asked for (0 = unstated); still capped
    // The model asked for an OPEN position in this instrument to be closed (action
    // "CLOSE"/"EXIT"). Distinct from HOLD, which means "no action", and from the
    // opposite side, which means "close and reverse".
    bool exitNow = false;
    QString rationale;
    QString source;           // "ollama / qwen2.5:1.5b" — what the log line names
};

// Which of the app's instruments a proposal refers to, or an empty string when
// that is not clear. An exact (case-insensitive) match wins; otherwise the
// proposal must mention exactly ONE known symbol, so a chatty "SPX500 composite"
// still resolves while an ambiguous answer ("gold vs GoldMiners") or an unknown
// instrument resolves to nothing — and nothing is what gets traded then.
[[nodiscard]] QString matchProposalSymbol(const QString &proposalSymbol,
                                          const QStringList &known);

// The AI gate for one candidate instrument: may it be opened at all, and in which
// direction? `why` carries the refusal for the log — a skipped trade must always
// be explainable — and `code` is its stable, countable category, so a scan can
// report "18 x market closed, 5 x AI picked elsewhere" instead of nothing at all.
struct AiGate {
    bool allow = false;
    qint32 dir = 0;      // the direction to trade when allow (composite's, or the model's in Lead)
    qsizetype pick = -1; // index of the proposal that named this instrument, or -1
    QString why;
    QString code;        // "ai-none" | "ai-hold" | "ai-unknown-symbol" | "ai-other-pick" |
                         // "ai-disagree" | "composite-neutral" | "" when allowed
};

// The gate takes the model's WHOLE answer, not one pick: a model that sees three
// instruments worth trading should be able to say so, and the bot should be able
// to take all three (REQ-F-030) — the number of trades is limited by the risk
// budget, never by how many opinions fit in the protocol.
[[nodiscard]] AiGate paperAiGate(const QString &symbol, qint32 compositeDir,
                                 const QList<AiProposal> &proposals, BotAiMode mode);

// Leverage for a trade the model is leading: never more than the risk-derived
// `sized` value (the model cannot lever up past the risk budget — REQ-F-030), but
// a MORE cautious request is honoured, since in Lead mode it is the model's trade.
// Only applies in Lead; other modes keep the sized value. 0/absent = no request.
[[nodiscard]] qint32 paperLeverageWithAi(qint32 sized, qint32 asked, BotAiMode mode);

// How bold the bot is. The defaults are deliberately aggressive (REQ-F-029 asks
// for courage): a low confidence floor and a stake fraction that puts real
// weight behind each call, because the experiment is about measuring many
// trades, not about capital preservation. Every one of them is a guard rail, so
// none may be omitted: an unbounded paper account teaches nothing either.
struct BotConfig {
    double startEquity = 50000.0;      // EUR of simulated capital
    double stakeFraction = 0.06;       // TARGET stake per trade as a fraction of CURRENT equity
    double minStake = 100.0;           // …but never a stake too small to pay its costs
    // How many trades the bot may hold is NOT a policy number here: it takes as
    // many as it judges worthwhile and the RISK BUDGET below still permits, so this
    // is only a sanity bound (one position per instrument caps it in practice).
    qint32 maxOpenTrades = 40;
    // Σ stakes ≤ this × equity (margin, not notional). Deliberately below 1: an
    // account that commits every last euro as margin has no free margin left, which
    // a real broker answers with a margin call — and the opening costs are paid out
    // of cash ON TOP of the stakes, so a 1.0 cap drove cash NEGATIVE (measured: 17
    // trades, cash −7.69). Free margin is a feature, not slack.
    double maxExposureFraction = 0.75;
    // The real governor: the summed loss-if-every-stop-is-hit of all open trades
    // must stay within this fraction of equity. That is what "as many trades as it
    // should, at a decent risk level" means operationally — conviction decides the
    // COUNT, this decides the total exposure to being wrong.
    //
    // 20% with the 25%-per-trade budget below means room for ~0.8 × equity of stake
    // (~13 trades at the 6% target), which is deliberately LESS than the margin cap
    // above: the binding constraint has to be the risk one, or the bot would stop
    // for a bookkeeping reason ("no margin left") instead of a risk reason, and the
    // log would say the wrong thing about why it stopped.
    double maxPortfolioRiskFraction = 0.20;
    // …and no more than this fraction inside ONE correlation bucket, so the budget
    // above cannot be spent on a single market view (REQ-F-031).
    double maxGroupRiskFraction = 0.08;
    // …and no more than this fraction in ONE instrument. Adding to a position is
    // allowed (REQ-F-032) but it is the most concentrated thing the bot can do, so
    // it gets the tightest cap of the three.
    double maxSymbolRiskFraction = 0.03;
    // How many positions one instrument may hold. Beyond the first, every one of
    // them needs the model to name that instrument again.
    qint32 maxPositionsPerSymbol = 3;
    double minConfidence = 12.0;       // composite confidence floor to enter (0..100)
    qint32 leverageCap = 10;           // hard cap on top of the instrument's own
    double riskBudgetFraction = 0.25;  // loss at the stop ≤ this × stake (sets leverage)
    qint32 horizonHours = 24;          // intended holding horizon (drives the SL/TP geometry)
    qint32 maxHoldHours = 72;          // …and the hard exit if neither leg is touched
    double flipConfidence = 30.0;
    // A trade must be worth taking AFTER what it costs: the move to its target has
    // to be at least this multiple of the round trip (both half-spreads plus the
    // rollover to the horizon). 1.0 would mean "break even if it works perfectly".
    double minEdgeOverCost = 2.5;
    // The signal that justified the entry has faded to this fraction of its entry
    // strength → the reason to be in the trade is gone; a position that is not in
    // profit is let go rather than ridden to its stop.
    double signalFadeFraction = 0.45;
    // A position that was up and has given back this fraction of its best result is
    // closed while it is still a winner. Only above giveBackMinNet, so noise on a
    // few euros of profit does not churn the book.
    double giveBackFraction = 0.5;
    double giveBackMinNet = 30.0;      // an opposite call this strong closes the trade
    double minEquityFraction = 0.25;   // stop OPENING below this × start equity (ruin guard)
    // The DAY rules — the only honest form of "make 350 a day". A fixed daily
    // profit cannot be guaranteed by any strategy; what a rule CAN do is stop the
    // day once the target is made (so a made day is not given back) and stop it
    // once the loss limit is hit (so a bad day cannot become a bad month). Both are
    // no-ops for the positions already open: their stops and targets still govern.
    //
    // What is deliberately ABSENT is any size increase after a loss. Chasing a daily
    // number with a bigger stake is how accounts die, and the stake here scales with
    // CURRENT equity, so it can only shrink after losses.
    // Book the day when a single open winner already completes the target (see
    // CloseReason::DayTarget). Off = only stops, targets, flips and carry ever close
    // a trade, and a day is made only by what those happen to book.
    bool harvestForDailyTarget = true;
    double dailyProfitTarget = 350.0;  // EUR of realised net; 0 disables the rule
    double dailyLossLimit = 350.0;     // EUR of realised net loss; 0 disables the rule
    bool tradeWeekdaysOnly = true;     // Mon-Fri; the weekend has no session to trade
    // How an AI advisor's proposal is used (REQ-F-030). Off by default: turning a
    // local model into the decision maker is the user's explicit choice, never a
    // silent change of what the simulation measures.
    BotAiMode aiMode = BotAiMode::Off;

    [[nodiscard]] bool operator==(const BotConfig &other) const = default;
};

// One trading day's own ledger: what the bot actually booked today. The daily
// rules read this and nothing else, so "the day is made" is a fact about closed
// money, never about hoped-for open profit.
struct BotDay {
    QDate date;
    double realized = 0.0;  // net P/L of the trades CLOSED today, after all costs
    qint32 opened = 0;
    qint32 closed = 0;

    [[nodiscard]] bool operator==(const BotDay &other) const = default;
};

// Whether the day is still open for NEW trades, and if not, why.
enum class DayGate : qint8 { Open, TargetReached, LossLimitReached, Weekend };

[[nodiscard]] QString dayGateWord(DayGate gate);

// The day rule for `now` against today's ledger. Weekend first (there is no
// session to trade), then the target, then the loss limit. Open = keep trading.
// One open position as a harvest option: what it would BOOK if closed right now,
// net of the exit cost. The runner computes that from live spreads; the decision
// itself stays pure.
struct HarvestOption {
    qint64 id = 0;
    double netIfClosedNow = 0.0;
};

// The position to close so today's booked net reaches the target, or 0 for none.
// Picks the SMALLEST sufficient winner: the day gets banked with the least upside
// given up, and a position that is running well keeps running. Returns 0 when the
// rule is off, the target is already made (the day gate stops entries then), no
// single winner is enough, or nothing is in profit.
[[nodiscard]] qint64 paperHarvestPick(const QList<HarvestOption> &options, const BotDay &day,
                                      const BotConfig &cfg);

// One open position, as the model is shown it.
struct OpenPositionBrief {
    QString symbol;
    bool isBuy = true;
    double netPnl = 0.0;      // EUR, after the costs paid so far
    double heldHours = 0.0;
    double entryConfidence = 0.0;
};

// The section appended to the evidence prompt that asks the model to judge what is
// already OPEN, not just what to open next (REQ-F-032). Empty when nothing is open.
[[nodiscard]] QString paperHoldEvidence(const QList<OpenPositionBrief> &open);

// Should this position be closed because the model no longer believes in it?
// `close` only when the model said so EXPLICITLY — the opposite side for that
// instrument, or an explicit CLOSE. Silence is not a verdict: a local model
// regularly answers about two instruments out of twenty-six, and reading that as
// "close everything else" would liquidate the book on a slow answer.
struct HoldVerdict {
    bool close = false;
    QString why;
    QString code;   // "" | "ai-reversed" | "ai-close"
};
[[nodiscard]] DayGate paperDayGate(const BotDay &day, const QDateTime &now, const BotConfig &cfg);

// Why a simulated position closed. Recorded per trade: a strategy's exit mix is
// the most informative part of the result (all stops = the geometry is too tight,
// all max-hold = no conviction).
enum class CloseReason : qint8 {
    None,
    StopLoss,
    TakeProfit,
    SignalFlip,
    MaxHold,
    // The two CARRY exits: a leveraged position pays rent every night, and eToro
    // charges the Friday night three times. A trade whose remaining upside no
    // longer covers what holding it costs is a loss being paid for in instalments,
    // so the bot closes it instead of waiting for a target it cannot profitably
    // reach — and it does not pay the tripled weekend charge on a position that has
    // not earned it (which also sheds the weekend gap risk of REQ-F-011).
    CostsExceedEdge,
    WeekendCarry,
    // The model that opened it no longer wants it (REQ-F-032). Only ever on an
    // explicit contrary answer — never on the absence of one.
    AiExit,
    // The two DYNAMIC exits (REQ-F-032): the signal that justified the trade has
    // faded while the trade is not paying, and a winner that has handed back most
    // of its best result. Both exist because waiting for the stop or the target is
    // a decision to ignore everything that happens in between.
    SignalFade,
    GiveBack,
    // The day's target, booked. A machine aiming at a DAILY number has to realise
    // it: an unrealised gain is not a made day. This closes the SMALLEST open
    // winner that completes today's target, which truncates that one trade's upside
    // — a real, deliberate cost, paid to turn "the day is up" into "the day is
    // banked" (REQ-F-031). The biggest winner is left running.
    DayTarget,
    Manual,
    Reset
};

// One word for the reason, for the table and the log.
[[nodiscard]] QString closeReasonWord(CloseReason reason);

// ---------------------------------------------------------------------------
// Learning from the bot's own record (REQ-F-033)
// ---------------------------------------------------------------------------

// What was true about a trade WHEN IT WAS OPENED, as plain numbers. This is the
// input side of every training example the bot produces: the outcome (net after
// costs) is the label, and the pair is what a model can be fitted to.
//
// Captured at entry rather than reconstructed at exit on purpose — the market
// state that justified the trade is gone by the time it closes.
struct EntryFeatures {
    double confidence = 0.0;         // 0-100, the conviction the entry was taken on
    double volPct = 0.0;             // per-bar volatility at entry, %
    double stopPct = 0.0;            // stop distance from the fill, %
    double targetPct = 0.0;          // target distance from the fill, %
    double spreadPct = 0.0;          // live spread at entry, %
    double edgeOverCost = 0.0;       // gain at target / round-trip cost
    qint32 leverage = 1;
    qint32 dir = 1;                  // +1 long, −1 short
    qint32 hourUtc = 0;              // 0-23: sessions behave differently
    qint32 dayOfWeek = 1;            // 1 = Monday
    bool aiBacked = false;           // the model named this instrument
    [[nodiscard]] bool isValid() const { return volPct > 0.0; }
};

// The feature vector's names, in ONE canonical order. Both the JSONL the app
// writes and the model the trainer produces name their columns from this list, so
// a model can never be fed its inputs in the wrong order.
[[nodiscard]] QStringList entryFeatureNames();
// The values, in the same order as entryFeatureNames().
[[nodiscard]] QList<double> entryFeatureValues(const EntryFeatures &f);
// …and as a name→value map, which is how the network looks its inputs up.
[[nodiscard]] QHash<QString, double> entryFeatureMap(const EntryFeatures &f);

// An open simulated position. `stake` is the invested money the window shows per
// trade; the costs are carried on the trade itself so the closed row can state
// what the round trip actually cost.
struct PaperTrade {
    qint64 id = 0;
    QString symbol;
    qint64 instrumentId = 0;
    bool isBuy = true;
    double stake = 0.0;         // invested EUR (margin committed)
    qint32 leverage = 1;
    double openRate = 0.0;      // the mid at entry; the half-spread is charged separately
    double slRate = 0.0;        // 0 = no stop (never happens for a bot trade)
    double tpRate = 0.0;
    double openCost = 0.0;      // half-spread crossed on entry, EUR (already paid)
    double feesPaid = 0.0;      // rollover charged so far, EUR
    qint32 nightsCharged = 0;   // nights already billed (weekend counts triple)
    double markRate = 0.0;      // last mark (0 = not yet marked → openRate is used)
    // The best net result this position has shown, EUR. Updated on every mark and
    // persisted: the give-back exit is about what a trade WAS worth, so forgetting
    // it across a restart would silently switch that rule off.
    double peakNet = 0.0;
    bool markLive = false;      // the mark came from a live quote, not a stale row
    QDateTime openTime;
    QDateTime markTime;
    // Instant up to which rollover has been billed — a timestamp rather than a
    // counter so the charge stays correct across an app restart (the books are
    // persisted) and across a poll that was missed while the app was closed.
    QDateTime feesChargedTo;
    double entryConfidence = 0.0;
    QString entryBasis;
    // What the entry looked like, kept for the experience log (REQ-F-033).
    EntryFeatures features;         // the entry's own numbers, for the experience log

    // Notional the position controls (EUR): stake × leverage.
    [[nodiscard]] double notional() const;
    // Units held, in the instrument's own quote currency terms.
    [[nodiscard]] double units() const;
    // The rate the P/L is currently measured at (markRate, or openRate before the
    // first mark — a fresh trade shows 0.00, never a bogus swing).
    [[nodiscard]] double effectiveRate() const;
    // Gross P/L at the current mark, before any cost.
    [[nodiscard]] double grossPnl() const;
    // What the trade has cost so far: the spread paid on entry plus rollover.
    [[nodiscard]] double costsSoFar() const;
    // Loss if this position's stop is hit: notional × the stop's distance from the
    // open, as a fraction. 0 without a stop. The portfolio risk budget sums these.
    [[nodiscard]] double riskAtStop() const;
    // Gross P/L minus the costs incurred so far. The still-unpaid closing spread is
    // deliberately NOT subtracted here — it is charged at the close, and the window
    // shows it as part of the closed row's cost total.
    [[nodiscard]] double netPnl() const;
};

// A finished simulated round trip.
struct PaperClosedTrade {
    qint64 id = 0;
    QString symbol;
    bool isBuy = true;
    double stake = 0.0;
    qint32 leverage = 1;
    double openRate = 0.0;
    double closeRate = 0.0;
    QDateTime openTime;
    QDateTime closeTime;
    double grossPnl = 0.0;      // before costs
    double openCost = 0.0;      // half-spread on entry
    double closeCost = 0.0;     // half-spread on exit
    double feesPaid = 0.0;      // rollover over the holding time
    double netPnl = 0.0;        // gross − every cost above: what the account kept
    CloseReason reason = CloseReason::None;
    EntryFeatures features;     // the entry's own numbers, carried to the record

    [[nodiscard]] double totalCost() const;
    // Holding time in hours (0 when either stamp is invalid).
    [[nodiscard]] double heldHours() const;
};

// ---------------------------------------------------------------------------
// Cost and P/L model (pure)
// ---------------------------------------------------------------------------

// Units a stake controls at a rate: stake × leverage / rate. 0 for a
// non-positive rate — an unpriced instrument must not produce a position.
[[nodiscard]] double paperUnits(double stake, qint32 leverage, double rate);

// Half of the live spread, crossed once on opening and once on closing, on the
// position's notional: notional × (spreadPct / 100) / 2. spreadPct is the app's
// own (ask − bid)/mid × 100. An unknown spread (0) yields 0 — flagged by the
// caller rather than silently invented.
[[nodiscard]] double paperHalfSpreadCost(double stake, qint32 leverage, double spreadPct);

// Gross P/L in account currency: stake × leverage × (mark − open)/open, negated
// for a short. FX-free (see the file comment). 0 when either rate is unusable.
[[nodiscard]] double paperGrossPnl(double stake, qint32 leverage, double openRate,
                                   double markRate, bool isBuy);

// Rollover nights between two instants, charging a night per date boundary
// crossed, with a Friday→Monday crossing counted as THREE (eToro's tripled
// weekend rollover, REQ-F-013). Never negative.
[[nodiscard]] qint32 paperRolloverNights(const QDateTime &from, const QDateTime &to);

// Rollover cost in EUR for `nights` nights on this position, from the
// instrument's per-unit fee table (USD per unit per night; negative = a credit).
// eurPerUsd ≤ 0 leaves the figure in the fee table's own currency rather than
// inventing a rate.
[[nodiscard]] double paperRolloverCost(const PaperTrade &trade, const InstrumentFees &fees,
                                        qint32 nights, double eurPerUsd);

// ---------------------------------------------------------------------------
// Entry evaluation (pure)
// ---------------------------------------------------------------------------

// Everything the bot knows about one candidate instrument on one scan.
struct CandidateInput {
    QString symbol;
    qint64 instrumentId = 0;
    qint32 dir = 0;              // composite call: +1 BUY / −1 SELL / 0 neutral
    double confidence = 0.0;     // its confidence, 0..100
    QList<double> closes;        // recent (hourly) closes, oldest first — the volatility source
    // The live two-sided quote. The fill is priced at their MID and the crossing of
    // the spread is charged as an explicit cost (see the file comment), so both
    // sides are needed even though neither is the fill rate.
    double bid = 0.0;
    double ask = 0.0;
    double spreadPct = 0.0;      // live (ask − bid)/mid × 100; 0 = unknown
    qint32 maxLeverage = 0;      // instrument's own cap (0 = unknown)
    QList<qint32> leverageSteps; // offered multipliers, ascending (empty = the default ladder)
    bool marketOpen = false;     // inferred from the quote timestamp advancing
    bool quoteLive = false;      // the quote is fresh enough to trade off
    QDateTime now;
    // The instrument's rollover table and the FX rate, so the entry can price what
    // holding the trade to its horizon will cost. Unknown fees leave the carry out
    // of the sum (the spread alone is still charged) rather than inventing it.
    InstrumentFees fees;
    bool feesKnown = false;
    double eurPerUsd = 0.0;
    // The model named THIS instrument in its current answer. Only then may the bot
    // add to a position it already holds (REQ-F-032): stacking on the composite
    // alone would turn one opinion into three copies of the same trade.
    bool aiBacked = false;               // for the day rules (invalid = they are skipped)
};

// A fully specified simulated entry: the side, the price it fills at, the
// vol-derived geometry and the reason line the log and the trade carry.
struct EntrySignal {
    bool valid = false;          // false = not enough data to size a trade at all
    QString symbol;
    qint64 instrumentId = 0;
    bool isBuy = true;
    double fillRate = 0.0;       // the quote's mid (the spread is a separate charge)
    double spreadPct = 0.0;
    qint32 leverage = 1;
    double slRate = 0.0;
    double tpRate = 0.0;
    double confidence = 0.0;
    double volPct = 0.0;         // per-bar volatility the geometry came from
    QString basis;               // "composite BUY 34 conf, σ 0.42%/h, lev 5"
};

// Turn a candidate into a sized entry: volatility → stop distance (1.5σ over the
// horizon) → leverage (largest step keeping the loss at the stop inside the risk
// budget) → SL/TP rates (reward:risk 1.5) → the fill side. Pure and cheap on
// purpose: the bot evaluates every instrument on every scan, so this must not
// pull in the Monte-Carlo path (REQ-N-006).
[[nodiscard]] EntrySignal buildEntrySignal(const CandidateInput &in, const BotConfig &cfg);

// The book state an entry is judged against (plain data, so the gate stays pure).
// What the book already holds in ONE instrument. Several positions in the same
// symbol are perfectly correlated — the same bet, sized up — so they are governed
// by their own count and risk caps on top of the bucket and portfolio budgets.
struct SymbolExposure {
    qint32 count = 0;   // open positions in this instrument
    qint32 dir = 0;     // +1 long, -1 short, 0 none (all of them share one side)
    double risk = 0.0;  // Σ loss-at-stop in this instrument, EUR
};

struct BookState {
    double equity = 0.0;
    // Uninvested EUR. A stake can never exceed this: the simulated account may not
    // commit money it does not hold, and the opening cost comes out of it too.
    double cash = 0.0;
    double invested = 0.0;      // Σ open stakes (margin committed)
    // Σ loss-if-the-stop-is-hit over all open trades: what the account stands to
    // lose if every open position goes wrong. This — not a trade count — is what
    // limits how many trades the bot may hold (BotConfig::maxPortfolioRiskFraction).
    double openRisk = 0.0;
    qint32 openCount = 0;
    SymbolExposure symbol;      // the CANDIDATE's instrument (the caller fills it)
    BotDay day;                 // today's booked result, for the daily rules
    // Loss-at-stop already committed PER CORRELATION BUCKET (see correlationGroup).
    // Thirteen long index positions are one bet, not thirteen: without this the
    // portfolio budget is satisfied by a position count while the account carries a
    // single undiversified risk.
    QHash<QString, double> riskByGroup;
};

// Take it or skip it, and why — the "why" is what the log line states, so a
// refusal is never silent.
struct EntryVerdict {
    bool take = false;
    double stake = 0.0;         // the sized stake when take == true
    QString why;
    // Stable, countable category of the refusal, so one scan can summarise WHY it
    // opened nothing ("market-closed x18, confidence x5") instead of leaving the
    // window silent — which is indistinguishable from a broken bot.
    QString code;               // "market-closed" | "no-live-quote" | "no-signal" |
                                // "confidence" | "already-holding" | "trade-limit" |
                                // "ruin-guard" | "no-history" | "spread-unknown" |
                                // "risk-budget" | "margin-cap" | "cash" | "" when taken
};

// Stake for the next trade: the configured fraction of CURRENT equity (so the bot
// compounds and de-risks by itself), floored at minStake and clamped to whatever
// room is LEFT — both in margin (maxExposureFraction) and, decisively, in the
// portfolio risk budget (maxPortfolioRiskFraction). 0 = no room for a meaningful
// trade, which is how "as many trades as it should" ends: not at a count, but when
// the next one would push the account past the risk it is allowed to carry.
//
// riskPerStake is this candidate's loss-at-stop per euro of stake — leverage ×
// stop distance as a fraction — i.e. what buildEntrySignal's geometry implies
// (paperEntrySignalRisk). 0 or less falls back to the per-trade risk budget.
[[nodiscard]] double paperStakeFor(const BookState &book, const BotConfig &cfg,
                                   double riskPerStake);

// The same sizing, but naming WHICH limit bound it — the scan summary reports the
// binding limit per refusal, and reporting the wrong one is worse than reporting
// none: it sends the reader to tune the wrong knob.
// The correlation bucket a symbol belongs to, for risk aggregation: instruments
// in the same bucket move together closely enough that holding several of them is
// one position in disguise. Derived from the instrument catalog's group, with the
// documented exceptions where that group mixes unlike things (the dollar index is
// an FX bet, gold and silver track each other more than they track oil). Unknown
// symbols get their own bucket rather than sharing one, which is the conservative
// reading: it never lets an unrecognised name inflate an existing bucket.
[[nodiscard]] QString correlationGroup(const QString &symbol);

struct StakeRoom {
    double stake = 0.0;
    // "" when the target stake fit, else the binding limit:
    // "risk-budget" | "group-risk" | "margin-cap" | "cash"
    QString limit;
};

// `symbol` adds the candidate's correlation-bucket cap to the answer; empty asks
// the portfolio-level question only.
[[nodiscard]] StakeRoom paperStakeRoom(const BookState &book, const BotConfig &cfg,
                                       double riskPerStake, const QString &symbol = QString());

// Loss-at-stop per euro of stake implied by a sized entry: leverage × the stop's
// distance from the fill, as a fraction. The risk a trade contributes per euro
// committed, which is what the portfolio budget above is measured in.
[[nodiscard]] double paperEntrySignalRisk(const EntrySignal &signal);

// The full entry gate: market open, quote live, a signal at all, confidence over
// the floor, a valid sizing, room under the trade/exposure/equity limits, and not
// already in this instrument.
[[nodiscard]] EntryVerdict paperEntryVerdict(const CandidateInput &in, const EntrySignal &sig,
                                              const BookState &book, const BotConfig &cfg);

// What one candidate is worth after what it costs, at the stake it would be sized
// to: the gross gain if the target is reached, the round trip that has to be paid
// for out of it (both half-spreads, plus the rollover to the horizon when the fee
// table is known), and their ratio. A trade whose ratio is below 1 cannot make
// money even when it works perfectly.
struct EntryEconomics {
    double gainAtTarget = 0.0;   // EUR
    double cost = 0.0;           // EUR
    double ratio = 0.0;          // gainAtTarget / cost (0 when the cost is unknown)
};
[[nodiscard]] EntryEconomics paperEntryEconomics(const EntrySignal &sig, double stake,
                                                 const CandidateInput &in, const BotConfig &cfg);

// ---------------------------------------------------------------------------
// Exit evaluation (pure)
// ---------------------------------------------------------------------------

[[nodiscard]] HoldVerdict paperAiHold(const PaperTrade &trade, const QList<AiProposal> &proposals,
                                      BotAiMode mode);


// The live read an open position is judged against between scans (the exit-side
// counterpart of CandidateInput).
struct ExitContext {
    double markRate = 0.0;   // current mark; 0 = unknown, so the barriers are not tested
    qint32 dirNow = 0;       // newest composite call for this instrument, ±1/0
    double confNow = 0.0;    // its confidence, 0..100
    QDateTime now;           // "now" for the holding-time limit (invalid = skip it)
    // What holding on would COST, so the exit can be an economic decision and not
    // only a price one. Without known fees the carry rules stay silent rather than
    // guessing (feesKnown false), exactly as the entry gate refuses an unknown
    // spread instead of pricing it at zero.
    double spreadPct = 0.0;  // live spread, for the closing half-spread
    InstrumentFees fees;     // per-unit per-night rollover (negative = a credit)
    bool feesKnown = false;
    double eurPerUsd = 0.0;  // 0 = unknown; the fee then stays in its own currency
};

// What is still to be won if the take-profit is reached, in EUR: notional × the
// remaining distance to the target as a fraction of the open rate. 0 when there is
// no target, or when the mark is already past it (the barrier rule owns that case).
[[nodiscard]] double paperRemainingUpside(const PaperTrade &trade, double markRate);

// What holding this position until `until` will cost in EUR: the rollover nights in
// between (with eToro's tripled weekend night, via paperRolloverNights) plus the
// half-spread it must still pay to get out. A carry CREDIT makes this smaller, and
// can make it negative — being paid to hold is not a reason to close.
[[nodiscard]] double paperCostToHold(const PaperTrade &trade, const ExitContext &ctx,
                                     const QDateTime &until);

// Is the next rollover this position pays the TRIPLED weekend one? True when the
// next date boundary crossed from `now` starts a Saturday.
[[nodiscard]] bool paperWeekendChargeAhead(const QDateTime &now);

// Whether this position should close now, and why: its stop-loss or take-profit
// rate touched, the carry no longer covered by the remaining upside, the tripled
// weekend charge not earned, the composite flipping against it with conviction, or
// the maximum holding time expired. CloseReason::None = hold.
[[nodiscard]] CloseReason paperCloseDecision(const PaperTrade &trade, const ExitContext &ctx,
                                              const BotConfig &cfg);

// ---------------------------------------------------------------------------
// The books
// ---------------------------------------------------------------------------

// Aggregate result of the experiment so far — what the window's header states.
struct PaperStats {
    double startEquity = 0.0;
    double cash = 0.0;          // uninvested EUR
    double invested = 0.0;      // Σ open stakes
    double openPnl = 0.0;       // Σ open net P/L (gross − costs so far)
    double realized = 0.0;      // Σ closed net P/L
    double equity = 0.0;        // startEquity + realized + openPnl
    double totalPnl = 0.0;      // equity − startEquity
    double totalPnlPct = 0.0;
    qint32 openTrades = 0;
    qint32 closedTrades = 0;
    qint32 wins = 0;
    qint32 losses = 0;
    double winRate = 0.0;       // wins / closed, 0..100
    double costsPaid = 0.0;     // every spread and rollover charge the account has paid
    double bestTrade = 0.0;
    double worstTrade = 0.0;
};

// What the closed-trade record actually says about the strategy. These are the
// figures that decide whether the bot has any business touching real money — not
// an impression of "it seems to work", and not the open positions' hoped-for P/L.
struct PaperPerformance {
    qint32 closedTrades = 0;
    qint32 tradingDays = 0;      // distinct dates on which something closed
    double netTotal = 0.0;       // after every cost
    double netPerDay = 0.0;      // netTotal / tradingDays
    double grossWins = 0.0;      // Σ positive nets
    double grossLosses = 0.0;    // Σ |negative nets|
    double profitFactor = 0.0;   // grossWins / grossLosses; 0 when there are no losses yet
    double expectancy = 0.0;     // net per closed trade
    double winRate = 0.0;        // 0..100
    double maxDrawdown = 0.0;    // deepest peak-to-trough of the closed-trade curve, EUR
    double maxDrawdownPct = 0.0; // …as a percentage of the starting equity
    qint32 daysAtTarget = 0;     // days whose booked net reached the daily target
    double targetHitRate = 0.0;  // daysAtTarget / tradingDays, 0..100
    // "Profit on a daily basis, or over a few days": the rolling net of the most
    // recent kRollingDays trading days, which is the honest way to judge a strategy
    // that cannot be positive every single day.
    double netLastDays = 0.0;
    qint32 rollingDays = 0;      // how many days that figure actually covers
    qint32 shortTrades = 0;      // closed SELL trades — the bot trades both sides
    double shortNet = 0.0;       // …and what they contributed, so a one-sided
    double longNet = 0.0;        // …edge is visible instead of assumed
    double costsPaid = 0.0;      // every spread and rollover the record paid
};

// Trading days the rolling window covers ("a few days").
constexpr qint32 kRollingDays = 5;

// Measured over the closed trades in the order they closed. `dailyTarget` is what
// a day has to book to count towards daysAtTarget.
[[nodiscard]] PaperPerformance paperPerformance(const QList<PaperClosedTrade> &closed,
                                                double startEquity, double dailyTarget);

// The evidence a paper record must show before the bot may trade REAL money. Every
// threshold is a number someone can argue with — which is the point: the decision
// is explicit and inspectable instead of a feeling.
struct LiveGateConfig {
    qint32 minClosedTrades = 200;    // a sample, not a lucky afternoon
    qint32 minTradingDays = 20;      // …spread over enough different days
    double minNetTotal = 0.0;        // it has to have MADE money after costs
    double minProfitFactor = 1.20;   // winners must out-earn losers by a margin
    double maxDrawdownPct = 15.0;    // …without a hole the account could not survive
    double minExpectancy = 0.0;      // and the average trade must be positive
};

// ready = every condition met. `blockers` lists each unmet one in plain words, so
// the window can say exactly what is still missing rather than just "no".
struct LiveReadiness {
    bool ready = false;
    QStringList blockers;
};

[[nodiscard]] LiveReadiness paperLiveReadiness(const PaperPerformance &perf,
                                                const LiveGateConfig &gate);

// The simulated account: cash, open positions, closed trades. Deterministic and
// self-contained — the runner supplies rates and decisions, this owns the money.
//
// Accounting (so the numbers reconcile): opening a trade takes stake + opening
// cost out of cash; the stake comes back at the close together with the gross
// P/L, minus the closing cost and the rollover accrued. Hence
// equity == startEquity + realised + Σ(open gross − open costs so far) at all
// times, which is exactly what stats() reports.
class PaperBook
{
public:
    explicit PaperBook(BotConfig cfg = {});

    // Back to the starting capital, no positions, no history.
    void reset();
    void setConfig(const BotConfig &cfg);
    [[nodiscard]] const BotConfig &config() const & { return m_cfg; }

    [[nodiscard]] const QList<PaperTrade> &openTrades() const & { return m_open; }
    [[nodiscard]] const QList<PaperClosedTrade> &closedTrades() const & { return m_closed; }
    [[nodiscard]] BookState state() const;
    // What is already open in ONE instrument — the per-candidate half of BookState,
    // which state() cannot answer because it does not know the candidate.
    [[nodiscard]] SymbolExposure exposureFor(const QString &symbol) const;
    // Today's ledger (an empty one for a date the book has not booked into yet).
    [[nodiscard]] const BotDay &day() const & { return m_day; }
    [[nodiscard]] PaperStats stats() const;

    // Open a simulated position for a sized entry. Returns its id, or 0 when the
    // entry cannot be filled (no rate). Charges the opening half-spread at once.
    qint64 open(const EntrySignal &sig, double stake, const QDateTime &now);

    // Attach the entry's feature vector to a position just opened. Separate from
    // open() so the pure sizing path stays free of the learning machinery — and so
    // a build without it simply records nothing (REQ-F-033).
    void setFeatures(qint64 id, const EntryFeatures &features);

    // Re-mark one position from its instrument's current close rate (bid for a
    // long, ask for a short). Unknown ids are ignored.
    void mark(qint64 id, double closeRate, bool live, const QDateTime &at);

    // Charge any rollover nights that have passed since the last charge.
    void accrueRollover(qint64 id, const InstrumentFees &fees, double eurPerUsd,
                        const QDateTime &now);

    // Close a position at `closeRate`, charging the closing half-spread from
    // `spreadPct`. Returns the closed record; its id is 0 when nothing matched.
    PaperClosedTrade close(qint64 id, double closeRate, double spreadPct, CloseReason reason,
                           const QDateTime &now);

    // Persistence (REQ-F-029: an experiment runs over days, not one session).
    // Qt Core only — the runner decides WHERE this lands.
    [[nodiscard]] QJsonObject toJson() const;
    // Restores cash, positions and history. false = the payload was unusable, in
    // which case the book is left untouched rather than half-loaded.
    bool fromJson(const QJsonObject &obj);

private:
    // Index of `id` in m_open, or -1. Positions are few (maxOpenTrades), so a
    // linear scan is both the simplest and the fastest thing here.
    [[nodiscard]] qsizetype indexOf(qint64 id) const;

    BotConfig m_cfg;
    // Today's booked result. Rolled over on the first close of a new date, so the
    // daily target/limit always judge TODAY (and a restart mid-day resumes it).
    BotDay m_day;
    double m_cash = 0.0;
    double m_realized = 0.0;
    double m_costsPaid = 0.0;
    qint64 m_nextId = 1;
    QList<PaperTrade> m_open;
    QList<PaperClosedTrade> m_closed;
};

} // namespace trading

#endif // TRADINGAPP_DOMAIN_PAPERTRADER_H
