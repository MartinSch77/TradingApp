// Unit tests for the paper-trading bot's books and rules (DES-DOM-PAPER).
//
// The point of these is that the SIMULATION cannot flatter itself: the cost
// model, the accounting identity and the entry/exit gates are all checked
// against hand-computed figures.

#include "domain/PaperTrader.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {

// A candidate with everything in place: an open market, a live quote, a decent
// composite BUY and enough hourly history to derive a stop from. Cases below
// start from this and break exactly one thing.
CandidateInput goodCandidate()
{
    CandidateInput in;
    in.symbol = QStringLiteral("SPX500");
    in.instrumentId = 27;
    in.dir = 1;
    in.confidence = 40.0;
    in.bid = 5000.0;
    in.ask = 5001.0;
    in.spreadPct = 0.02;
    in.maxLeverage = 20;
    in.leverageSteps = {1, 2, 5, 10, 20};
    in.marketOpen = true;
    in.quoteLive = true;
    // A gently rising series with a little noise: a plausible, non-degenerate
    // volatility (a constant series would give sigma 0 and a floored stop).
    for (int i = 0; i < 40; ++i) {
        in.closes.append(4900.0 + (i * 2.0) + ((i % 3 == 0) ? 6.0 : -4.0));
    }
    return in;
}

BookState freshBook()
{
    BookState st;
    st.equity = 50000.0;
    st.cash = 50000.0;
    st.invested = 0.0;
    st.openCount = 0;
    return st;
}

// The live read an exit is judged against, spelled out per case below.
trading::ExitContext exitAt(double markRate, qint32 dir, double conf, const QDateTime &now)
{
    trading::ExitContext ctx;
    ctx.markRate = markRate;
    ctx.dirNow = dir;
    ctx.confNow = conf;
    ctx.now = now;
    return ctx;
}

// A trade as PaperBook::open would have created it, without needing the book.
PaperTrade tradeAt(double openRate, bool isBuy, double stake = 3000.0, qint32 leverage = 5)
{
    PaperTrade t;
    t.id = 1;
    t.symbol = QStringLiteral("SPX500");
    t.isBuy = isBuy;
    t.stake = stake;
    t.leverage = leverage;
    t.openRate = openRate;
    t.markRate = openRate;
    t.openTime = QDateTime(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);
    t.feesChargedTo = t.openTime;
    return t;
}

} // namespace

class TestPaperTrader : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-PAPER-001 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_001_costModelChargesHalfSpreadAndFxFreePnl()
    {
        // Notional 3000 x 5 = 15 000; a 0.02% spread costs half of that on the
        // notional = 15 000 x 0.0002 / 2 = 1.50 per side.
        QCOMPARE(paperHalfSpreadCost(3000.0, 5, 0.02), 1.5);
        // An unknown spread charges nothing (and the entry gate refuses such a
        // trade rather than pretending the cost is zero — TS-PAPER-004).
        QCOMPARE(paperHalfSpreadCost(3000.0, 5, 0.0), 0.0);
        // Units: stake x leverage / rate — the quote-currency size.
        QCOMPARE(paperUnits(3000.0, 5, 5000.0), 3.0);
        QCOMPARE(paperUnits(3000.0, 5, 0.0), 0.0);

        // P/L is the FX-free identity: a +1% move on a 15 000 notional is +150,
        // and the same move against a short is −150.
        QCOMPARE(paperGrossPnl(3000.0, 5, 5000.0, 5050.0, true), 150.0);
        QCOMPARE(paperGrossPnl(3000.0, 5, 5000.0, 5050.0, false), -150.0);
        QCOMPARE(paperGrossPnl(3000.0, 5, 0.0, 5050.0, true), 0.0);
    }

    //! @tstid TS-PAPER-002 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_002_rolloverNightsCountWeekendTriple()
    {
        const QDateTime tue(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);   // Tuesday
        // Same day: nothing rolled over yet.
        QCOMPARE(paperRolloverNights(tue, tue.addSecs(3600)), 0);
        // Into Wednesday: one ordinary night.
        QCOMPARE(paperRolloverNights(tue, tue.addDays(1)), 1);
        // Tuesday -> Saturday crosses Tue/Wed, Wed/Thu, Thu/Fri and the FRIDAY
        // night, which eToro charges three times: 3 + 3 = 6.
        QCOMPARE(paperRolloverNights(tue, tue.addDays(4)), 6);
        // Invalid or reversed inputs never charge.
        QCOMPARE(paperRolloverNights(QDateTime(), tue), 0);
        QCOMPARE(paperRolloverNights(tue, tue.addDays(-1)), 0);

        // The fee is per unit per night, and a negative table entry stays a
        // CREDIT rather than becoming a charge.
        PaperTrade t = tradeAt(5000.0, true);  // 3 units
        InstrumentFees fees;
        fees.buyOvernight = 0.5;
        fees.sellOvernight = -0.2;
        QCOMPARE(paperRolloverCost(t, fees, 2, 1.0), 3.0);       // 0.5 x 3 units x 2 nights
        QCOMPARE(paperRolloverCost(t, fees, 2, 0.5), 1.5);       // …converted to EUR
        t.isBuy = false;
        QVERIFY(paperRolloverCost(t, fees, 1, 1.0) < 0.0);       // the short earns carry
        QCOMPARE(paperRolloverCost(t, fees, 0, 1.0), 0.0);
        QCOMPARE(paperRolloverCost(t, InstrumentFees{}, 2, 1.0), 0.0);  // fees unknown
    }

    //! @tstid TS-PAPER-003 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_003_entrySignalGeometryAndFillSide()
    {
        const BotConfig cfg;
        const EntrySignal buy = buildEntrySignal(goodCandidate(), cfg);
        QVERIFY(buy.valid);
        QVERIFY(buy.isBuy);
        // Priced at the MID of 5000/5001, because the crossing of the spread is
        // charged as its own cost — pricing the fill at the ask as well would bill
        // that half-spread twice (see the header's cost-model note).
        QCOMPARE(buy.fillRate, 5000.5);
        QVERIFY(buy.volPct > 0.0);
        QVERIFY(buy.slRate < buy.fillRate);       // stop below, target above
        QVERIFY(buy.tpRate > buy.fillRate);
        // Reward:risk 1.5 — the target is 1.5x the stop distance away.
        const double risk = buy.fillRate - buy.slRate;
        const double reward = buy.tpRate - buy.fillRate;
        QVERIFY(qAbs((reward / risk) - 1.5) < 1e-9);
        QVERIFY(buy.leverage >= 1);
        QVERIFY(buy.leverage <= cfg.leverageCap);  // the hard cap holds
        QVERIFY(!buy.basis.isEmpty());             // the log line exists

        CandidateInput sellIn = goodCandidate();
        sellIn.dir = -1;
        const EntrySignal sell = buildEntrySignal(sellIn, cfg);
        QVERIFY(sell.valid);
        QVERIFY(!sell.isBuy);
        QCOMPARE(sell.fillRate, 5000.5);           // the same mid, either side
        QVERIFY(sell.slRate > sell.fillRate);      // …and its geometry mirrors
        QVERIFY(sell.tpRate < sell.fillRate);

        // A neutral call, an unpriced instrument and too little history are all
        // "cannot size a trade", never a guess.
        CandidateInput neutral = goodCandidate();
        neutral.dir = 0;
        QVERIFY(!buildEntrySignal(neutral, cfg).valid);
        // A one-sided quote is no quote: without both sides there is no mid to
        // price at and no spread to charge.
        CandidateInput unpriced = goodCandidate();
        unpriced.ask = 0.0;
        QVERIFY(!buildEntrySignal(unpriced, cfg).valid);
        CandidateInput noBid = goodCandidate();
        noBid.bid = 0.0;
        QVERIFY(!buildEntrySignal(noBid, cfg).valid);
        CandidateInput shortSeries = goodCandidate();
        shortSeries.closes = {100.0, 101.0, 100.5};
        QVERIFY(!buildEntrySignal(shortSeries, cfg).valid);
    }

    //! @tstid TS-PAPER-004 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_004_entryGateRefusesWithAReasonPerCase()
    {
        const BotConfig cfg;
        const CandidateInput good = goodCandidate();
        const EntrySignal sig = buildEntrySignal(good, cfg);

        const EntryVerdict take = paperEntryVerdict(good, sig, freshBook(), cfg);
        QVERIFY(take.take);
        QVERIFY(take.stake > 0.0);
        // 6% of 50 000 equity, and well inside the exposure cap.
        QCOMPARE(take.stake, 3000.0);

        struct Case {
            const char *what;
            CandidateInput in;
            BookState book;
        };
        CandidateInput closed = good;
        closed.marketOpen = false;
        CandidateInput stale = good;
        stale.quoteLive = false;
        CandidateInput neutral = good;
        neutral.dir = 0;
        CandidateInput timid = good;
        timid.confidence = cfg.minConfidence - 1.0;
        CandidateInput noSpread = good;
        noSpread.spreadPct = 0.0;

        BookState holding = freshBook();
        holding.symbol = SymbolExposure{1, 1, 0.0};   // one long already open in it
        BookState full = freshBook();
        full.openCount = cfg.maxOpenTrades;
        BookState ruined = freshBook();
        ruined.equity = cfg.startEquity * cfg.minEquityFraction;
        BookState maxedOut = freshBook();
        maxedOut.invested = freshBook().equity * cfg.maxExposureFraction;

        const QList<Case> cases = {
            {"market closed", closed, freshBook()},
            {"stale quote", stale, freshBook()},
            {"no call", neutral, freshBook()},
            {"below the confidence floor", timid, freshBook()},
            {"spread unknown", noSpread, freshBook()},
            {"already holding", good, holding},
            {"trade limit", good, full},
            {"ruin guard", good, ruined},
            {"exposure cap", good, maxedOut},
        };
        QSet<QString> codes;
        for (const Case &c : cases) {
            const EntrySignal s = buildEntrySignal(c.in, cfg);
            const EntryVerdict v = paperEntryVerdict(c.in, s, c.book, cfg);
            QVERIFY2(!v.take, c.what);
            QVERIFY2(!v.why.isEmpty(), c.what);   // every refusal is explainable…
            QVERIFY2(!v.code.isEmpty(), c.what);  // …and countable, for the scan summary
            static_cast<void>(codes.insert(v.code));
        }
        // Each case has its OWN category, so a scan summary can tell them apart
        // instead of reporting one lump of "skipped".
        QCOMPARE(codes.size(), cases.size());
    }

    //! @tstid TS-PAPER-005 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_005_exitRulesStopTargetFlipAndMaxHold()
    {
        const BotConfig cfg;
        PaperTrade t = tradeAt(5000.0, true);
        t.slRate = 4950.0;
        t.tpRate = 5075.0;
        const QDateTime soon = t.openTime.addSecs(3600);

        // Holding: inside the barriers, signal still on side, well inside the hold.
        QCOMPARE(paperCloseDecision(t, exitAt(5010.0, 1, 80.0, soon), cfg), CloseReason::None);
        QCOMPARE(paperCloseDecision(t, exitAt(4950.0, 1, 80.0, soon), cfg), CloseReason::StopLoss);
        QCOMPARE(paperCloseDecision(t, exitAt(5075.0, 1, 80.0, soon), cfg),
                 CloseReason::TakeProfit);
        // An unknown mark tests no barrier at all rather than guessing one.
        QCOMPARE(paperCloseDecision(t, exitAt(0.0, 1, 80.0, soon), cfg), CloseReason::None);
        // A gap past BOTH legs is read as the loss: the path between two marks is
        // unknown, so the simulation must not award itself the better outcome.
        PaperTrade gapped = t;
        gapped.tpRate = 4960.0;  // target below the stop (contrived), both touched
        QCOMPARE(paperCloseDecision(gapped, exitAt(4940.0, 1, 80.0, soon), cfg),
                 CloseReason::StopLoss);

        // The composite flipping against the trade, but only with conviction.
        QCOMPARE(paperCloseDecision(t, exitAt(5010.0, -1, cfg.flipConfidence, soon), cfg),
                 CloseReason::SignalFlip);
        QCOMPARE(paperCloseDecision(t, exitAt(5010.0, -1, cfg.flipConfidence - 1.0, soon), cfg),
                 CloseReason::None);
        QCOMPARE(paperCloseDecision(t, exitAt(5010.0, 0, 90.0, soon), cfg), CloseReason::None);

        // The hard holding limit.
        const QDateTime late = t.openTime.addSecs((cfg.maxHoldHours * 3600) + 1);
        QCOMPARE(paperCloseDecision(t, exitAt(5010.0, 1, 80.0, late), cfg), CloseReason::MaxHold);

        // A short mirrors the barrier sides.
        PaperTrade s = tradeAt(5000.0, false);
        s.slRate = 5050.0;
        s.tpRate = 4925.0;
        QCOMPARE(paperCloseDecision(s, exitAt(5050.0, -1, 80.0, soon), cfg), CloseReason::StopLoss);
        QCOMPARE(paperCloseDecision(s, exitAt(4925.0, -1, 80.0, soon), cfg),
                 CloseReason::TakeProfit);
        QCOMPARE(paperCloseDecision(s, exitAt(4990.0, 1, cfg.flipConfidence, soon), cfg),
                 CloseReason::SignalFlip);
    }

    //! @tstid TS-PAPER-006 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_006_bookAccountingReconcilesToTheCent()
    {
        BotConfig cfg;
        cfg.startEquity = 50000.0;
        PaperBook book(cfg);
        QCOMPARE(book.stats().equity, 50000.0);
        QCOMPARE(book.stats().cash, 50000.0);

        const QDateTime t0(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);
        EntrySignal sig = buildEntrySignal(goodCandidate(), cfg);
        // Pin the fill and the leverage so every figure below is exact arithmetic:
        // notional 15 000, 3.0 units. (The geometry itself is TS-PAPER-003's job.)
        sig.fillRate = 5000.0;
        sig.leverage = 5;
        const qint64 id = book.open(sig, 3000.0, t0);
        QVERIFY(id > 0);

        // Opening cost: notional 15 000 x 0.02% / 2 = 1.50, out of cash at once.
        const PaperTrade &t = book.openTrades().constFirst();
        QCOMPARE(t.openCost, 1.5);
        QCOMPARE(book.stats().cash, 50000.0 - 3000.0 - 1.5);
        QCOMPARE(book.stats().invested, 3000.0);
        // The spread is a real loss the moment the trade is on.
        QCOMPARE(book.stats().equity, 50000.0 - 1.5);

        // Mark it 1% up: +150 gross on the notional.
        book.mark(id, t.openRate * 1.01, true, t0.addSecs(60));
        QVERIFY(qAbs(book.stats().openPnl - (150.0 - 1.5)) < 1e-6);
        QVERIFY(qAbs(book.stats().equity - (50000.0 + 150.0 - 1.5)) < 1e-6);

        // Two nights of rollover at 0.5 USD per unit (3 units), EUR/USD 1.0.
        InstrumentFees fees;
        fees.buyOvernight = 0.5;
        book.accrueRollover(id, fees, 1.0, t0.addDays(2));
        QVERIFY(qAbs(book.openTrades().constFirst().feesPaid - 3.0) < 1e-9);

        // Close it back at the open rate: the gross is 0, so the account is out
        // exactly the two spreads plus the rollover.
        const PaperClosedTrade done =
            book.close(id, t.openRate, sig.spreadPct, CloseReason::TakeProfit, t0.addDays(2));
        QCOMPARE(done.openCost, 1.5);
        QCOMPARE(done.closeCost, 1.5);
        QVERIFY(qAbs(done.feesPaid - 3.0) < 1e-9);
        QVERIFY(qAbs(done.netPnl - (-6.0)) < 1e-9);
        QVERIFY(qAbs(done.totalCost() - 6.0) < 1e-9);
        QCOMPARE(done.reason, CloseReason::TakeProfit);

        const PaperStats s = book.stats();
        QVERIFY(book.openTrades().isEmpty());
        QCOMPARE(s.closedTrades, 1);
        QVERIFY(qAbs(s.realized - (-6.0)) < 1e-9);
        // The two ways of stating equity agree: cash-based (what the account
        // holds) and P/L-based (start + realised + open). This is the invariant
        // that keeps the window's numbers from contradicting each other.
        QVERIFY(qAbs(s.equity - (50000.0 - 6.0)) < 1e-9);
        QVERIFY(qAbs(s.equity - (s.startEquity + s.realized + s.openPnl)) < 1e-9);
        QVERIFY(qAbs(s.cash - s.equity) < 1e-9);  // nothing invested any more
        QVERIFY(qAbs(s.costsPaid - 6.0) < 1e-9);
        QCOMPARE(s.losses, 1);
        QCOMPARE(s.wins, 0);
        QCOMPARE(s.winRate, 0.0);

        // …and a reset really is a fresh experiment.
        book.reset();
        QCOMPARE(book.stats().equity, 50000.0);
        QVERIFY(book.closedTrades().isEmpty());
    }

    //! @tstid TS-PAPER-007 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_007_booksSurviveASaveLoadRoundTrip()
    {
        BotConfig cfg;
        cfg.startEquity = 50000.0;
        PaperBook book(cfg);
        const QDateTime t0(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);
        const EntrySignal sig = buildEntrySignal(goodCandidate(), cfg);
        const qint64 keep = book.open(sig, 3000.0, t0);
        const qint64 gone = book.open(sig, 2000.0, t0);
        book.mark(keep, 5100.0, true, t0.addSecs(60));
        static_cast<void>(book.close(gone, 5100.0, 0.02, CloseReason::SignalFlip, t0.addSecs(120)));

        const QJsonObject saved = book.toJson();
        const PaperStats before = book.stats();

        PaperBook restored(cfg);
        QVERIFY(restored.fromJson(saved));
        const PaperStats after = restored.stats();
        QCOMPARE(after.openTrades, before.openTrades);
        QCOMPARE(after.closedTrades, before.closedTrades);
        QVERIFY(qAbs(after.equity - before.equity) < 1e-9);
        QVERIFY(qAbs(after.realized - before.realized) < 1e-9);
        QVERIFY(qAbs(after.cash - before.cash) < 1e-9);
        QCOMPARE(restored.openTrades().constFirst().symbol, QStringLiteral("SPX500"));
        QCOMPARE(restored.closedTrades().constFirst().reason, CloseReason::SignalFlip);
        // A new id must not collide with a restored one.
        QVERIFY(restored.open(sig, 1000.0, t0.addSecs(200)) > keep);

        // An unknown schema leaves the books untouched instead of half-loading.
        QJsonObject alien = saved;
        alien.insert(QStringLiteral("schema"), 99);
        PaperBook other(cfg);
        QVERIFY(!other.fromJson(alien));
        QCOMPARE(other.stats().equity, 50000.0);
        QVERIFY(other.openTrades().isEmpty());
    }

    //! @tstid TS-PAPER-008 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_008_stakeCompoundsWithEquityAndRespectsTheCap()
    {
        const BotConfig cfg;  // 6% of equity, 60% exposure cap, 100 minimum
        BookState st = freshBook();
        QCOMPARE(paperStakeFor(st, cfg, cfg.riskBudgetFraction), 3000.0);

        st.equity = 80000.0;  // a winning run sizes up…
        QCOMPARE(paperStakeFor(st, cfg, cfg.riskBudgetFraction), 4800.0);
        st.equity = 20000.0;  // …and a losing one sizes down, with no extra rule
        QCOMPARE(paperStakeFor(st, cfg, cfg.riskBudgetFraction), 1200.0);

        // The exposure cap clips the last trade instead of exceeding the cap.
        st = freshBook();
        st.invested = (st.equity * cfg.maxExposureFraction) - 500.0;
        st.cash = st.equity - st.invested;   // cash and margin always move together
        st.openRisk = 0.0;  // …with the risk budget out of the way for this case
        QCOMPARE(paperStakeFor(st, cfg, cfg.riskBudgetFraction), 500.0);
        // …and refuses a stake too small to be worth its costs.
        st.invested = (st.equity * cfg.maxExposureFraction) - (cfg.minStake / 2.0);
        st.cash = st.equity - st.invested;
        QCOMPARE(paperStakeFor(st, cfg, cfg.riskBudgetFraction), 0.0);

        // A tiny account still never stakes below the floor.
        st = freshBook();
        st.equity = 500.0;
        st.cash = 500.0;
        QCOMPARE(paperStakeFor(st, cfg, cfg.riskBudgetFraction), cfg.minStake);
    }

    //! @tstid TS-PAPER-010 @design DES-DOM-PAPER
    // @relation(REQ-F-030, scope=function)
    void TS_PAPER_010_aiProposalSymbolResolution()
    {
        const QStringList known = {QStringLiteral("SPX500"), QStringLiteral("GOLD"),
                                   QStringLiteral("GoldMiners"), QStringLiteral("GER40")};
        // Exactly as spelled, and case-insensitively.
        QCOMPARE(matchProposalSymbol(QStringLiteral("SPX500"), known), QStringLiteral("SPX500"));
        QCOMPARE(matchProposalSymbol(QStringLiteral("ger40"), known), QStringLiteral("GER40"));
        // An exact match wins even though "GOLD" is also a substring of GoldMiners.
        QCOMPARE(matchProposalSymbol(QStringLiteral("GOLD"), known), QStringLiteral("GOLD"));
        // Chatty answers still resolve while they name exactly one instrument —
        // this is what a 1.5B model really answered ("SPX500 composite").
        QCOMPARE(matchProposalSymbol(QStringLiteral("SPX500 composite"), known),
                 QStringLiteral("SPX500"));
        QCOMPARE(matchProposalSymbol(QStringLiteral("buy GER40 now"), known),
                 QStringLiteral("GER40"));
        // Ambiguous or unknown resolves to nothing, and nothing gets traded then.
        QVERIFY(matchProposalSymbol(QStringLiteral("GOLD or GER40, hard to say"), known).isEmpty());
        QVERIFY(matchProposalSymbol(QStringLiteral("Bitcoin"), known).isEmpty());
        QVERIFY(matchProposalSymbol(QString(), known).isEmpty());
        QVERIFY(matchProposalSymbol(QStringLiteral("SPX500"), {}).isEmpty());
    }

    //! @tstid TS-PAPER-011 @design DES-DOM-PAPER
    // @relation(REQ-F-030, scope=function)
    void TS_PAPER_011_aiModeGatesDirectionAndNeverRaisesRisk()
    {
        AiProposal buyPick;
        buyPick.ok = true;
        buyPick.symbol = QStringLiteral("SPX500");
        buyPick.resolvedSymbol = QStringLiteral("SPX500");
        buyPick.dir = 1;
        buyPick.confidence = 70.0;

        // OFF: the composite decides and the proposal is irrelevant.
        AiGate g = paperAiGate(QStringLiteral("GER40"), -1, {buyPick}, BotAiMode::Off);
        QVERIFY(g.allow);
        QCOMPARE(g.dir, -1);
        QVERIFY(!paperAiGate(QStringLiteral("GER40"), 0, {buyPick}, BotAiMode::Off).allow);

        // CONFIRM: only the model's pick, and only while the composite agrees.
        g = paperAiGate(QStringLiteral("SPX500"), 1, {buyPick}, BotAiMode::Confirm);
        QVERIFY(g.allow);
        QCOMPARE(g.dir, 1);
        g = paperAiGate(QStringLiteral("SPX500"), -1, {buyPick}, BotAiMode::Confirm);
        QVERIFY(!g.allow);
        QVERIFY(g.why.contains(QStringLiteral("disagree")));
        g = paperAiGate(QStringLiteral("SPX500"), 0, {buyPick}, BotAiMode::Confirm);
        QVERIFY(!g.allow);
        QVERIFY(g.why.contains(QStringLiteral("neutral")));
        g = paperAiGate(QStringLiteral("GER40"), 1, {buyPick}, BotAiMode::Confirm);
        QVERIFY(!g.allow);
        QVERIFY(g.why.contains(QStringLiteral("AI picked SPX500")));

        // LEAD: the model supplies the direction, against the composite if need be.
        g = paperAiGate(QStringLiteral("SPX500"), -1, {buyPick}, BotAiMode::Lead);
        QVERIFY(g.allow);
        QCOMPARE(g.dir, 1);
        QVERIFY(!paperAiGate(QStringLiteral("GER40"), 1, {buyPick}, BotAiMode::Lead).allow);

        // A HOLD, an unusable answer and an unresolvable instrument all refuse —
        // each with its own stated reason — in both AI-driven modes.
        AiProposal hold = buyPick;
        hold.dir = 0;
        const AiProposal failed;                 // ok = false
        AiProposal unknown = buyPick;
        unknown.resolvedSymbol.clear();
        unknown.symbol = QStringLiteral("Bitcoin");
        for (const BotAiMode mode : {BotAiMode::Confirm, BotAiMode::Lead}) {
            const AiGate h = paperAiGate(QStringLiteral("SPX500"), 1, {hold}, mode);
            QVERIFY(!h.allow);
            QVERIFY(h.why.contains(QStringLiteral("HOLD")));
            const AiGate f = paperAiGate(QStringLiteral("SPX500"), 1, {failed}, mode);
            QVERIFY(!f.allow);
            QVERIFY(f.why.contains(QStringLiteral("no AI proposal")));
            const AiGate u = paperAiGate(QStringLiteral("SPX500"), 1, {unknown}, mode);
            QVERIFY(!u.allow);
            QVERIFY(u.why.contains(QStringLiteral("Bitcoin")));
        }

        // Leverage: a model may be MORE cautious than the risk budget, never bolder.
        QCOMPARE(paperLeverageWithAi(10, 20, BotAiMode::Lead), 10);   // capped by risk
        QCOMPARE(paperLeverageWithAi(10, 3, BotAiMode::Lead), 3);     // its caution honoured
        QCOMPARE(paperLeverageWithAi(10, 0, BotAiMode::Lead), 10);    // unstated -> sized
        QCOMPARE(paperLeverageWithAi(10, -5, BotAiMode::Lead), 10);   // nonsense -> sized
        QCOMPARE(paperLeverageWithAi(10, 2, BotAiMode::Confirm), 10); // only Lead honours it
        QCOMPARE(paperLeverageWithAi(10, 2, BotAiMode::Off), 10);
        QCOMPARE(paperLeverageWithAi(1, 0, BotAiMode::Lead), 1);

        // The mode has a word for the log and the window.
        QCOMPARE(botAiModeWord(BotAiMode::Off), QStringLiteral("off"));
        QCOMPARE(botAiModeWord(BotAiMode::Confirm), QStringLiteral("confirm"));
        QCOMPARE(botAiModeWord(BotAiMode::Lead), QStringLiteral("lead"));
    }

    //! @tstid TS-PAPER-012 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_012_riskBudgetNotATradeCountLimitsHowManyTradesRun()
    {
        const BotConfig cfg;
        // The count is deliberately NOT the policy: the sanity bound is far above
        // what one position per instrument can reach.
        QVERIFY(cfg.maxOpenTrades >= 26);

        // Risk per euro of stake = leverage x stop distance. A x10 trade stopping
        // 2% away risks 20% of its stake.
        EntrySignal sig;
        sig.fillRate = 100.0;
        sig.slRate = 98.0;
        sig.leverage = 10;
        QVERIFY(qAbs(paperEntrySignalRisk(sig) - 0.2) < 1e-9);
        sig.slRate = 0.0;                                  // no stop -> unmeasurable
        QCOMPARE(paperEntrySignalRisk(sig), 0.0);

        // With 30% of equity allowed at risk and 20% risked per euro of stake, the
        // room is 0.30 x 50 000 / 0.20 = 75 000 of stake — far above the 6% target,
        // so early trades are sized by conviction, not by the budget.
        BookState st = freshBook();
        QCOMPARE(paperStakeFor(st, cfg, 0.2), 3000.0);

        // …but as risk accumulates the budget clips the next stake, and eventually
        // refuses: this is what ends a scan, and it is a RISK limit, not a count.
        st.openRisk = cfg.maxPortfolioRiskFraction * st.equity - 400.0;  // 400 EUR of room
        QCOMPARE(paperStakeFor(st, cfg, 0.2), 2000.0);                   // 400 / 0.2
        st.openRisk = cfg.maxPortfolioRiskFraction * st.equity - 10.0;   // 10 EUR of room
        QCOMPARE(paperStakeFor(st, cfg, 0.2), 0.0);                      // < minStake
        st.openRisk = cfg.maxPortfolioRiskFraction * st.equity;          // budget spent
        QCOMPARE(paperStakeFor(st, cfg, 0.2), 0.0);

        // A trade's own contribution is its notional x stop distance.
        PaperTrade t = tradeAt(5000.0, true);   // 3000 stake x5 = 15 000 notional
        t.slRate = 4900.0;                      // 2% away
        QVERIFY(qAbs(t.riskAtStop() - 300.0) < 1e-9);
        t.slRate = 0.0;
        QCOMPARE(t.riskAtStop(), 0.0);

        // And the entry gate names the limit that actually bound — reporting the
        // wrong one sends the reader to tune the wrong knob.
        const CandidateInput in = goodCandidate();
        const EntrySignal entry = buildEntrySignal(in, cfg);

        BookState riskFull = freshBook();
        riskFull.openRisk = cfg.maxPortfolioRiskFraction * riskFull.equity;
        const EntryVerdict byRisk = paperEntryVerdict(in, entry, riskFull, cfg);
        QVERIFY(!byRisk.take);
        QCOMPARE(byRisk.code, QStringLiteral("risk-budget"));
        QVERIFY(byRisk.why.contains(QStringLiteral("risk budget")));

        // Margin exhausted while risk still has room -> "margin-cap", NOT the risk
        // budget (the live log mislabelled exactly this case).
        BookState marginFull = freshBook();
        marginFull.invested = cfg.maxExposureFraction * marginFull.equity;
        marginFull.cash = marginFull.equity - marginFull.invested;
        marginFull.openRisk = 0.05 * marginFull.equity;   // plenty of risk room left
        const EntryVerdict byMargin = paperEntryVerdict(in, entry, marginFull, cfg);
        QVERIFY(!byMargin.take);
        QCOMPARE(byMargin.code, QStringLiteral("margin-cap"));
        QVERIFY(byMargin.why.contains(QStringLiteral("margin cap")));

        // Cash gone while both other rooms are open -> "cash".
        BookState broke = freshBook();
        broke.cash = 10.0;
        const EntryVerdict byCash = paperEntryVerdict(in, entry, broke, cfg);
        QVERIFY(!byCash.take);
        QCOMPARE(byCash.code, QStringLiteral("cash"));
        QVERIFY(byCash.why.contains(QStringLiteral("free cash")));

        // A fitting stake reports no limit at all.
        QVERIFY(paperStakeRoom(freshBook(), cfg, 0.2).limit.isEmpty());
    }

    //! @tstid TS-PAPER-013 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_013_carryCostsCloseTradesThatCannotOutEarnTheirRent()
    {
        const BotConfig cfg;
        // A Wednesday, so the weekend rule is out of the way until asked for.
        const QDateTime wed(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC);
        PaperTrade t = tradeAt(5000.0, true);   // 3000 x5 = 15 000 notional, 3 units
        t.openTime = wed.addSecs(-3600);
        t.feesChargedTo = t.openTime;
        t.slRate = 4900.0;
        t.tpRate = 5150.0;                      // 3% away -> 450 EUR of upside
        t.markRate = 5000.0;

        // What is left to win, and what the rest of the horizon costs.
        QVERIFY(qAbs(paperRemainingUpside(t, 5000.0) - 450.0) < 1e-9);
        QCOMPARE(paperRemainingUpside(t, 5200.0), 0.0);   // already past the target
        PaperTrade noTarget = t;
        noTarget.tpRate = 0.0;
        QCOMPARE(paperRemainingUpside(noTarget, 5000.0), 0.0);

        trading::ExitContext ctx;
        ctx.markRate = 5000.0;
        ctx.now = wed;
        ctx.dirNow = 1;
        ctx.confNow = 80.0;
        ctx.spreadPct = 0.02;                   // 1.50 EUR to get out
        ctx.eurPerUsd = 1.0;

        // Cheap carry: 0.10 per unit per night over ~2 remaining nights is nowhere
        // near the 450 upside — the trade stays open.
        InstrumentFees cheap;
        cheap.buyOvernight = 0.10;
        ctx.fees = cheap;
        ctx.feesKnown = true;
        QVERIFY(paperCostToHold(t, ctx, wed.addDays(2)) < 450.0);
        QCOMPARE(paperCloseDecision(t, ctx, cfg), CloseReason::None);

        // Expensive carry: 80 per unit per night = 240/night on 3 units. Over the
        // remaining ~2.9 days of the 72 h horizon that is more than the 450 left to
        // win, so holding is a loss paid in instalments — close it.
        InstrumentFees dear;
        dear.buyOvernight = 80.0;
        ctx.fees = dear;
        QVERIFY(paperCostToHold(t, ctx, wed.addDays(2)) > 450.0);
        QCOMPARE(paperCloseDecision(t, ctx, cfg), CloseReason::CostsExceedEdge);

        // A CREDIT is never a reason to close: being paid to hold makes the cost
        // negative, and the position stays.
        InstrumentFees credit;
        credit.buyOvernight = -5.0;
        ctx.fees = credit;
        QVERIFY(paperCostToHold(t, ctx, wed.addDays(2)) < 0.0);
        QCOMPARE(paperCloseDecision(t, ctx, cfg), CloseReason::None);
        // …and with no fee table the rule stays silent instead of guessing.
        ctx.fees = dear;
        ctx.feesKnown = false;
        QCOMPARE(paperCloseDecision(t, ctx, cfg), CloseReason::None);

        // The weekend rule: eToro bills the Friday night three times, so it must be
        // EARNED. Friday 12:00 — the next boundary starts a Saturday.
        const QDateTime fri(QDate(2026, 8, 7), QTime(12, 0), QTimeZone::UTC);
        QVERIFY(paperWeekendChargeAhead(fri));
        QVERIFY(!paperWeekendChargeAhead(wed));
        QVERIFY(!paperWeekendChargeAhead(QDateTime()));

        PaperTrade f = t;
        f.openTime = fri.addSecs(-3600);
        f.feesChargedTo = f.openTime;
        trading::ExitContext wknd = ctx;
        wknd.now = fri;
        wknd.feesKnown = true;
        InstrumentFees modest;                  // 3 x 5.0 x 3 units = 45 EUR weekend
        modest.buyOvernight = 5.0;
        wknd.fees = modest;
        // Flat trade: it has not earned the 45 EUR charge -> closed before paying it.
        f.markRate = 5000.0;
        QCOMPARE(paperCloseDecision(f, wknd, cfg), CloseReason::WeekendCarry);
        // Up 200 EUR (well over the charge): it may ride through the weekend.
        f.markRate = 5066.7;                    // ~ +200 gross on a 15 000 notional
        f.openCost = 1.5;
        QVERIFY(f.netPnl() > 45.0);
        QCOMPARE(paperCloseDecision(f, wknd, cfg), CloseReason::None);
        // A weekend CREDIT is not a charge to earn.
        wknd.fees = credit;
        f.markRate = 5000.0;
        f.openCost = 0.0;
        QCOMPARE(paperCloseDecision(f, wknd, cfg), CloseReason::None);

        // A touched barrier still wins over the economics.
        trading::ExitContext hit = wknd;
        hit.fees = dear;
        hit.markRate = 4900.0;
        QCOMPARE(paperCloseDecision(f, hit, cfg), CloseReason::StopLoss);

        // Both new reasons have a word for the table.
        QCOMPARE(closeReasonWord(CloseReason::CostsExceedEdge),
                 QStringLiteral("carry beats the edge"));
        QCOMPARE(closeReasonWord(CloseReason::WeekendCarry), QStringLiteral("weekend carry"));
    }

    //! @tstid TS-PAPER-014 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_014_theAccountNeverCommitsMoreThanItHolds()
    {
        // Opening trade after trade until the bot refuses: cash must never go
        // negative on the way (it DID once — a 100% margin cap plus the opening
        // costs, which are paid from cash on top of the stake).
        const BotConfig cfg;
        PaperBook book(cfg);
        const QDateTime t0(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);
        const EntrySignal sig = buildEntrySignal(goodCandidate(), cfg);

        qint32 opened = 0;
        for (int i = 0; i < 60; ++i) {
            BookState st = book.state();
            st.symbol = SymbolExposure{};  // pretend each is a different instrument
            const double stake = paperStakeFor(st, cfg, paperEntrySignalRisk(sig));
            if (stake <= 0.0) {
                break;  // the economics said stop — which is the point
            }
            QVERIFY(book.open(sig, stake, t0) > 0);
            ++opened;
            const PaperStats s = book.stats();
            QVERIFY2(s.cash >= 0.0, qPrintable(QStringLiteral("cash went negative after %1 "
                                                              "trades: %2")
                                                   .arg(opened)
                                                   .arg(s.cash)));
            // Measured against the equity the DECISIONS were made on: each opening
            // spread reduces equity afterwards, so the ratio may drift a hair above
            // the cap without any decision having exceeded it.
            QVERIFY(s.invested <= ((s.equity + s.costsPaid) * cfg.maxExposureFraction) + 1e-6);
        }
        // It really did take many trades before stopping — the count is not the limit.
        QVERIFY2(opened >= 8, qPrintable(QStringLiteral("only %1 trades").arg(opened)));
        QVERIFY(book.stats().cash >= 0.0);
        // …and free margin is left over on purpose, so the account is never fully
        // committed.
        QVERIFY(book.stats().invested < book.stats().equity);
    }

    //! @tstid TS-PAPER-015 @design DES-DOM-PAPER
    // @relation(REQ-F-031, scope=function)
    void TS_PAPER_015_dayRulesStopTheDayAndNeverEscalate()
    {
        BotConfig cfg;                       // 350 target, 350 loss limit, weekdays only
        const QDateTime tue(QDate(2026, 8, 4), QTime(14, 0), QTimeZone::UTC);
        const QDateTime sat(QDate(2026, 8, 8), QTime(14, 0), QTimeZone::UTC);

        BotDay day;
        day.date = tue.date();
        QCOMPARE(paperDayGate(day, tue, cfg), DayGate::Open);

        // The day is MADE: stop opening. Booked money only — open profit is not a
        // made day, and a rule reading it would stop on nothing.
        day.realized = cfg.dailyProfitTarget;
        QCOMPARE(paperDayGate(day, tue, cfg), DayGate::TargetReached);
        day.realized = cfg.dailyProfitTarget - 0.01;
        QCOMPARE(paperDayGate(day, tue, cfg), DayGate::Open);

        // The day is LOST: stop opening, so a bad day cannot become a bad month.
        day.realized = -cfg.dailyLossLimit;
        QCOMPARE(paperDayGate(day, tue, cfg), DayGate::LossLimitReached);

        // A NEW date starts open whatever yesterday did — no carry-over grudge.
        day.realized = -10000.0;
        QCOMPARE(paperDayGate(day, tue.addDays(1), cfg), DayGate::Open);

        // Saturday and Sunday have no session to trade.
        QCOMPARE(paperDayGate(day, sat, cfg), DayGate::Weekend);
        QCOMPARE(paperDayGate(day, sat.addDays(1), cfg), DayGate::Weekend);
        cfg.tradeWeekdaysOnly = false;               // …unless that is switched off
        day.realized = 0.0;
        day.date = sat.date();
        QCOMPARE(paperDayGate(day, sat, cfg), DayGate::Open);
        cfg.tradeWeekdaysOnly = true;

        // Zero disables either rule outright.
        cfg.dailyProfitTarget = 0.0;
        day.date = tue.date();
        day.realized = 99999.0;
        QCOMPARE(paperDayGate(day, tue, cfg), DayGate::Open);

        // The entry gate refuses with the day's own code, whatever the signal says.
        const BotConfig plain;
        CandidateInput in = goodCandidate();
        in.now = tue;
        const EntrySignal sig = buildEntrySignal(in, plain);
        BookState made = freshBook();
        made.day.date = tue.date();
        made.day.realized = plain.dailyProfitTarget;
        const EntryVerdict v = paperEntryVerdict(in, sig, made, plain);
        QVERIFY(!v.take);
        QCOMPARE(v.code, QStringLiteral("day-target"));
        BookState lost = freshBook();
        lost.day.date = tue.date();
        lost.day.realized = -plain.dailyLossLimit;
        QCOMPARE(paperEntryVerdict(in, sig, lost, plain).code, QStringLiteral("day-loss"));

        // NO ESCALATION: the stake never grows after a losing day. It is a fraction
        // of CURRENT equity, so a smaller account stakes less — the opposite of
        // chasing a daily number.
        BookState after = freshBook();
        after.equity = 45000.0;   // down 10%
        after.cash = 45000.0;
        QVERIFY(paperStakeFor(after, plain, 0.2) < paperStakeFor(freshBook(), plain, 0.2));

        QCOMPARE(dayGateWord(DayGate::TargetReached), QStringLiteral("daily target reached"));
        QCOMPARE(dayGateWord(DayGate::LossLimitReached),
                 QStringLiteral("daily loss limit reached"));
        QCOMPARE(dayGateWord(DayGate::Weekend), QStringLiteral("weekend"));
        QCOMPARE(dayGateWord(DayGate::Open), QStringLiteral("open"));
    }

    //! @tstid TS-PAPER-016 @design DES-DOM-PAPER
    // @relation(REQ-F-031, scope=function)
    void TS_PAPER_016_performanceAndTheLiveMoneyGate()
    {
        // A record with three trading days: +200 / −150 / +400 (the last one a short).
        const auto trade = [](const QDate &date, double net, bool isBuy) {
            PaperClosedTrade c;
            c.symbol = QStringLiteral("SPX500");
            c.isBuy = isBuy;
            c.netPnl = net;
            c.openCost = 1.0;
            c.closeCost = 1.0;
            c.closeTime = QDateTime(date, QTime(17, 0), QTimeZone::UTC);
            c.openTime = c.closeTime.addSecs(-7200);
            return c;
        };
        const QList<PaperClosedTrade> closed = {
            trade(QDate(2026, 8, 3), 200.0, true),
            trade(QDate(2026, 8, 4), -150.0, true),
            trade(QDate(2026, 8, 5), 400.0, false),
        };
        const PaperPerformance perf = paperPerformance(closed, 50000.0, 350.0);
        QCOMPARE(perf.closedTrades, 3);
        QCOMPARE(perf.tradingDays, 3);
        QVERIFY(qAbs(perf.netTotal - 450.0) < 1e-9);
        QVERIFY(qAbs(perf.netPerDay - 150.0) < 1e-9);
        QVERIFY(qAbs(perf.expectancy - 150.0) < 1e-9);
        QVERIFY(qAbs(perf.profitFactor - 4.0) < 1e-9);      // 600 won / 150 lost
        QVERIFY(qAbs(perf.winRate - (200.0 / 3.0)) < 1e-9);
        QVERIFY(qAbs(perf.maxDrawdown - 150.0) < 1e-9);     // the −150 day
        QVERIFY(qAbs(perf.maxDrawdownPct - 0.3) < 1e-9);
        QCOMPARE(perf.daysAtTarget, 1);                     // only the +400 day
        QVERIFY(qAbs(perf.targetHitRate - (100.0 / 3.0)) < 1e-9);
        QVERIFY(qAbs(perf.netLastDays - 450.0) < 1e-9);     // all three fit the window
        QCOMPARE(perf.rollingDays, 3);
        // Both sides are attributed, so a long-only edge cannot hide.
        QCOMPARE(perf.shortTrades, 1);
        QVERIFY(qAbs(perf.shortNet - 400.0) < 1e-9);
        QVERIFY(qAbs(perf.longNet - 50.0) < 1e-9);
        QVERIFY(qAbs(perf.costsPaid - 6.0) < 1e-9);
        // An empty record measures nothing rather than something flattering.
        const PaperPerformance none = paperPerformance({}, 50000.0, 350.0);
        QCOMPARE(none.closedTrades, 0);
        QCOMPARE(none.profitFactor, 0.0);
        QCOMPARE(none.netPerDay, 0.0);

        // THE LIVE GATE. This tiny, profitable-looking record is nowhere near
        // enough evidence to risk real money, and the verdict says why.
        const LiveGateConfig gate;
        const LiveReadiness verdict = paperLiveReadiness(perf, gate);
        QVERIFY(!verdict.ready);
        QCOMPARE(verdict.blockers.size(), 2);               // sample size and days
        QVERIFY(verdict.blockers.join(u"; ").contains(QStringLiteral("closed trades")));
        QVERIFY(verdict.blockers.join(u"; ").contains(QStringLiteral("trading days")));

        // A record that meets every threshold passes — and each threshold is
        // checked on its own, so exactly one failure blocks with exactly one reason.
        PaperPerformance good;
        good.closedTrades = gate.minClosedTrades;
        good.tradingDays = gate.minTradingDays;
        good.netTotal = 5000.0;
        good.profitFactor = 1.5;
        good.maxDrawdownPct = 8.0;
        good.expectancy = 25.0;
        QVERIFY(paperLiveReadiness(good, gate).ready);

        PaperPerformance losing = good;
        losing.netTotal = -1.0;
        QCOMPARE(paperLiveReadiness(losing, gate).blockers.size(), 1);
        PaperPerformance thin = good;
        thin.profitFactor = 1.0;
        QCOMPARE(paperLiveReadiness(thin, gate).blockers.size(), 1);
        PaperPerformance deep = good;
        deep.maxDrawdownPct = 25.0;
        QCOMPARE(paperLiveReadiness(deep, gate).blockers.size(), 1);
        PaperPerformance flat = good;
        flat.expectancy = 0.0;
        QCOMPARE(paperLiveReadiness(flat, gate).blockers.size(), 1);
    }

    //! @tstid TS-PAPER-020 @design DES-DOM-PAPER
    // @relation(REQ-F-032, scope=function)
    void TS_PAPER_020_addingToAPositionNeedsTheModelAndItsOwnCaps()
    {
        const BotConfig cfg;
        QVERIFY(cfg.maxSymbolRiskFraction < cfg.maxGroupRiskFraction);   // the tightest cap
        CandidateInput in = goodCandidate();
        const EntrySignal sig = buildEntrySignal(in, cfg);

        // Nothing open in it: the composite alone is enough, as before.
        QVERIFY(paperEntryVerdict(in, sig, freshBook(), cfg).take);

        // One long already open, no AI behind this candidate: refused, unchanged.
        BookState held = freshBook();
        held.symbol = SymbolExposure{1, 1, 300.0};
        const EntryVerdict noAi = paperEntryVerdict(in, sig, held, cfg);
        QVERIFY(!noAi.take);
        QCOMPARE(noAi.code, QStringLiteral("already-holding"));

        // …the model naming this instrument makes the same candidate takeable.
        in.aiBacked = true;
        QVERIFY(paperEntryVerdict(in, sig, held, cfg).take);

        // But never the other side at the same time: a hedge pays the spread twice
        // to own nothing, and a reversal is a CLOSE.
        BookState heldShort = freshBook();
        heldShort.symbol = SymbolExposure{1, -1, 300.0};
        const EntryVerdict opposite = paperEntryVerdict(in, sig, heldShort, cfg);
        QVERIFY(!opposite.take);
        QCOMPARE(opposite.code, QStringLiteral("opposite-open"));

        // …and never beyond the per-instrument count.
        BookState many = freshBook();
        many.symbol = SymbolExposure{cfg.maxPositionsPerSymbol, 1, 300.0};
        const EntryVerdict counted = paperEntryVerdict(in, sig, many, cfg);
        QVERIFY(!counted.take);
        QCOMPARE(counted.code, QStringLiteral("symbol-count"));

        // …nor beyond what that ONE instrument may risk: 3% of 50 000 = 1500 EUR.
        BookState risky = freshBook();
        risky.symbol = SymbolExposure{1, 1, cfg.maxSymbolRiskFraction * risky.equity};
        const StakeRoom room = paperStakeRoom(risky, cfg, 0.2, QStringLiteral("SPX500"));
        QCOMPARE(room.stake, 0.0);
        QCOMPARE(room.limit, QStringLiteral("symbol-risk"));
        const EntryVerdict byRisk = paperEntryVerdict(in, sig, risky, cfg);
        QVERIFY(!byRisk.take);
        QCOMPARE(byRisk.code, QStringLiteral("symbol-risk"));

        // The book counts its own exposure per instrument, so the runner cannot
        // forget to: two positions in one symbol, one in another.
        PaperBook book;
        const QDateTime t0(QDate(2026, 8, 5), QTime(10, 0), QTimeZone::utc());
        CandidateInput spx = goodCandidate();
        spx.symbol = QStringLiteral("SPX500");
        QVERIFY(book.open(buildEntrySignal(spx, cfg), 3000.0, t0) > 0);
        QVERIFY(book.open(buildEntrySignal(spx, cfg), 1000.0, t0) > 0);
        CandidateInput eur = goodCandidate();
        eur.symbol = QStringLiteral("EURUSD");
        QVERIFY(book.open(buildEntrySignal(eur, cfg), 1000.0, t0) > 0);
        const SymbolExposure exposure = book.exposureFor(QStringLiteral("SPX500"));
        QCOMPARE(exposure.count, 2);
        QCOMPARE(exposure.dir, 1);
        QVERIFY(exposure.risk > 0.0);
        QVERIFY(exposure.risk < book.state().openRisk);          // EURUSD is not in it
        QCOMPARE(book.exposureFor(QStringLiteral("GER40")).count, 0);
    }

    //! @tstid TS-PAPER-021 @design DES-DOM-PAPER
    // @relation(REQ-F-032, scope=function)
    void TS_PAPER_021_theModelCanAskToBeLetOutButSilenceNeverCloses()
    {
        PaperTrade longTrade = tradeAt(5000.0, true);
        longTrade.symbol = QStringLiteral("SPX500");

        AiProposal agree;
        agree.ok = true;
        agree.resolvedSymbol = QStringLiteral("SPX500");
        agree.dir = 1;
        AiProposal reverse = agree;
        reverse.dir = -1;
        reverse.rationale = QStringLiteral("momentum has turned");
        AiProposal closeIt = agree;
        closeIt.dir = 0;
        closeIt.exitNow = true;
        AiProposal elsewhere = agree;
        elsewhere.resolvedSymbol = QStringLiteral("GER40");
        elsewhere.dir = -1;
        AiProposal hold = agree;
        hold.dir = 0;

        // Silence — the common case with a small model — always keeps the position.
        QVERIFY(!paperAiHold(longTrade, {}, BotAiMode::Confirm).close);
        QVERIFY(!paperAiHold(longTrade, {elsewhere}, BotAiMode::Confirm).close);
        // An explicit HOLD is "no action", not "get out".
        QVERIFY(!paperAiHold(longTrade, {hold}, BotAiMode::Confirm).close);
        // Agreement keeps it too.
        QVERIFY(!paperAiHold(longTrade, {agree}, BotAiMode::Lead).close);

        // The other side, or an explicit CLOSE, closes it — with a countable code.
        const HoldVerdict reversed = paperAiHold(longTrade, {reverse}, BotAiMode::Confirm);
        QVERIFY(reversed.close);
        QCOMPARE(reversed.code, QStringLiteral("ai-reversed"));
        QVERIFY(reversed.why.contains(QStringLiteral("momentum has turned")));
        const HoldVerdict asked = paperAiHold(longTrade, {closeIt}, BotAiMode::Lead);
        QVERIFY(asked.close);
        QCOMPARE(asked.code, QStringLiteral("ai-close"));

        // A SHORT is the mirror image: a BUY pick is the contrary opinion.
        PaperTrade shortTrade = tradeAt(5000.0, false);
        shortTrade.symbol = QStringLiteral("SPX500");
        QVERIFY(paperAiHold(shortTrade, {agree}, BotAiMode::Confirm).close);
        QVERIFY(!paperAiHold(shortTrade, {reverse}, BotAiMode::Confirm).close);

        // With the model switched off nobody was asked, so nothing it "said" counts.
        QVERIFY(!paperAiHold(longTrade, {reverse, closeIt}, BotAiMode::Off).close);

        // The open book is put in front of the model, or there is nothing to answer.
        QVERIFY(paperHoldEvidence({}).isEmpty());
        OpenPositionBrief brief;
        brief.symbol = QStringLiteral("OIL.24-7");
        brief.isBuy = false;
        brief.netPnl = -59.68;
        brief.heldHours = 7.5;
        const QString evidence = paperHoldEvidence({brief});
        QVERIFY(evidence.contains(QStringLiteral("OIL.24-7")));
        QVERIFY(evidence.contains(QStringLiteral("SHORT")));
        QVERIFY(evidence.contains(QStringLiteral("-59.68")));
        QVERIFY(evidence.contains(QStringLiteral("CLOSE")));
    }

    //! @tstid TS-PAPER-022 @design DES-DOM-PAPER
    // @relation(REQ-F-032, scope=function)
    void TS_PAPER_022_costsDecideEntriesAndDynamicsDecideExits()
    {
        const BotConfig cfg;
        const CandidateInput in = goodCandidate();
        const EntrySignal sig = buildEntrySignal(in, cfg);

        // The economics of one candidate: the gain at the target against the round
        // trip it has to pay for. Both half-spreads, so twice the single crossing.
        const EntryEconomics econ = paperEntryEconomics(sig, 3000.0, in, cfg);
        QVERIFY(econ.gainAtTarget > 0.0);
        QVERIFY(qAbs(econ.cost - 2.0 * paperHalfSpreadCost(3000.0, sig.leverage, sig.spreadPct))
                < 1e-9);
        QVERIFY(qAbs(econ.ratio - (econ.gainAtTarget / econ.cost)) < 1e-9);

        // A spread wide enough to swallow the target refuses the trade by NAME —
        // the trade would need the target to be reached just to break even.
        CandidateInput dear = goodCandidate();
        dear.spreadPct = 3.0;
        const EntrySignal dearSig = buildEntrySignal(dear, cfg);
        const EntryVerdict refused = paperEntryVerdict(dear, dearSig, freshBook(), cfg);
        QVERIFY(!refused.take);
        QCOMPARE(refused.code, QStringLiteral("cost-vs-edge"));
        QVERIFY(refused.why.contains(QStringLiteral("costs")));
        // …and switching the rule off takes the same candidate.
        BotConfig noGate = cfg;
        noGate.minEdgeOverCost = 0.0;
        QVERIFY(paperEntryVerdict(dear, buildEntrySignal(dear, noGate), freshBook(), noGate).take);

        // Dynamics, exit side. A faded signal on a position that is not paying:
        // the reason to be in the trade is gone, so it goes.
        PaperTrade faded = tradeAt(5000.0, true);
        faded.entryConfidence = 60.0;
        faded.markRate = 4990.0;                 // slightly under water
        // Still nominally its way, but with a third of the conviction that opened it.
        ExitContext ctx = exitAt(4990.0, 1, 20.0, QDateTime(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC));
        QCOMPARE(paperCloseDecision(faded, ctx, cfg), CloseReason::SignalFade);
        ctx.confNow = 55.0;                      // conviction intact: hold on
        QCOMPARE(paperCloseDecision(faded, ctx, cfg), CloseReason::None);
        // A position in PROFIT is never closed for a faded signal — the give-back
        // rule governs winners, and this one is still running.
        PaperTrade winning = faded;
        winning.markRate = 5100.0;
        ctx = exitAt(5100.0, 1, 5.0, QDateTime(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC));
        QCOMPARE(paperCloseDecision(winning, ctx, cfg), CloseReason::None);

        // Given back: it was up 100 EUR and is down to 40 — banked before the rest
        // goes too.
        PaperTrade gaveBack = tradeAt(5000.0, true);
        gaveBack.entryConfidence = 60.0;
        gaveBack.markRate = 5010.0;              // 15 000 notional x 0.2% = +30 EUR
        gaveBack.peakNet = 100.0;                // …down from 100: most of it is gone
        const ExitContext back =
            exitAt(5010.0, 1, 60.0, QDateTime(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC));
        QVERIFY(gaveBack.netPnl() > 0.0);
        QVERIFY(gaveBack.netPnl() <= 50.0);
        QCOMPARE(paperCloseDecision(gaveBack, back, cfg), CloseReason::GiveBack);
        // …but not on a peak too small to be worth protecting.
        PaperTrade noise = gaveBack;
        noise.peakNet = 5.0;
        QCOMPARE(paperCloseDecision(noise, back, cfg), CloseReason::None);

        // The rollover to the HORIZON is part of the entry cost when the fee table
        // is known — an overnight trade that only clears the spread is not a trade.
        CandidateInput carried = goodCandidate();
        carried.now = QDateTime(QDate(2026, 8, 5), QTime(10, 0), QTimeZone::UTC);  // Wednesday
        carried.feesKnown = true;
        carried.fees.buyOvernight = 0.5;      // USD per unit per night
        carried.fees.sellOvernight = 0.5;
        carried.fees.buyWeekend = 1.5;
        carried.fees.sellWeekend = 1.5;
        carried.eurPerUsd = 1.0;
        const EntrySignal carriedSig = buildEntrySignal(carried, cfg);
        const EntryEconomics withCarry = paperEntryEconomics(carriedSig, 3000.0, carried, cfg);
        CandidateInput bare = carried;
        bare.feesKnown = false;
        QVERIFY(withCarry.cost
                > paperEntryEconomics(buildEntrySignal(bare, cfg), 3000.0, bare, cfg).cost);
        // …and an unsizeable candidate has no economics at all rather than a
        // division by zero dressed up as a ratio.
        QCOMPARE(paperEntryEconomics(carriedSig, 0.0, carried, cfg).ratio, 0.0);

        // Every close reason has a word for the table and the record: an exit the
        // reader cannot name is an exit nobody can audit.
        for (const CloseReason reason :
             {CloseReason::StopLoss, CloseReason::TakeProfit, CloseReason::SignalFlip,
              CloseReason::MaxHold, CloseReason::CostsExceedEdge, CloseReason::WeekendCarry,
              CloseReason::AiExit, CloseReason::SignalFade, CloseReason::GiveBack,
              CloseReason::DayTarget, CloseReason::Manual, CloseReason::Reset,
              CloseReason::None}) {
            QVERIFY(!closeReasonWord(reason).isEmpty());
        }
        QCOMPARE(closeReasonWord(CloseReason::AiExit), QStringLiteral("AI says exit"));
        QCOMPARE(closeReasonWord(CloseReason::SignalFade), QStringLiteral("signal faded"));
        QCOMPARE(closeReasonWord(CloseReason::None), QStringLiteral("open"));

        // The peak is the book's memory of what a position WAS worth, and it
        // survives a save/load — otherwise the rule silently switches itself off.
        PaperBook book;
        const QDateTime t0(QDate(2026, 8, 5), QTime(10, 0), QTimeZone::utc());
        const qint64 id = book.open(buildEntrySignal(in, cfg), 3000.0, t0);
        book.mark(id, sig.fillRate * 1.01, true, t0.addSecs(60));
        const double peak = book.openTrades().constFirst().peakNet;
        QVERIFY(peak > 0.0);
        book.mark(id, sig.fillRate, true, t0.addSecs(120));       // handed back
        QCOMPARE(book.openTrades().constFirst().peakNet, peak);   // …the peak stands
        PaperBook restored;
        QVERIFY(restored.fromJson(book.toJson()));
        QCOMPARE(restored.openTrades().constFirst().peakNet, peak);
    }

    //! @tstid TS-PAPER-023 @design DES-DOM-DAY
    // @relation(REQ-F-031, scope=function)
    void TS_PAPER_023_theDayLedgerSurvivesARestart()
    {
        // A restart must not hand the bot a fresh daily budget it has already spent.
        PaperBook book;
        BotConfig cfg;
        cfg.dailyProfitTarget = 350.0;
        book.setConfig(cfg);
        const QDateTime t0(QDate(2026, 8, 5), QTime(10, 0), QTimeZone::utc());
        const qint64 id = book.open(buildEntrySignal(goodCandidate(), cfg), 3000.0, t0);
        QVERIFY(id > 0);
        static_cast<void>(book.close(id, 5200.0, 0.02, CloseReason::TakeProfit, t0.addSecs(3600)));
        const BotDay before = book.day();
        QVERIFY(before.realized > 0.0);
        QCOMPARE(before.opened, 1);
        QCOMPARE(before.closed, 1);

        PaperBook restored;
        QVERIFY(restored.fromJson(book.toJson()));
        QCOMPARE(restored.day().date, before.date);
        QVERIFY(qAbs(restored.day().realized - before.realized) < 1e-9);
        QCOMPARE(restored.day().opened, before.opened);
        QCOMPARE(restored.day().closed, before.closed);

        // A file from before the ledger existed simply starts a day, rather than
        // refusing to load a book that is otherwise perfectly good.
        QJsonObject older = book.toJson();
        older.remove(QStringLiteral("day"));
        PaperBook legacy;
        QVERIFY(legacy.fromJson(older));
        QCOMPARE(legacy.day().realized, 0.0);
        QCOMPARE(legacy.closedTrades().size(), 1);   // …and the record is intact
    }

    //! @tstid TS-PAPER-019 @design DES-DOM-DAY
    // @relation(REQ-F-031, scope=function)
    void TS_PAPER_019_theDayIsBankedByTheSmallestSufficientWinner()
    {
        BotConfig cfg;
        cfg.dailyProfitTarget = 350.0;
        BotDay day;
        day.date = QDate(2026, 8, 4);

        // Nothing yet booked, nothing open: nothing to bank.
        QCOMPARE(paperHarvestPick({}, day, cfg), qint64{0});

        // No single position covers the whole 350: the rule waits rather than
        // stacking closes — banking a PART of the day is what the exits already do.
        const QList<HarvestOption> small{{1, 100.0}, {2, 180.0}, {3, -40.0}};
        QCOMPARE(paperHarvestPick(small, day, cfg), qint64{0});

        // One does: it is closed, and the day is made.
        const QList<HarvestOption> enough{{1, 100.0}, {2, 420.0}, {3, -40.0}};
        QCOMPARE(paperHarvestPick(enough, day, cfg), qint64{2});

        // Several do: the SMALLEST sufficient one goes, so the best position keeps
        // running and the least upside is given up.
        const QList<HarvestOption> several{{1, 900.0}, {2, 420.0}, {3, 355.0}, {4, 40.0}};
        QCOMPARE(paperHarvestPick(several, day, cfg), qint64{3});

        // What is already booked counts: with 300 banked, 60 completes the day.
        day.realized = 300.0;
        QCOMPARE(paperHarvestPick({{7, 60.0}, {8, 900.0}}, day, cfg), qint64{7});
        QCOMPARE(paperHarvestPick({{7, 49.99}}, day, cfg), qint64{0});

        // Target already reached — the day gate stops the bot; this rule stops too
        // instead of closing healthy positions for a number that is already made.
        day.realized = 400.0;
        QCOMPARE(paperHarvestPick({{7, 900.0}}, day, cfg), qint64{0});

        // A losing day is not banked by this rule either: the loss limit governs.
        day.realized = -500.0;
        QCOMPARE(paperHarvestPick({{7, 100.0}}, day, cfg), qint64{0});   // 100 < 850 missing

        // Switched off, or no target: never.
        day.realized = 0.0;
        cfg.harvestForDailyTarget = false;
        QCOMPARE(paperHarvestPick({{7, 900.0}}, day, cfg), qint64{0});
        cfg.harvestForDailyTarget = true;
        cfg.dailyProfitTarget = 0.0;
        QCOMPARE(paperHarvestPick({{7, 900.0}}, day, cfg), qint64{0});

        // The reason has a word of its own for the table — the record must show WHY
        // a winner was cut, since that is the cost this rule pays.
        QVERIFY(closeReasonWord(CloseReason::DayTarget).contains(QStringLiteral("day target")));
    }

    //! @tstid TS-PAPER-018 @design DES-DOM-PAPER
    // @relation(REQ-F-031, scope=function)
    void TS_PAPER_018_correlatedPositionsShareOneRiskBucket()
    {
        // Thirteen long index positions are ONE bet. The bucket map has to say so.
        QCOMPARE(correlationGroup(QStringLiteral("SPX500")), QStringLiteral("equity-index"));
        QCOMPARE(correlationGroup(QStringLiteral("SP.24-7")), QStringLiteral("equity-index"));
        QCOMPARE(correlationGroup(QStringLiteral("NSDQ100")), QStringLiteral("equity-index"));
        QCOMPARE(correlationGroup(QStringLiteral("GER40")), QStringLiteral("equity-index"));
        QCOMPARE(correlationGroup(QStringLiteral("EURUSD")), QStringLiteral("fx"));
        // Listed under Indices, but it is an FX position — and it moves the other way.
        QCOMPARE(correlationGroup(QStringLiteral("USDOLLAR")), QStringLiteral("fx"));
        QCOMPARE(correlationGroup(QStringLiteral("Gold.24-7")), QStringLiteral("metals"));
        QCOMPARE(correlationGroup(QStringLiteral("OIL.24-7")), QStringLiteral("commodity"));
        QCOMPARE(correlationGroup(QStringLiteral("RUBBER")), QStringLiteral("commodity"));
        // An unknown name never joins an existing bucket: it gets its own.
        const QString unknown = correlationGroup(QStringLiteral("NOT-A-SYMBOL"));
        QVERIFY(unknown.contains(QStringLiteral("NOT-A-SYMBOL")));
        QVERIFY(unknown != correlationGroup(QStringLiteral("SPX500")));

        const BotConfig cfg;
        QVERIFY(cfg.maxGroupRiskFraction > 0.0);
        QVERIFY(cfg.maxGroupRiskFraction < cfg.maxPortfolioRiskFraction);   // it must BIND

        // 8% of 50 000 = 4000 EUR of loss-at-stop per bucket. With the whole bucket
        // spent, the next index trade is refused — while the portfolio budget (20%)
        // still has room, which is exactly the case the bucket cap exists for.
        BookState st = freshBook();
        st.riskByGroup[QStringLiteral("equity-index")] = cfg.maxGroupRiskFraction * st.equity;
        st.openRisk = st.riskByGroup.value(QStringLiteral("equity-index"));
        QVERIFY(st.openRisk < cfg.maxPortfolioRiskFraction * st.equity);
        const StakeRoom blocked = paperStakeRoom(st, cfg, 0.2, QStringLiteral("SPX500"));
        QCOMPARE(blocked.stake, 0.0);
        QCOMPARE(blocked.limit, QStringLiteral("group-risk"));

        // A DIFFERENT bucket is untouched by it — diversification is what the rule
        // rewards, and the same call without a symbol asks the portfolio question.
        QCOMPARE(paperStakeRoom(st, cfg, 0.2, QStringLiteral("EURUSD")).stake, 3000.0);
        QCOMPARE(paperStakeRoom(st, cfg, 0.2).stake, 3000.0);

        // Partly spent: 4000 - 3600 = 400 EUR of bucket room, 400 / 0.2 = 2000 stake.
        st.riskByGroup[QStringLiteral("equity-index")] = 3600.0;
        const StakeRoom clipped = paperStakeRoom(st, cfg, 0.2, QStringLiteral("NSDQ100"));
        QCOMPARE(clipped.stake, 2000.0);
        QCOMPARE(clipped.limit, QStringLiteral("group-risk"));

        // The refusal names the bucket, not the portfolio budget: pointing at the
        // wrong limit sends the reader to tune the wrong knob.
        st.riskByGroup[QStringLiteral("equity-index")] = cfg.maxGroupRiskFraction * st.equity;
        CandidateInput in = goodCandidate();
        in.symbol = QStringLiteral("GER40");
        const EntryVerdict v = paperEntryVerdict(in, buildEntrySignal(in, cfg), st, cfg);
        QVERIFY(!v.take);
        QCOMPARE(v.code, QStringLiteral("group-risk"));
        QVERIFY(v.why.contains(QStringLiteral("equity-index")));

        // And the book aggregates it from the open positions themselves, so the
        // runner cannot forget to.
        PaperBook book;
        const QDateTime t0(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::utc());
        CandidateInput index = goodCandidate();
        index.symbol = QStringLiteral("SPX500");
        QVERIFY(book.open(buildEntrySignal(index, cfg), 3000.0, t0) > 0);
        CandidateInput fx = goodCandidate();
        fx.symbol = QStringLiteral("EURUSD");
        QVERIFY(book.open(buildEntrySignal(fx, cfg), 3000.0, t0) > 0);
        const BookState live = book.state();
        QCOMPARE(live.riskByGroup.size(), 2);        // two buckets, not one pile
        QVERIFY(live.riskByGroup.contains(QStringLiteral("equity-index")));
        QVERIFY(live.riskByGroup.contains(QStringLiteral("fx")));
        QVERIFY(qAbs(live.riskByGroup.value(QStringLiteral("equity-index"))
                     + live.riskByGroup.value(QStringLiteral("fx")) - live.openRisk)
                < 1e-9);
    }

    //! @tstid TS-PAPER-017 @design DES-DOM-PAPER
    // @relation(REQ-F-031, scope=function)
    void TS_PAPER_017_theBotTradesShortsOnTheSameTerms()
    {
        // A SELL candidate has to clear exactly the same gates as a BUY and be sized
        // the same way — the bot is not long-only, and nothing about a short is
        // treated as second class.
        const BotConfig cfg;
        CandidateInput down = goodCandidate();
        down.dir = -1;                       // the composite says SELL
        down.now = QDateTime(QDate(2026, 8, 4), QTime(14, 0), QTimeZone::UTC);
        const EntrySignal sell = buildEntrySignal(down, cfg);
        QVERIFY(sell.valid);
        QVERIFY(!sell.isBuy);
        const EntryVerdict take = paperEntryVerdict(down, sell, freshBook(), cfg);
        QVERIFY2(take.take, qPrintable(take.why));
        QCOMPARE(take.stake, 3000.0);        // the same size as the long case

        CandidateInput up = goodCandidate();
        up.now = down.now;
        const EntrySignal buy = buildEntrySignal(up, cfg);
        QCOMPARE(sell.leverage, buy.leverage);
        QCOMPARE(sell.fillRate, buy.fillRate);
        // …with mirrored geometry and the same risk per euro of stake.
        QVERIFY(sell.slRate > sell.fillRate);
        QVERIFY(sell.tpRate < sell.fillRate);
        QVERIFY(qAbs(paperEntrySignalRisk(sell) - paperEntrySignalRisk(buy)) < 1e-9);

        // A short earns when the price FALLS, and its carry uses the sell-side fee
        // (often a credit, which is never a reason to close).
        PaperBook book(cfg);
        const qint64 id = book.open(sell, 3000.0, down.now);
        QVERIFY(id > 0);
        book.mark(id, sell.fillRate * 0.99, true, down.now.addSecs(60));
        QVERIFY(book.openTrades().constFirst().grossPnl() > 0.0);
        QVERIFY(book.stats().openPnl > 0.0);
    }

    //! @tstid TS-PAPER-009 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_009_closeReasonsAreNamedAndTradeHelpersReadOut()
    {
        // Every reason has a word for the table and the log.
        QCOMPARE(closeReasonWord(CloseReason::StopLoss), QStringLiteral("stop-loss"));
        QCOMPARE(closeReasonWord(CloseReason::TakeProfit), QStringLiteral("take-profit"));
        QCOMPARE(closeReasonWord(CloseReason::SignalFlip), QStringLiteral("signal flip"));
        QCOMPARE(closeReasonWord(CloseReason::MaxHold), QStringLiteral("max hold"));
        QCOMPARE(closeReasonWord(CloseReason::Manual), QStringLiteral("manual"));
        QCOMPARE(closeReasonWord(CloseReason::Reset), QStringLiteral("reset"));
        QCOMPARE(closeReasonWord(CloseReason::None), QStringLiteral("open"));

        PaperTrade t = tradeAt(5000.0, true);   // 3000 stake, x5, 3 units
        t.openCost = 1.5;
        QCOMPARE(t.notional(), 15000.0);
        QCOMPARE(t.units(), 3.0);
        QCOMPARE(t.effectiveRate(), 5000.0);
        t.markRate = 0.0;                       // never marked -> the open rate stands
        QCOMPARE(t.effectiveRate(), 5000.0);
        t.markRate = 5100.0;
        QCOMPARE(t.grossPnl(), 300.0);
        t.feesPaid = 3.0;
        QCOMPARE(t.costsSoFar(), 4.5);
        QCOMPARE(t.netPnl(), 295.5);

        PaperClosedTrade c;
        c.openTime = QDateTime(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);
        c.closeTime = c.openTime.addSecs(5400);
        QCOMPARE(c.heldHours(), 1.5);
        c.closeTime = QDateTime();
        QCOMPARE(c.heldHours(), 0.0);
    }
};

QTEST_GUILESS_MAIN(TestPaperTrader)
#include "tst_papertrader.moc"
