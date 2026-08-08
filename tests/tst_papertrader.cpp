// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Unit tests for the paper-trading bot's books and rules (DES-DOM-PAPER).
//
// The point of these is that the SIMULATION cannot flatter itself: the cost
// model, the accounting identity and the entry/exit gates are all checked
// against hand-computed figures.

#include "domain/PaperTrader.h"

#include <QtTest/QtTest>

#include <numeric>

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
    // A deliberately QUIET moment: 13:00 Berlin / 07:00 New York on a Tuesday is
    // not an open, a close or a macro-data slot, so the session rules of REQ-F-034
    // leave the geometry and the size alone unless a test asks for otherwise.
    in.now = QDateTime(QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC);
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
        // The FRACTIONAL exposure cap is what this test is about, so the absolute euro
        // ceiling is switched off to isolate it. Left on, maxInvestedEur would bind first
        // at the default equity and this test would be measuring that instead — the
        // ceiling has its own test (TS-PAPER-031).
        BotConfig cfg;  // 6% of equity, 75% exposure cap, 100 minimum
        cfg.maxInvestedEur = 0.0;
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

        // CRYPTO NORMALISATION — the live defect: the model answers with the exchange pair
        // (BTCUSDT, ETH-USD, SOLUSDT) while eToro names crypto BTC/ETH/SOL. The quote suffix
        // is stripped so the base matches; an unlisted base still finds nothing.
        const QStringList crypto = {QStringLiteral("BTC"), QStringLiteral("ETH"),
                                    QStringLiteral("SOL"), QStringLiteral("SPX500")};
        QCOMPARE(matchProposalSymbol(QStringLiteral("BTCUSDT"), crypto), QStringLiteral("BTC"));
        QCOMPARE(matchProposalSymbol(QStringLiteral("ETH-USD"), crypto), QStringLiteral("ETH"));
        QCOMPARE(matchProposalSymbol(QStringLiteral("SOLUSDT"), crypto), QStringLiteral("SOL"));
        QCOMPARE(matchProposalSymbol(QStringLiteral("btcusd"), crypto), QStringLiteral("BTC"));
        // Only ONE suffix is stripped, so the bare ticker is untouched and still matches.
        QCOMPARE(matchProposalSymbol(QStringLiteral("ETH"), crypto), QStringLiteral("ETH"));
        // XRPUSDT has no catalog base here -> nothing, which is correct (not tradable).
        QVERIFY(matchProposalSymbol(QStringLiteral("XRPUSDT"), crypto).isEmpty());
    }

    //! @tstid TS-PAPER-036 @design DES-DOM-PAPER
    // @relation(REQ-F-030, REQ-F-031, scope=function)
    //
    // Crypto is its OWN correlation bucket, capped at x2, and costs ~1% — the three facts the
    // user gave about eToro crypto, each pinned so a change to one is visible.
    void TS_PAPER_036_cryptoIsItsOwnBucketCappedAndCosted()
    {
        // One bucket, so three crypto positions count as close to one bet in the risk budget.
        QCOMPARE(correlationGroup(QStringLiteral("BTC")), QStringLiteral("crypto"));
        QCOMPARE(correlationGroup(QStringLiteral("ETH")), QStringLiteral("crypto"));
        QCOMPARE(correlationGroup(QStringLiteral("SOL")), QStringLiteral("crypto"));
        // …distinct from the equity indices, so crypto risk does not hide inside the index bucket.
        QVERIFY(correlationGroup(QStringLiteral("SPX500")) != correlationGroup(QStringLiteral("BTC")));

        // eToro caps retail crypto at x2.
        QCOMPARE(groupLeverageCap(QStringLiteral("crypto")), 2);

        // The ~1% round-trip cost is modelled as a spread FLOOR for crypto and nothing for a
        // non-crypto instrument.
        QCOMPARE(minSpreadPctFor(QStringLiteral("BTC")), 1.0);
        QCOMPARE(minSpreadPctFor(QStringLiteral("ETH")), 1.0);
        QCOMPARE(minSpreadPctFor(QStringLiteral("SPX500")), 0.0);
        QCOMPARE(minSpreadPctFor(QStringLiteral("NOT-LISTED")), 0.0);

        // Crypto trades 24/7, so the weekday-only stop must NOT refuse it on a weekend, while
        // an equity index is still stopped then.
        QVERIFY(tradesOnWeekend(QStringLiteral("BTC")));
        QVERIFY(!tradesOnWeekend(QStringLiteral("SPX500")));
        BotDay day;
        const QDateTime sat(QDate(2026, 8, 8), QTime(12, 0), QTimeZone::UTC);  // a Saturday
        BotConfig weekdayOnly;
        weekdayOnly.tradeWeekdaysOnly = true;
        QCOMPARE(paperDayGate(day, sat, weekdayOnly, /*tradesWeekend=*/false), DayGate::Weekend);
        QCOMPARE(paperDayGate(day, sat, weekdayOnly, /*tradesWeekend=*/true), DayGate::Open);
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
            // An unusable answer is refused — and the REASON names which of the four
            // situations it is. "no AI proposal available" used to cover all four, which
            // told a reader nothing: no model configured, no answer yet, an answer too old,
            // and an answer that failed to parse call for four different fixes.
            AiSource unparsed;
            unparsed.configured = true;
            unparsed.asked = true;
            unparsed.received = 1;      // it answered…
            unparsed.usable = 0;        // …with something unusable
            unparsed.ageMs = 1000;
            unparsed.maxAgeMs = 300000;
            const AiGate f = paperAiGate(QStringLiteral("SPX500"), 1, {failed}, mode, unparsed);
            QVERIFY(!f.allow);
            QCOMPARE(f.code, QStringLiteral("ai-unparsed"));
            QVERIFY(f.why.contains(QStringLiteral("none of them parsed")));

            // No model at all is a DIFFERENT refusal, and names what to configure.
            const AiSource absent;
            const AiGate none = paperAiGate(QStringLiteral("SPX500"), 1, {failed}, mode, absent);
            QCOMPARE(none.code, QStringLiteral("ai-not-configured"));
            QVERIFY(none.why.contains(QStringLiteral("ollamaModel")));

            // An answer past its freshness bound is a third, and carries the actual age —
            // a CPU model that cannot keep up leaves every instrument un-evaluated, and the
            // number is what makes that diagnosable rather than guessable.
            AiSource stale;
            stale.configured = true;
            stale.asked = true;
            stale.received = 2;
            stale.ageMs = 600000;
            stale.maxAgeMs = 300000;
            const AiGate old = paperAiGate(QStringLiteral("SPX500"), 1, {failed}, mode, stale);
            QCOMPARE(old.code, QStringLiteral("ai-stale"));
            QVERIFY(old.why.contains(QStringLiteral("600 s")));
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

    //! @tstid TS-PAPER-035 @design DES-DOM-PAPER
    // @relation(REQ-F-030, scope=function)
    //
    // LEAD-WITH-FALLBACK: the model leads WHEN IT SPEAKS, and the composite leads when it
    // does not. This is what makes the bot actually trade — measured on its own ledger, 442
    // of 673 refusals were the 1.5B model simply not answering while the composite had a
    // direction the whole time.
    void TS_PAPER_035_leadFallsBackToCompositeWhenTheModelAbstains()
    {
        AiProposal unusable;
        unusable.ok = false;   // the model answered with nothing actionable
        AiSource src;
        src.configured = true;
        src.asked = true;
        src.received = 1;
        src.ageMs = 1000;
        src.maxAgeMs = 300000;
        src.leadFallback = false;   // strict lead: a proposal or nothing

        // WITHOUT fallback (the strict default): no model direction, no trade.
        const AiGate strict = paperAiGate(QStringLiteral("SPX500"), /*compositeDir=*/1,
                                          {unusable}, BotAiMode::Lead, src);
        QVERIFY(!strict.allow);
        QCOMPARE(strict.code, QStringLiteral("ai-unparsed"));

        // WITH fallback: the composite's LONG leads instead, and the gate allows it.
        AiSource fbSrc = src;
        fbSrc.leadFallback = true;
        const AiGate fb = paperAiGate(QStringLiteral("SPX500"), /*compositeDir=*/1,
                                      {unusable}, BotAiMode::Lead, fbSrc);
        QVERIFY(fb.allow);
        QCOMPARE(fb.dir, 1);            // direction comes from the composite
        QVERIFY(fb.code.isEmpty());

        // A NEUTRAL composite (dir 0) still cannot manufacture a trade from nothing.
        const AiGate neutral = paperAiGate(QStringLiteral("SPX500"), /*compositeDir=*/0,
                                           {unusable}, BotAiMode::Lead, fbSrc);
        QVERIFY(!neutral.allow);

        // An explicit HOLD is NOT overridden: a model that says HOLD has an opinion.
        AiProposal hold;
        hold.ok = true;
        hold.symbol = QStringLiteral("SPX500");
        hold.resolvedSymbol = QStringLiteral("SPX500");   // matchPick keys on the RESOLVED name
        hold.dir = 0;   // HOLD
        const AiGate held = paperAiGate(QStringLiteral("SPX500"), /*compositeDir=*/1,
                                        {hold}, BotAiMode::Lead, fbSrc);
        QVERIFY(!held.allow);
        QCOMPARE(held.code, QStringLiteral("ai-hold"));

        // The model naming a DIFFERENT instrument falls back for THIS one rather than
        // refusing it as ai-other-pick.
        AiProposal other;
        other.ok = true;
        other.symbol = QStringLiteral("NSDQ100");
        other.resolvedSymbol = QStringLiteral("NSDQ100");
        other.dir = 1;
        const AiGate otherPick = paperAiGate(QStringLiteral("SPX500"), /*compositeDir=*/-1,
                                             {other}, BotAiMode::Lead, fbSrc);
        QVERIFY(otherPick.allow);
        QCOMPARE(otherPick.dir, -1);   // SPX500 follows ITS composite, not NSDQ100's pick
    }

    //! @tstid TS-PAPER-012 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_012_riskBudgetNotATradeCountLimitsHowManyTradesRun()
    {
        // The RISK BUDGET is the subject here — that it, and not a trade count, is what
        // limits the book. The absolute euro ceiling is switched off so it cannot be the
        // thing that stops the run; with it on, the answer would be "the invested ceiling
        // stopped it", which is a different (and separately tested) statement.
        BotConfig cfg;
        cfg.maxInvestedEur = 0.0;
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

        // THE AI CARRY OVERRIDE (REQ-F-032): an ACTIVE keep from the model lets a conviction
        // position ride through carry it has not earned — but never through a stop, and only
        // while the config permits it, and never on mere silence.
        //
        // Expensive carry that would otherwise close as CostsExceedEdge:
        trading::ExitContext aiCtx = ctx;   // `dear` fees, still set on ctx above
        aiCtx.fees = dear;
        aiCtx.feesKnown = true;
        QCOMPARE(paperCloseDecision(t, aiCtx, cfg), CloseReason::CostsExceedEdge);
        aiCtx.aiBacksHold = true;           // the model actively wants it kept
        QCOMPARE(paperCloseDecision(t, aiCtx, cfg), CloseReason::None);  // rides the carry

        // The unearned WEEKEND charge, likewise waived by an active keep.
        trading::ExitContext aiWknd = wknd;
        aiWknd.fees = modest;
        f.markRate = 5000.0;                // flat: has not earned the 45 EUR charge
        f.openCost = 0.0;
        QCOMPARE(paperCloseDecision(f, aiWknd, cfg), CloseReason::WeekendCarry);
        aiWknd.aiBacksHold = true;
        QCOMPARE(paperCloseDecision(f, aiWknd, cfg), CloseReason::None);

        // The STOP is never overridden: the barrier is checked before the carry rules, so an
        // AI-backed position that has hit its stop is still closed. The AI can hold through
        // the rent, not through a loss.
        trading::ExitContext aiHit = aiWknd;
        aiHit.markRate = 4900.0;
        QCOMPARE(paperCloseDecision(f, aiHit, cfg), CloseReason::StopLoss);

        // Config OFF restores strict carry discipline even with the model backing it.
        BotConfig strictCarry = cfg;
        strictCarry.aiMayOverrideCarry = false;
        QCOMPARE(paperCloseDecision(t, aiCtx, strictCarry), CloseReason::CostsExceedEdge);
    }

    //! @tstid TS-PAPER-014 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PAPER_014_theAccountNeverCommitsMoreThanItHolds()
    {
        // Opening trade after trade until the bot refuses: cash must never go
        // negative on the way (it DID once — a 100% margin cap plus the opening
        // costs, which are paid from cash on top of the stake).
        //
        // The absolute euro ceiling is switched off because this test needs to push the
        // book until CASH is the thing that runs out — that is the invariant under test.
        // With the ceiling on, the run stops at 15 000 committed (five trades at the 6%
        // default) long before cash is anywhere near strained, and the test would pass
        // while proving nothing about the cash rule.
        BotConfig cfg;
        cfg.maxInvestedEur = 0.0;
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

    //! @tstid TS-PAPER-025 @design DES-DOM-WHEN
    // @relation(REQ-F-034, scope=function)
    void TS_PAPER_025_churnIsWhatLosesTheMoneyAndTheRulesSayNo()
    {
        // Measured, not imagined: six closes in one hour, median holding time 5.2
        // minutes, gross +1.64 EUR against 19.38 EUR of spread. Every rule below
        // exists because of that hour.
        const BotConfig cfg;
        QVERIFY(cfg.minHoldMinutes > 0);
        QVERIFY(cfg.aiExitMinConfidence >= 50.0);

        PaperTrade fresh = tradeAt(5000.0, true);
        fresh.symbol = QStringLiteral("SPX500");
        fresh.entryCompositeConf = 60.0;
        fresh.markRate = 4990.0;                       // a little under water
        const QDateTime opened = fresh.openTime;

        AiProposal reversal;
        reversal.ok = true;
        reversal.resolvedSymbol = QStringLiteral("SPX500");
        reversal.dir = -1;                             // the model turned around
        reversal.confidence = 90.0;

        // Five minutes in — exactly the case that bled the book — the model's change
        // of mind is SHOWN but not acted on.
        const HoldVerdict tooSoon =
            paperAiHold(fresh, {reversal}, BotAiMode::Lead, opened.addSecs(qint64{60} * 5), cfg);
        QCOMPARE(tooSoon.opinion, HoldOpinion::Close);  // the flag still says close
        QVERIFY(!tooSoon.close);                        // …but nothing closes
        QCOMPARE(tooSoon.code, QStringLiteral("ai-too-soon"));
        QVERIFY(tooSoon.why.contains(QStringLiteral("held only")));

        // After the minimum holding time, the same answer does close it.
        const HoldVerdict allowed =
            paperAiHold(fresh, {reversal}, BotAiMode::Lead,
                        opened.addSecs(qint64{60} * (cfg.minHoldMinutes + 1)), cfg);
        QVERIFY(allowed.close);
        QCOMPARE(allowed.code, QStringLiteral("ai-reversed"));

        // A HESITANT reversal never closes anything, however old the trade: acting on
        // it costs two half-spreads, and a small model's 30%-confidence turn is not
        // worth them.
        AiProposal unsure = reversal;
        unsure.confidence = cfg.aiExitMinConfidence - 10.0;
        const HoldVerdict hesitant =
            paperAiHold(fresh, {unsure}, BotAiMode::Lead,
                        opened.addSecs(qint64{60} * (cfg.minHoldMinutes + 60)), cfg);
        QCOMPARE(hesitant.opinion, HoldOpinion::Close);
        QVERIFY(!hesitant.close);
        QCOMPARE(hesitant.code, QStringLiteral("ai-too-soon"));
        QVERIFY(hesitant.why.contains(QStringLiteral("conviction")));

        // The dynamic exits wait out the same clock — a signal cannot "fade" in the
        // first minute of a trade.
        const ExitContext early = exitAt(4990.0, 1, 5.0, opened.addSecs(60));
        QCOMPARE(paperCloseDecision(fresh, early, cfg), CloseReason::None);
        const ExitContext late = exitAt(4990.0, 1, 5.0,
                                        opened.addSecs(qint64{60} * (cfg.minHoldMinutes + 1)));
        QCOMPARE(paperCloseDecision(fresh, late, cfg), CloseReason::SignalFade);
        // …but a STOP still closes instantly, because that is a price and not an
        // opinion.
        PaperTrade stopped = fresh;
        stopped.slRate = 4995.0;
        QCOMPARE(paperCloseDecision(stopped, early, cfg), CloseReason::StopLoss);

        // A model-led trade must not look faded the moment it opens. In lead mode
        // entryConfidence is the MODEL's number (95 on its own scale) while the
        // composite reads ~20 — comparing those two closed every AI trade at once.
        PaperTrade modelLed = fresh;
        modelLed.entryConfidence = 95.0;               // the model's conviction
        modelLed.entryCompositeConf = 20.0;            // …the composite's, at the same moment
        const ExitContext steady = exitAt(4990.0, 1, 20.0,
                                          opened.addSecs(qint64{60} * (cfg.minHoldMinutes + 1)));
        QCOMPARE(paperCloseDecision(modelLed, steady, cfg), CloseReason::None);

        // Whatever the caps say, the answer is always a leverage the INSTRUMENT
        // ACTUALLY OFFERS: Gold.24-7 sells 1/2/5/20, so an x8 bucket ceiling must
        // fold DOWN to x5 rather than invent an x8 nobody can trade.
        CandidateInput gold = goodCandidate();
        gold.symbol = QStringLiteral("Gold.24-7");
        gold.bid = 4183.0;
        gold.ask = 4184.0;
        gold.spreadPct = 0.02;
        gold.closes = QList<double>(120, 4183.0);
        for (qsizetype i = 0; i < gold.closes.size(); ++i) {
            gold.closes[i] = 4183.0 + (2.0 * static_cast<double>(i % 5));
        }
        const EntrySignal goldSig = buildEntrySignal(gold, cfg);
        QVERIFY(goldSig.valid);
        const QList<qint32> goldSteps = {1, 2, 5, 20};
        QVERIFY2(goldSteps.contains(goldSig.leverage),
                 qPrintable(QStringLiteral("Gold.24-7 levered x%1, which it does not offer")
                                .arg(goldSig.leverage)));
        QVERIFY(goldSig.leverage <= groupLeverageCap(QStringLiteral("metals")));

        // And forex is levered more carefully than an index, because a few tenths of
        // a percent is EUR/USD's whole day — the model asked for x30.
        QCOMPARE(groupLeverageCap(QStringLiteral("fx")), 5);
        QVERIFY(groupLeverageCap(QStringLiteral("fx"))
                < groupLeverageCap(QStringLiteral("equity-index")));
        CandidateInput fx = goodCandidate();
        fx.symbol = QStringLiteral("EURUSD");
        fx.bid = 1.0900;
        fx.ask = 1.0902;
        fx.spreadPct = 0.02;
        fx.closes = QList<double>(120, 1.09);
        for (qsizetype i = 0; i < fx.closes.size(); ++i) {
            fx.closes[i] = 1.09 + (0.0002 * static_cast<double>(i % 7));
        }
        const EntrySignal fxSig = buildEntrySignal(fx, cfg);
        QVERIFY(fxSig.valid);
        QVERIFY2(fxSig.leverage <= groupLeverageCap(QStringLiteral("fx")),
                 qPrintable(QStringLiteral("FX levered x%1").arg(fxSig.leverage)));
        const QList<qint32> fxSteps = {1, 2, 5, 10, 20, 30};
        QVERIFY(fxSteps.contains(fxSig.leverage));   // …and on EURUSD's own ladder
        const CandidateInput index = goodCandidate();
        QVERIFY(buildEntrySignal(index, cfg).leverage
                <= groupLeverageCap(QStringLiteral("equity-index")));
    }

    //! @tstid TS-PAPER-024 @design DES-DOM-WHEN
    // @relation(REQ-F-034, scope=function)
    void TS_PAPER_024_whenItTradesMattersAsMuchAsWhat()
    {
        // The loud windows of the trading day, on the instrument's OWN clock. August
        // is summer time in both zones, which is precisely why the classifier asks
        // the zone database instead of adding a fixed offset.
        const auto berlin = [](int hour, int minute) {
            return QDateTime(QDate(2026, 8, 4), QTime(hour, minute),
                             QTimeZone("Europe/Berlin"));
        };
        // The first QUARTER HOUR after an open is its own phase: fast, wide-spread,
        // and the phase that forms the range whose break is the actual signal.
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"), berlin(9, 5)),
                 SessionPhase::OpeningChaos);
        QCOMPARE(sessionPhaseFor(QStringLiteral("SPX500"), berlin(15, 35)),
                 SessionPhase::OpeningChaos);
        // …and the rest of that first hour is the readable part.
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"), berlin(9, 40)),
                 SessionPhase::OpeningBurst);
        QCOMPARE(sessionPhaseFor(QStringLiteral("SPX500"), berlin(16, 25)),
                 SessionPhase::OpeningBurst);
        // …and a scheduled release inside that hour is the more specific fact: 16:00
        // Berlin is 10:00 in New York, when ISM and friends print.
        QCOMPARE(sessionPhaseFor(QStringLiteral("SPX500"), berlin(16, 5)),
                 SessionPhase::DataWindow);
        // The central bank: the statement at 20:00 Berlin (14:00 New York) and the
        // press conference half an hour later, the one window whose first move
        // regularly reverses in full.
        QCOMPARE(sessionPhaseFor(QStringLiteral("SPX500"), berlin(20, 5)),
                 SessionPhase::PolicyWindow);
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"), berlin(20, 35)),
                 SessionPhase::PolicyWindow);
        // The power hour, 21:00-21:30 Berlin, and then the close itself.
        QCOMPARE(sessionPhaseFor(QStringLiteral("SPX500"), berlin(21, 15)),
                 SessionPhase::PowerHour);
        QCOMPARE(sessionPhaseFor(QStringLiteral("SPX500"), berlin(21, 45)),
                 SessionPhase::ClosingBurst);
        // The US macro slots, which a German book feels at 14:30 and 16:00.
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"), berlin(14, 32)),
                 SessionPhase::DataWindow);
        QCOMPARE(sessionPhaseFor(QStringLiteral("EURUSD"), berlin(16, 5)),
                 SessionPhase::DataWindow);
        // 17:00-17:30, the run into the German close.
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"), berlin(17, 20)),
                 SessionPhase::ClosingBurst);
        // …and the American lunch hours are quiet, which is a real answer too.
        QCOMPARE(sessionPhaseFor(QStringLiteral("SPX500"), berlin(18, 30)),
                 SessionPhase::Normal);
        // …and the quiet middle of the day is quiet.
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"), berlin(13, 0)),
                 SessionPhase::Normal);
        // A weekend has no session to be at the edge of, and an invalid instant
        // cannot be judged at all.
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"),
                                 QDateTime(QDate(2026, 8, 8), QTime(9, 5),
                                           QTimeZone("Europe/Berlin"))),
                 SessionPhase::Normal);
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"), QDateTime()), SessionPhase::Normal);
        QVERIFY(!sessionPhaseWord(SessionPhase::OpeningBurst).isEmpty());
        QCOMPARE(sessionPhaseWord(SessionPhase::Normal), QStringLiteral("normal"));
    }

    //! @tstid TS-PAPER-029 @design DES-DOM-WHEN
    // @relation(REQ-F-034, scope=function)
    void TS_PAPER_029_someInstrumentsHaveToEarnTheAttempt()
    {
        // Measured: USDOLLAR took 3 trades for −19.22 EUR. The dollar index moves a few
        // hundredths of a percent an hour and the spread does not care, so it is traded
        // only when the expected move is big AND fast enough to outrun its costs.
        const BotConfig cfg;
        QVERIFY(cfg.reluctantSymbols.contains(QStringLiteral("USDOLLAR")));

        // A quiet dollar index: refused BY NAME, with the numbers that refused it.
        CandidateInput quiet = goodCandidate();
        quiet.symbol = QStringLiteral("USDOLLAR");
        quiet.bid = 99.60;
        quiet.ask = 99.62;
        quiet.spreadPct = 0.02;
        quiet.closes = QList<double>(120, 99.6);
        for (qsizetype i = 0; i < quiet.closes.size(); ++i) {
            quiet.closes[i] = 99.6 + (0.004 * static_cast<double>(i % 5));   // ~0.004%/h
        }
        const EntrySignal quietSig = buildEntrySignal(quiet, cfg);
        const EntryVerdict refused = paperEntryVerdict(quiet, quietSig, freshBook(), cfg);
        QVERIFY(!refused.take);
        QCOMPARE(refused.code, QStringLiteral("reluctant-symbol"));
        QVERIFY(refused.why.contains(QStringLiteral("USDOLLAR")));
        QVERIFY(refused.why.contains(QStringLiteral("per hour")));

        // The SAME quiet series on an instrument that is not on the list is taken —
        // the rule is about this instrument, not about quiet markets in general.
        CandidateInput other = quiet;
        other.symbol = QStringLiteral("SPX500");
        QVERIFY(paperEntryVerdict(other, buildEntrySignal(other, cfg), freshBook(), cfg).take);

        // A dollar index that IS moving, and with conviction, may be traded: the rule is
        // reluctance, not a ban.
        CandidateInput lively = quiet;
        lively.confidence = cfg.minConfidence * cfg.reluctantConfidenceFactor + 5.0;
        for (qsizetype i = 0; i < lively.closes.size(); ++i) {
            lively.closes[i] = 99.6 + (0.55 * static_cast<double>(i % 5));   // a real move
        }
        const EntrySignal livelySig = buildEntrySignal(lively, cfg);
        QVERIFY(livelySig.volPct * livelySig.leverage >= cfg.reluctantMinHourlyMovePct);
        QVERIFY2(paperEntryVerdict(lively, livelySig, freshBook(), cfg).take,
                 qPrintable(paperEntryVerdict(lively, livelySig, freshBook(), cfg).why));

        // …but a lively dollar index with ORDINARY conviction is still refused: both
        // conditions have to hold.
        CandidateInput halfHearted = lively;
        halfHearted.confidence = cfg.minConfidence + 1.0;
        const EntryVerdict thin =
            paperEntryVerdict(halfHearted, buildEntrySignal(halfHearted, cfg), freshBook(), cfg);
        QVERIFY(!thin.take);
        QCOMPARE(thin.code, QStringLiteral("reluctant-symbol"));
        QVERIFY(thin.why.contains(QStringLiteral("conviction")));

        // And the list is configuration: emptying it removes the reluctance entirely.
        BotConfig eager = cfg;
        eager.reluctantSymbols.clear();
        QVERIFY(paperEntryVerdict(quiet, quietSig, freshBook(), eager).take);
    }

    //! @tstid TS-PAPER-031 @design DES-DOM-DAY
    // @relation(REQ-F-031, scope=function)
    void TS_PAPER_031_theInvestedTotalIsCappedInEurosAndMatchesItsRows()
    {
        BotConfig cfg;
        cfg.startEquity = 50000.0;
        cfg.maxInvestedEur = 15000.0;
        // The fractional cap is deliberately left wide here so the ABSOLUTE one is what
        // binds; both are exercised as a pair further down.
        cfg.maxExposureFraction = 0.75;

        // 1. The reported total is the SUM OF THE ROWS, by construction. This is the
        //    invariant behind "adding the visible Invested column up must give the figure
        //    in the account line" — the two are read from the same book, and this pins it
        //    so a future change cannot let a cached total drift from the positions.
        PaperBook book(cfg);
        const QDateTime t0(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);
        EntrySignal sig = buildEntrySignal(goodCandidate(), cfg);
        sig.fillRate = 5000.0;
        sig.leverage = 5;
        static_cast<void>(book.open(sig, 3000.0, t0));
        static_cast<void>(book.open(sig, 2500.0, t0.addSecs(60)));
        const QList<PaperTrade> rows = book.openTrades();
        const double sumOfRows =
            std::accumulate(rows.cbegin(), rows.cend(), 0.0,
                            [](double acc, const PaperTrade &t) { return acc + t.stake; });
        QCOMPARE(book.stats().invested, sumOfRows);
        QCOMPARE(book.stats().invested, 5500.0);

        // 2. The absolute ceiling binds, and it is NAMED as itself rather than as the
        //    margin cap — the two would send a reader to change different numbers.
        BookState atCeiling;
        atCeiling.equity = 50000.0;
        atCeiling.cash = 44500.0;
        atCeiling.invested = 15000.0;   // exactly at the ceiling
        const StakeRoom noRoom = paperStakeRoom(atCeiling, cfg, cfg.riskBudgetFraction,
                                                QStringLiteral("SPX500"));
        QCOMPARE(noRoom.stake, 0.0);
        QCOMPARE(noRoom.limit, QStringLiteral("invested-cap"));

        // 3. …and it is what bound, not the fraction: 0.75 × 50 000 is 37 500, so the
        //    fractional cap still had 22 500 of room. Without the absolute ceiling this
        //    book would have kept opening.
        QVERIFY((atCeiling.equity * cfg.maxExposureFraction) - atCeiling.invested > 20000.0);

        // 4. A fraction CANNOT express the same rule, which is the whole reason both
        //    exist. The identical 0.30 fraction that means 15 000 at 50 000 of equity
        //    means 18 000 once equity reaches 60 000 — it drifts with the P&L it is meant
        //    to bound, while the absolute ceiling does not move.
        BotConfig fractionOnly = cfg;
        fractionOnly.maxInvestedEur = 0.0;   // switched off
        fractionOnly.maxExposureFraction = 0.30;
        BookState richer = atCeiling;
        richer.equity = 60000.0;
        richer.cash = 45000.0;
        const StakeRoom drifted = paperStakeRoom(richer, fractionOnly,
                                                 fractionOnly.riskBudgetFraction,
                                                 QStringLiteral("SPX500"));
        QVERIFY(drifted.stake > 0.0);   // 0.30 × 60 000 = 18 000 > the 15 000 committed
        // The absolute ceiling refuses that same book, because 15 000 is 15 000.
        const StakeRoom held = paperStakeRoom(richer, cfg, cfg.riskBudgetFraction,
                                              QStringLiteral("SPX500"));
        QCOMPARE(held.stake, 0.0);
        QCOMPARE(held.limit, QStringLiteral("invested-cap"));

        // 5. Zero switches it off entirely: configuration, not a hard-coded rule.
        BotConfig uncapped = cfg;
        uncapped.maxInvestedEur = 0.0;
        QVERIFY(paperStakeRoom(atCeiling, uncapped, uncapped.riskBudgetFraction,
                               QStringLiteral("SPX500"))
                    .stake
                > 0.0);
    }

    //! @tstid TS-PAPER-030 @design DES-DOM-WHEN
    // @relation(REQ-F-034, scope=function)
    void TS_PAPER_030_aFavouriteOfTheModelStillHasToConvince()
    {
        // The OTHER way reluctance is earned, and the one the hourly-move floor cannot
        // see. Measured over a 286-decision run in lead mode: 3 positions opened, 2 of
        // them OIL.24-7, and OIL.24-7 carried −179.04 of the −197.05 EUR net — 91% of the
        // damage from one instrument that moves perfectly well. A small local model keeps
        // naming its favourite, so in lead mode one symbol becomes most of the book.
        const BotConfig cfg;
        QVERIFY(cfg.reluctantSymbols.contains(QStringLiteral("OIL.24-7")));

        // An oil instrument that genuinely moves: it CLEARS the hourly-move floor, so
        // that test alone would wave it straight through. This is the regression — the
        // concentration case must not be judged by volatility.
        CandidateInput lively = goodCandidate();
        lively.symbol = QStringLiteral("OIL.24-7");
        lively.bid = 76.98;
        lively.ask = 77.02;
        lively.spreadPct = 0.05;
        lively.closes = QList<double>(120, 77.0);
        for (qsizetype i = 0; i < lively.closes.size(); ++i) {
            lively.closes[i] = 77.0 + (0.42 * static_cast<double>(i % 5));
        }
        const EntrySignal livelySig = buildEntrySignal(lively, cfg);
        QVERIFY(livelySig.volPct * livelySig.leverage >= cfg.reluctantMinHourlyMovePct);

        // With ORDINARY conviction it is refused anyway, and the reason names conviction
        // rather than the hourly move — proving the second condition is what bit.
        CandidateInput ordinary = lively;
        ordinary.confidence = cfg.minConfidence + 1.0;
        const EntryVerdict refused =
            paperEntryVerdict(ordinary, buildEntrySignal(ordinary, cfg), freshBook(), cfg);
        QVERIFY(!refused.take);
        QCOMPARE(refused.code, QStringLiteral("reluctant-symbol"));
        QVERIFY(refused.why.contains(QStringLiteral("OIL.24-7")));
        QVERIFY(refused.why.contains(QStringLiteral("conviction")));

        // Reluctance is NOT a ban: with the doubled conviction actually present, the
        // trade is allowed. A forbidden-symbol list would freeze today's model's habits
        // into a permanent rule, which is the thing this deliberately avoids.
        CandidateInput convinced = lively;
        convinced.confidence = cfg.minConfidence * cfg.reluctantConfidenceFactor + 5.0;
        const EntryVerdict allowed =
            paperEntryVerdict(convinced, buildEntrySignal(convinced, cfg), freshBook(), cfg);
        QVERIFY2(allowed.take, qPrintable(allowed.why));

        // The dollar index stays on the list beside it: adding one did not replace it.
        QVERIFY(cfg.reluctantSymbols.contains(QStringLiteral("USDOLLAR")));
    }

    //! @tstid TS-PAPER-028 @design DES-DOM-DAY
    // @relation(REQ-F-031, REQ-F-034, scope=function)
    void TS_PAPER_028_theRecordSaysWhichRuleMadeOrLostTheMoney()
    {
        // The most diagnostic number the record holds, and the one that answered "why
        // is the bot losing" on the first 18 real closes: the fade rule was −97.12 EUR
        // over 7 trades while give-back was +99.10 over 2. A total hides that; a split
        // by exit rule does not.
        QList<PaperClosedTrade> closed;
        const auto add = [&closed](CloseReason reason, double net, int day) {
            PaperClosedTrade c;
            c.symbol = QStringLiteral("SPX500");
            c.isBuy = true;
            c.netPnl = net;
            c.reason = reason;
            c.openTime = QDateTime(QDate(2026, 8, day), QTime(10, 0), QTimeZone::utc());
            c.closeTime = c.openTime.addSecs(3600);
            closed.append(c);
        };
        add(CloseReason::SignalFade, -40.0, 3);
        add(CloseReason::SignalFade, -57.12, 3);
        add(CloseReason::GiveBack, +99.10, 4);
        add(CloseReason::AiExit, -12.0, 4);

        const PaperPerformance perf = paperPerformance(closed, 50000.0, 350.0);
        QCOMPARE(perf.countByReason.value(QStringLiteral("signal faded")), 2);
        QVERIFY(qAbs(perf.netByReason.value(QStringLiteral("signal faded")) + 97.12) < 1e-9);
        QCOMPARE(perf.countByReason.value(QStringLiteral("banked before giving it back")), 1);
        QVERIFY(qAbs(perf.netByReason.value(QStringLiteral("banked before giving it back"))
                     - 99.10)
                < 1e-9);
        QCOMPARE(perf.countByReason.value(QStringLiteral("AI says exit")), 1);
        // The split has to add up to the total, or it is a second opinion rather than a
        // decomposition.
        double sum = 0.0;
        for (auto it = perf.netByReason.cbegin(); it != perf.netByReason.cend(); ++it) {
            sum += it.value();
        }
        QVERIFY(qAbs(sum - perf.netTotal) < 1e-9);
        // A rule that never fired is absent rather than zero — "0.00" reads as
        // "measured and neutral", which would be a different claim.
        QVERIFY(!perf.netByReason.contains(QStringLiteral("stop-loss")));

        // And the fade rule now has to be worth acting on: a position down less than
        // the round trip is left alone, because closing it pays the spread to save
        // nothing (measured: 7 fades, −13.87 average, on a book whose gross was
        // POSITIVE).
        const BotConfig cfg;
        QVERIFY(cfg.fadeMinLossOverCost > 1.0);
        PaperTrade faded = tradeAt(5000.0, true);
        faded.entryConfidence = 60.0;
        faded.entryCompositeConf = 60.0;
        faded.markRate = 4999.5;                    // barely under water
        const QDateTime late =
            faded.openTime.addSecs(qint64{60} * (cfg.minHoldMinutes + 5));
        ExitContext ctx = exitAt(4999.5, 1, 10.0, late);
        ctx.spreadPct = 0.05;                       // a real exit cost to compare against
        QCOMPARE(paperCloseDecision(faded, ctx, cfg), CloseReason::None);
        // …once the loss is worth more than the round trip, the same fade acts.
        faded.markRate = 4950.0;
        const ExitContext deeper = exitAt(4950.0, 1, 10.0, late);
        QCOMPARE(paperCloseDecision(faded, deeper, cfg), CloseReason::SignalFade);
    }

    //! @tstid TS-PAPER-027 @design DES-DOM-WHEN
    // @relation(REQ-F-034, REQ-F-035, scope=function)
    void TS_PAPER_027_everyWordAndEveryClockFamilyIsReachable()
    {
        // The wording tables and the two clock families the phase classifier supports
        // besides Europe: an exit or a refusal nobody can name is one nobody can audit,
        // and a Hong Kong instrument is judged on the Hong Kong clock.
        for (const SessionPhase phase :
             {SessionPhase::Normal, SessionPhase::OpeningChaos, SessionPhase::OpeningBurst,
              SessionPhase::DataWindow, SessionPhase::PolicyWindow, SessionPhase::PowerHour,
              SessionPhase::ClosingBurst}) {
            QVERIFY(!sessionPhaseWord(phase).isEmpty());
        }
        QCOMPARE(sessionPhaseWord(SessionPhase::DataWindow),
                 QStringLiteral("macro-data window"));
        QCOMPARE(sessionPhaseWord(SessionPhase::PowerHour), QStringLiteral("power hour"));
        QCOMPARE(sessionPhaseWord(SessionPhase::ClosingBurst), QStringLiteral("closing burst"));
        for (const DayGate gate :
             {DayGate::Open, DayGate::TargetReached, DayGate::LossLimitReached,
              DayGate::Weekend}) {
            QVERIFY(!dayGateWord(gate).isEmpty());
        }
        QVERIFY(dayGateWord(DayGate::Weekend).contains(QStringLiteral("weekend")));

        // Hong Kong: opens at 09:30 local, which is 03:30 in Berlin — the classifier
        // reads the instrument's OWN exchange, not the user's.
        // 09:35 local is still inside the first quarter hour after the 09:30 open.
        const QDateTime hkOpen(QDate(2026, 8, 4), QTime(9, 35), QTimeZone("Asia/Hong_Kong"));
        QCOMPARE(sessionPhaseFor(QStringLiteral("HKG50"), hkOpen), SessionPhase::OpeningChaos);
        QCOMPARE(sessionPhaseFor(QStringLiteral("HKG50"),
                                 QDateTime(QDate(2026, 8, 4), QTime(10, 5),
                                           QTimeZone("Asia/Hong_Kong"))),
                 SessionPhase::OpeningBurst);
        // …and the user's clock is irrelevant: 09:35 in Berlin is 15:35 in Hong Kong,
        // which is the run into the 16:00 close there.
        QCOMPARE(sessionPhaseFor(QStringLiteral("HKG50"),
                                 QDateTime(QDate(2026, 8, 4), QTime(9, 35),
                                           QTimeZone("Europe/Berlin"))),
                 SessionPhase::ClosingBurst);
        // Small hours in Hong Kong: no session to be at the edge of.
        QCOMPARE(sessionPhaseFor(QStringLiteral("HKG50"),
                                 QDateTime(QDate(2026, 8, 4), QTime(4, 0),
                                           QTimeZone("Asia/Hong_Kong"))),
                 SessionPhase::Normal);

        // The 08:00 Berlin European release slot, on a European instrument.
        QCOMPARE(sessionPhaseFor(QStringLiteral("GER40"),
                                 QDateTime(QDate(2026, 8, 4), QTime(8, 5),
                                           QTimeZone("Europe/Berlin"))),
                 SessionPhase::DataWindow);

        // The confluence refusal names the numbers it was given.
        const BotConfig cfg;
        CandidateInput thin = goodCandidate();
        thin.agreeingReads = 1;
        thin.measuredReads = 5;
        const EntryVerdict refused =
            paperEntryVerdict(thin, buildEntrySignal(thin, cfg), freshBook(), cfg);
        QVERIFY(!refused.take);
        QCOMPARE(refused.code, QStringLiteral("no-confluence"));
        QVERIFY(refused.why.contains(QStringLiteral("1 of 5")));
        // …and a threshold above what could be measured is clamped to it, so an
        // instrument with only two readable feeds is still tradable.
        CandidateInput few = goodCandidate();
        few.agreeingReads = 2;
        few.measuredReads = 2;
        QVERIFY(paperEntryVerdict(few, buildEntrySignal(few, cfg), freshBook(), cfg).take);

        // The bar TRACKS THE NUMBER OF READS instead of being the constant it was when
        // there were five of them. A regression test for a real loosening: the configured
        // 3 is a MAJORITY of five reads and a MINORITY of nine, so every read added to
        // REQ-F-035 silently weakened the bot's main protection. Four of nine agreeing is
        // now refused; five — a majority — is taken.
        CandidateInput minority = goodCandidate();
        minority.agreeingReads = 4;
        minority.measuredReads = 9;
        const EntryVerdict outvoted =
            paperEntryVerdict(minority, buildEntrySignal(minority, cfg), freshBook(), cfg);
        QVERIFY(!outvoted.take);
        QCOMPARE(outvoted.code, QStringLiteral("no-confluence"));
        QVERIFY(outvoted.why.contains(QStringLiteral("needs 5")));
        CandidateInput majority = goodCandidate();
        majority.agreeingReads = 5;
        majority.measuredReads = 9;
        QVERIFY(paperEntryVerdict(majority, buildEntrySignal(majority, cfg), freshBook(), cfg)
                    .take);
        // …and switching the gate off entirely still works: 0 means no confluence
        // requirement at all, majority rule included.
        BotConfig ungated = cfg;
        ungated.minAgreeingReads = 0;
        QVERIFY(paperEntryVerdict(minority, buildEntrySignal(minority, ungated), freshBook(),
                                  ungated)
                    .take);

        // An unclassified instrument keeps its own leverage ceiling and its own bucket.
        QVERIFY(correlationGroup(QStringLiteral("NOT-LISTED")).contains(QStringLiteral("other")));
        QCOMPARE(groupLeverageCap(QStringLiteral("other:NOT-LISTED")), 5);
        // …and the catalog's own ladder is what an uncatalogued caller falls back to.
        CandidateInput gold = goodCandidate();
        gold.symbol = QStringLiteral("Gold.24-7");
        gold.leverageSteps.clear();
        QVERIFY(buildEntrySignal(gold, cfg).leverage <= 8);
    }

    //! @tstid TS-PAPER-026 @design DES-DOM-WHEN
    // @relation(REQ-F-034, REQ-F-035, scope=function)
    void TS_PAPER_026_loudWindowsChurnAndConfluenceAllGateTheEntry()
    {
        const auto berlin = [](int hour, int minute) {
            return QDateTime(QDate(2026, 8, 4), QTime(hour, minute),
                             QTimeZone("Europe/Berlin"));
        };
        const BotConfig cfg;
        CandidateInput quiet = goodCandidate();
        quiet.symbol = QStringLiteral("GER40");
        quiet.now = berlin(13, 0);
        const EntrySignal sig = buildEntrySignal(quiet, cfg);
        const EntryVerdict calm = paperEntryVerdict(quiet, sig, freshBook(), cfg);
        QVERIFY2(calm.take, qPrintable(calm.why));

        // A loud window is a chance AND a risk: the same candidate needs more
        // conviction to enter one, and gets less size when it does.
        QVERIFY(cfg.volatileWindowFactor > 1.0);
        CandidateInput loud = quiet;
        loud.now = berlin(9, 40);                       // the readable part of the open
        const EntryVerdict opening = paperEntryVerdict(loud, sig, freshBook(), cfg);
        QVERIFY2(opening.take, qPrintable(opening.why));   // 40 clears 12 x 1.6
        QVERIFY2(opening.stake < calm.stake,
                 "a volatile window has to be traded smaller, not the same");
        QVERIFY(qAbs(opening.stake - (calm.stake / cfg.volatileWindowFactor)) < 1e-9);

        // …and a candidate that only just cleared the normal floor is refused there,
        // by name, instead of being taken into the noise.
        CandidateInput thin = loud;
        thin.confidence = cfg.minConfidence + 1.0;
        const EntryVerdict refused = paperEntryVerdict(thin, buildEntrySignal(thin, cfg),
                                                       freshBook(), cfg);
        QVERIFY(!refused.take);
        QCOMPARE(refused.code, QStringLiteral("volatile-window"));
        QVERIFY(refused.why.contains(QStringLiteral("opening burst")));

        // The two windows that are sat out entirely rather than traded smaller: the
        // first quarter hour, and the central-bank slot.
        CandidateInput chaos = quiet;
        chaos.now = berlin(9, 5);
        const EntryVerdict inChaos = paperEntryVerdict(chaos, sig, freshBook(), cfg);
        QVERIFY(!inChaos.take);
        QCOMPARE(inChaos.code, QStringLiteral("volatile-window"));
        QVERIFY(inChaos.why.contains(QStringLiteral("first quarter hour")));
        CandidateInput fed = quiet;
        fed.now = berlin(20, 10);
        const EntryVerdict inFed = paperEntryVerdict(fed, sig, freshBook(), cfg);
        QVERIFY(!inFed.take);
        QCOMPARE(inFed.code, QStringLiteral("volatile-window"));
        QVERIFY(inFed.why.contains(QStringLiteral("central-bank")));
        // Both are switchable, and switching them off leaves the elevated bar rather
        // than removing every guard.
        BotConfig trades = cfg;
        trades.avoidOpeningChaos = false;
        trades.avoidPolicyWindow = false;
        QVERIFY(paperEntryVerdict(chaos, sig, freshBook(), trades).take);
        QVERIFY(paperEntryVerdict(fed, sig, freshBook(), trades).take);
        // The same candidate at a quiet hour is fine — the floor itself did not move.
        CandidateInput thinQuiet = thin;
        thinQuiet.now = berlin(13, 0);
        QVERIFY(paperEntryVerdict(thinQuiet, buildEntrySignal(thinQuiet, cfg), freshBook(), cfg)
                    .take);

        // Churn control. An instrument whose position just closed is left alone: two
        // half-spreads per round trip is what turns an edge into fees.
        CandidateInput again = quiet;
        again.lastClosedAt = again.now.addSecs(-qint64{60} * 10);   // ten minutes ago
        const EntryVerdict cooling = paperEntryVerdict(again, sig, freshBook(), cfg);
        QVERIFY(!cooling.take);
        QCOMPARE(cooling.code, QStringLiteral("cooldown"));
        QVERIFY(cooling.why.contains(QStringLiteral("10 min")));
        again.lastClosedAt = again.now.addSecs(-qint64{60} * (cfg.reentryCooldownMinutes + 1));
        QVERIFY(paperEntryVerdict(again, sig, freshBook(), cfg).take);   // cooled off
        BotConfig noCooldown = cfg;
        noCooldown.reentryCooldownMinutes = 0;                 // switched off
        again.lastClosedAt = again.now.addSecs(-60);
        QVERIFY(paperEntryVerdict(again, sig, freshBook(), noCooldown).take);

        // Never INTO a fresh opposite break of the session's opening range: the
        // session has just told everyone which way it is going.
        CandidateInput fighting = quiet;
        fighting.dir = 1;
        fighting.rangeBreakDir = -1;                   // it broke DOWN
        const EntryVerdict against = paperEntryVerdict(fighting, sig, freshBook(), cfg);
        QVERIFY(!against.take);
        QCOMPARE(against.code, QStringLiteral("against-range-break"));
        QVERIFY(against.why.contains(QStringLiteral("DOWN")));
        fighting.rangeBreakDir = 1;                    // …with the break, not into it
        QVERIFY(paperEntryVerdict(fighting, sig, freshBook(), cfg).take);
        BotConfig ignoreRange = cfg;
        ignoreRange.respectOpeningRange = false;
        fighting.rangeBreakDir = -1;
        QVERIFY(paperEntryVerdict(fighting, sig, freshBook(), ignoreRange).take);

        // …and the book as a whole has a pace limit, so one excited scan cannot
        // become a spree.
        CandidateInput busy = quiet;
        busy.opensLastHour = cfg.maxOpensPerHour;
        const EntryVerdict paced = paperEntryVerdict(busy, sig, freshBook(), cfg);
        QVERIFY(!paced.take);
        QCOMPARE(paced.code, QStringLiteral("pace-limit"));
        busy.opensLastHour = cfg.maxOpensPerHour - 1;
        QVERIFY(paperEntryVerdict(busy, sig, freshBook(), cfg).take);
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
        const BotConfig cfg;
        PaperTrade longTrade = tradeAt(5000.0, true);
        longTrade.symbol = QStringLiteral("SPX500");
        // Old enough that the minimum holding time is not what is being tested here.
        const QDateTime later = longTrade.openTime.addSecs(qint64{60} * (cfg.minHoldMinutes + 5));

        AiProposal agree;
        agree.ok = true;
        agree.resolvedSymbol = QStringLiteral("SPX500");
        agree.dir = 1;
        agree.confidence = cfg.aiExitMinConfidence + 10.0;   // convinced enough to act on
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

        // Silence — the common case with a small model — always keeps the position,
        // and says NO OPINION rather than recommending anything: the window shows
        // that state, so it must be distinguishable from an actual "hold".
        const HoldVerdict quiet = paperAiHold(longTrade, {}, BotAiMode::Confirm, later, cfg);
        QVERIFY(!quiet.close);
        QCOMPARE(quiet.opinion, HoldOpinion::NoOpinion);
        QCOMPARE(holdOpinionWord(quiet.opinion), QStringLiteral("—"));
        QCOMPARE(paperAiHold(longTrade, {elsewhere}, BotAiMode::Confirm, later, cfg).opinion,
                 HoldOpinion::NoOpinion);
        // An explicit HOLD is "no action" — but it IS an opinion about this trade.
        const HoldVerdict held = paperAiHold(longTrade, {hold}, BotAiMode::Confirm, later, cfg);
        QVERIFY(!held.close);
        QCOMPARE(held.opinion, HoldOpinion::Hold);
        QCOMPARE(held.code, QStringLiteral("ai-keep"));
        QCOMPARE(holdOpinionWord(held.opinion), QStringLiteral("hold"));
        // Agreement on the side keeps it too, and says so.
        const HoldVerdict agreed = paperAiHold(longTrade, {agree}, BotAiMode::Lead, later, cfg);
        QVERIFY(!agreed.close);
        QCOMPARE(agreed.opinion, HoldOpinion::Hold);

        // The other side, or an explicit CLOSE, closes it — with a countable code.
        const HoldVerdict reversed = paperAiHold(longTrade, {reverse}, BotAiMode::Confirm, later, cfg);
        QVERIFY(reversed.close);
        QCOMPARE(reversed.opinion, HoldOpinion::Close);
        QCOMPARE(holdOpinionWord(reversed.opinion), QStringLiteral("close"));
        QCOMPARE(reversed.code, QStringLiteral("ai-reversed"));
        QVERIFY(reversed.why.contains(QStringLiteral("momentum has turned")));
        const HoldVerdict asked = paperAiHold(longTrade, {closeIt}, BotAiMode::Lead, later, cfg);
        QVERIFY(asked.close);
        QCOMPARE(asked.code, QStringLiteral("ai-close"));

        // A SHORT is the mirror image: a BUY pick is the contrary opinion.
        PaperTrade shortTrade = tradeAt(5000.0, false);
        shortTrade.symbol = QStringLiteral("SPX500");
        QVERIFY(paperAiHold(shortTrade, {agree}, BotAiMode::Confirm, later, cfg).close);
        QVERIFY(!paperAiHold(shortTrade, {reverse}, BotAiMode::Confirm, later, cfg).close);

        // With the model switched off nobody was asked, so nothing it "said" counts —
        // and the flag stays empty rather than claiming a recommendation.
        const HoldVerdict silent = paperAiHold(longTrade, {reverse, closeIt}, BotAiMode::Off, later, cfg);
        QVERIFY(!silent.close);
        QCOMPARE(silent.opinion, HoldOpinion::NoOpinion);

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
        // The COMPOSITE's number is what the fade rule reads — in lead mode
        // entryConfidence is the model's, on its own scale.
        faded.entryCompositeConf = 60.0;
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
        gaveBack.entryCompositeConf = 60.0;
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
        down.now = QDateTime(QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC);
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
    //! @tstid TS-PT-030 @design DES-DOM-DAY
    // @relation(REQ-F-031, scope=function)
    void TS_PT_030_theDayGateAndItsCodesAnswerForEveryState()
    {
        BotConfig cfg;
        cfg.dailyProfitTarget = 350.0;
        cfg.dailyLossLimit = 350.0;
        cfg.tradeWeekdaysOnly = true;
        const QDateTime tuesday(QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC);
        const QDateTime saturday(QDate(2026, 8, 8), QTime(11, 0), QTimeZone::UTC);

        BotDay day;
        day.date = tuesday.date();
        day.realized = 0.0;
        QCOMPARE(paperDayGate(day, tuesday, cfg), DayGate::Open);
        // The weekend outranks everything, including a day that made its target.
        day.realized = 400.0;
        QCOMPARE(paperDayGate(day, saturday, cfg), DayGate::Weekend);
        // …and with weekend trading allowed, Saturday is just another day.
        BotConfig anyDay = cfg;
        anyDay.tradeWeekdaysOnly = false;
        BotDay satDay;
        satDay.date = saturday.date();
        satDay.realized = 0.0;
        QCOMPARE(paperDayGate(satDay, saturday, anyDay), DayGate::Open);

        // Target and loss limit, each at its own boundary.
        day.date = tuesday.date();
        day.realized = 349.99;
        QCOMPARE(paperDayGate(day, tuesday, cfg), DayGate::Open);
        day.realized = 350.0;
        QCOMPARE(paperDayGate(day, tuesday, cfg), DayGate::TargetReached);
        day.realized = -350.0;
        QCOMPARE(paperDayGate(day, tuesday, cfg), DayGate::LossLimitReached);
        day.realized = -349.99;
        QCOMPARE(paperDayGate(day, tuesday, cfg), DayGate::Open);
        // A rule switched off cannot fire, however far the day went.
        BotConfig noRules = cfg;
        noRules.dailyProfitTarget = 0.0;
        noRules.dailyLossLimit = 0.0;
        day.realized = 10000.0;
        QCOMPARE(paperDayGate(day, tuesday, noRules), DayGate::Open);
        day.realized = -10000.0;
        QCOMPARE(paperDayGate(day, tuesday, noRules), DayGate::Open);
        // Yesterday's ledger does not govern today, and an unknown clock cannot
        // close the day either.
        day.date = tuesday.date().addDays(-1);
        day.realized = 5000.0;
        QCOMPARE(paperDayGate(day, tuesday, cfg), DayGate::Open);
        QCOMPARE(paperDayGate(day, QDateTime(), cfg), DayGate::Open);

        // Every gate reaches the entry verdict with its own countable code — the codes
        // themselves are file-local, so they are asserted where a reader of the scan
        // summary would see them.
        const BotConfig entryCfg = cfg;
        const CandidateInput candidate = goodCandidate();
        const EntrySignal sig = buildEntrySignal(candidate, entryCfg);
        BookState book = freshBook();
        book.day.date = candidate.now.date();

        book.day.realized = 400.0;
        QCOMPARE(paperEntryVerdict(candidate, sig, book, entryCfg).code,
                 QStringLiteral("day-target"));
        book.day.realized = -400.0;
        QCOMPARE(paperEntryVerdict(candidate, sig, book, entryCfg).code,
                 QStringLiteral("day-loss"));
        book.day.realized = 0.0;
        QVERIFY(paperEntryVerdict(candidate, sig, book, entryCfg).take);

        CandidateInput onSaturday = candidate;
        onSaturday.now = saturday;
        BookState satBook = freshBook();
        satBook.day.date = saturday.date();
        QCOMPARE(paperEntryVerdict(onSaturday, buildEntrySignal(onSaturday, entryCfg), satBook,
                                   entryCfg)
                     .code,
                 QStringLiteral("weekend"));
    }

    //! @tstid TS-PT-031 @design DES-DOM-DAY
    // @relation(REQ-F-031, scope=function)
    void TS_PT_031_harvestPicksTheSmallestSufficientWinnerOrNothing()
    {
        BotConfig cfg;
        cfg.dailyProfitTarget = 100.0;
        cfg.harvestForDailyTarget = true;
        BotDay day;
        day.realized = 40.0;   // 60 still missing

        // Nothing on offer covers the rest of the day: nothing is closed.
        QCOMPARE(paperHarvestPick({{1, 10.0}, {2, 59.99}}, day, cfg), 0);
        // Two that do: the SMALLER is taken, because it gives up the least upside.
        QCOMPARE(paperHarvestPick({{1, 500.0}, {2, 61.0}, {3, 60.0}}, day, cfg), 3);
        // The day is already made — the day gate stops the bot, not the harvest.
        BotDay made;
        made.realized = 100.0;
        QCOMPARE(paperHarvestPick({{1, 500.0}}, made, cfg), 0);
        // The rule can be switched off, and a target of zero disables it too.
        BotConfig off = cfg;
        off.harvestForDailyTarget = false;
        QCOMPARE(paperHarvestPick({{1, 500.0}}, day, off), 0);
        BotConfig noTarget = cfg;
        noTarget.dailyProfitTarget = 0.0;
        QCOMPARE(paperHarvestPick({{1, 500.0}}, day, noTarget), 0);
        // No options at all is not a crash.
        QCOMPARE(paperHarvestPick({}, day, cfg), 0);
    }

    //! @tstid TS-PT-032 @design DES-DOM-LIFECYCLE
    // @relation(REQ-F-032, scope=function)
    void TS_PT_032_theHoldReviewAnswersOnlyWhenItWasAsked()
    {
        BotConfig cfg;
        cfg.minHoldMinutes = 30;
        cfg.aiExitMinConfidence = 60.0;
        const QDateTime opened(QDate(2026, 8, 4), QTime(10, 0), QTimeZone::UTC);
        const QDateTime later = opened.addSecs(qint64{60} * 60);

        PaperTrade trade;
        trade.id = 1;
        trade.symbol = QStringLiteral("SPX500");
        trade.isBuy = true;
        trade.openTime = opened;

        // OFF: nobody was asked, so there is no opinion — not a silent keep.
        QCOMPARE(paperAiHold(trade, {}, BotAiMode::Off, later, cfg).opinion,
                 HoldOpinion::NoOpinion);
        // Asked, but the model named other instruments: still NO opinion. Silence
        // must never close a position.
        AiProposal other;
        other.ok = true;
        other.resolvedSymbol = QStringLiteral("GOLD");
        other.dir = -1;
        other.confidence = 90.0;
        const HoldVerdict silent = paperAiHold(trade, {other}, BotAiMode::Lead, later, cfg);
        QCOMPARE(silent.opinion, HoldOpinion::NoOpinion);
        QVERIFY(!silent.close);
        // A proposal that failed to parse is not an opinion either.
        AiProposal broken;
        broken.ok = false;
        broken.resolvedSymbol = QStringLiteral("SPX500");
        broken.exitNow = true;
        QCOMPARE(paperAiHold(trade, {broken}, BotAiMode::Lead, later, cfg).opinion,
                 HoldOpinion::NoOpinion);

        // Named, same side: a KEEP with the model behind it.
        AiProposal keep;
        keep.ok = true;
        keep.resolvedSymbol = QStringLiteral("SPX500");
        keep.dir = 1;
        keep.confidence = 80.0;
        const HoldVerdict kept = paperAiHold(trade, {keep}, BotAiMode::Lead, later, cfg);
        QCOMPARE(kept.opinion, HoldOpinion::Hold);
        QCOMPARE(kept.code, QStringLiteral("ai-keep"));
        QVERIFY(!kept.close);
        // An explicit HOLD (no direction) is also a keep, and says so in words.
        AiProposal flat = keep;
        flat.dir = 0;
        QCOMPARE(paperAiHold(trade, {flat}, BotAiMode::Lead, later, cfg).opinion,
                 HoldOpinion::Hold);

        // Contrary and convinced, held long enough: a close.
        AiProposal against = keep;
        against.dir = -1;
        const HoldVerdict closes = paperAiHold(trade, {against}, BotAiMode::Lead, later, cfg);
        QCOMPARE(closes.opinion, HoldOpinion::Close);
        QVERIFY(closes.close);
        QCOMPARE(closes.code, QStringLiteral("ai-reversed"));
        // An explicit CLOSE reads the same way but names itself differently.
        AiProposal exit = keep;
        exit.exitNow = true;
        QCOMPARE(paperAiHold(trade, {exit}, BotAiMode::Lead, later, cfg).code,
                 QStringLiteral("ai-close"));

        // Too soon, or not convinced enough: the opinion is REPORTED but does not
        // close — hiding it would make the bot look broken.
        const QDateTime tooSoon = opened.addSecs(qint64{5} * 60);
        const HoldVerdict early = paperAiHold(trade, {against}, BotAiMode::Lead, tooSoon, cfg);
        QCOMPARE(early.opinion, HoldOpinion::Close);
        QVERIFY(!early.close);
        QCOMPARE(early.code, QStringLiteral("ai-too-soon"));
        AiProposal unsure = against;
        unsure.confidence = 30.0;
        const HoldVerdict weak = paperAiHold(trade, {unsure}, BotAiMode::Lead, later, cfg);
        QCOMPARE(weak.opinion, HoldOpinion::Close);
        QVERIFY(!weak.close);
        // An unknown clock counts as old enough — the alternative is a position
        // nothing can ever close.
        PaperTrade timeless = trade;
        timeless.openTime = QDateTime();
        QVERIFY(paperAiHold(timeless, {against}, BotAiMode::Lead, later, cfg).close);
        QVERIFY(paperAiHold(trade, {against}, BotAiMode::Lead, QDateTime(), cfg).close);

        // A SHORT position mirrors all of it: a long call is what contradicts it.
        PaperTrade shortTrade = trade;
        shortTrade.isBuy = false;
        QCOMPARE(paperAiHold(shortTrade, {keep}, BotAiMode::Lead, later, cfg).opinion,
                 HoldOpinion::Close);
        QCOMPARE(paperAiHold(shortTrade, {against}, BotAiMode::Lead, later, cfg).opinion,
                 HoldOpinion::Hold);
    }
    //! @tstid TS-PT-033 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PT_033_theArithmeticRefusesWhatItCannotCompute()
    {
        // The pure money arithmetic, at the edges where a naive implementation
        // divides by zero or invents a number. Every one of these returns "nothing
        // measured" rather than a plausible figure — the same rule the feeds follow.

        // Gross P/L needs both rates; either missing is not a zero move.
        QCOMPARE(paperGrossPnl(1000.0, 10, 0.0, 5000.0, true), 0.0);
        QCOMPARE(paperGrossPnl(1000.0, 10, 5000.0, 0.0, true), 0.0);
        QVERIFY(paperGrossPnl(1000.0, 10, 5000.0, 5050.0, true) > 0.0);
        // …and the SHORT is the mirror, not a copy: the same move loses money.
        QCOMPARE(paperGrossPnl(1000.0, 10, 5000.0, 5050.0, false),
                 -paperGrossPnl(1000.0, 10, 5000.0, 5050.0, true));
        // A leverage below 1 is treated as 1 rather than shrinking the position.
        QCOMPARE(paperGrossPnl(1000.0, 0, 5000.0, 5050.0, true),
                 paperGrossPnl(1000.0, 1, 5000.0, 5050.0, true));

        // Rollover nights: an invalid or backwards interval is zero nights, never a
        // negative charge.
        const QDateTime monday(QDate(2026, 8, 3), QTime(15, 0), QTimeZone::UTC);
        QCOMPARE(paperRolloverNights(QDateTime(), monday), 0);
        QCOMPARE(paperRolloverNights(monday, QDateTime()), 0);
        QCOMPARE(paperRolloverNights(monday, monday), 0);
        QCOMPARE(paperRolloverNights(monday.addDays(1), monday), 0);
        QCOMPARE(paperRolloverNights(monday, monday.addDays(1)), 1);

        // A trade with no stop has no measurable risk-at-stop — which the risk budget
        // must see as zero rather than as "safe".
        PaperTrade open;
        open.stake = 1000.0;
        open.leverage = 10;
        open.openRate = 5000.0;
        open.slRate = 0.0;
        QCOMPARE(open.riskAtStop(), 0.0);
        open.slRate = 4900.0;
        QVERIFY(open.riskAtStop() > 0.0);
        open.openRate = 0.0;
        QCOMPARE(open.riskAtStop(), 0.0);

        // A closed trade with a missing timestamp has no holding time — the record
        // shows it as unknown rather than as an instant round trip.
        PaperClosedTrade closed;
        QCOMPARE(closed.heldHours(), 0.0);
        closed.openTime = monday;
        QCOMPARE(closed.heldHours(), 0.0);
        closed.closeTime = monday.addSecs(5400);
        QCOMPARE(closed.heldHours(), 1.5);
    }

    //! @tstid TS-PT-034 @design DES-DOM-RISKGRP
    // @relation(REQ-F-031, scope=function)
    void TS_PT_034_everyCorrelationBucketIsReachableAndNamed()
    {
        // The buckets decide whether twelve positions are a diversified book or one
        // bet twelve times, so each has to be reachable from a real catalog symbol —
        // and a symbol the catalog does not know must get its OWN bucket rather than
        // being lumped in with something it does not move with.
        QCOMPARE(correlationGroup(QStringLiteral("SPX500")), QStringLiteral("equity-index"));
        QCOMPARE(correlationGroup(QStringLiteral("NSDQ100")), QStringLiteral("equity-index"));
        QCOMPARE(correlationGroup(QStringLiteral("EURUSD")), QStringLiteral("fx"));
        // The documented exception: a dollar INDEX is fx, not an equity index.
        QCOMPARE(correlationGroup(QStringLiteral("USDOLLAR")), QStringLiteral("fx"));
        // The catalog's own spellings: Gold.24-7 and OIL.24-7, not "GOLD"/"OIL" — a
        // symbol the catalog does not list gets its own bucket, which is exactly what
        // the first version of this test tripped over.
        QCOMPARE(correlationGroup(QStringLiteral("Gold.24-7")), QStringLiteral("metals"));
        QCOMPARE(correlationGroup(QStringLiteral("OIL.24-7")), QStringLiteral("commodity"));
        QCOMPARE(correlationGroup(QStringLiteral("RUBBER")), QStringLiteral("commodity"));
        // Unknown symbols get their own bucket, named after themselves, so they can
        // never share a risk pool with something they were never compared to.
        const QString unknown = correlationGroup(QStringLiteral("NOSUCHTHING"));
        QVERIFY(!unknown.isEmpty());
        QVERIFY(unknown != QStringLiteral("equity-index"));
        QVERIFY(unknown != correlationGroup(QStringLiteral("ALSONOTATHING")));

        // The per-bucket leverage ceilings, each one reachable and FX the tightest —
        // the rule that keeps a 1:30 forex pair from being sized like an index.
        QVERIFY(groupLeverageCap(QStringLiteral("fx"))
                <= groupLeverageCap(QStringLiteral("equity-index")));
        QVERIFY(groupLeverageCap(QStringLiteral("metals")) > 0);
        QVERIFY(groupLeverageCap(QStringLiteral("commodity")) > 0);
        QVERIFY(groupLeverageCap(QStringLiteral("something-else")) > 0);
    }
    //! @tstid TS-PT-035 @design DES-DOM-WHEN
    // @relation(REQ-F-034, scope=function)
    void TS_PT_035_everyRuleCanBeSwitchedOffAndSaysNothingWhenItIs()
    {
        // Each of the bot's brakes is a NUMBER, and every one of them can be set to
        // zero to disable its rule. That is not decoration: an experiment that cannot
        // isolate one rule cannot attribute a result to it. The off-position of each
        // is exercised here, because a guard that silently keeps firing when disabled
        // would quietly invalidate every such experiment.
        BotConfig off;
        off.maxOpensPerHour = 0;          // no pace limit
        off.reentryCooldownMinutes = 0;   // no cooldown
        off.minAgreeingReads = 0;         // no confluence requirement
        off.maxGroupRiskFraction = 0.0;   // no per-bucket cap
        off.maxSymbolRiskFraction = 0.0;  // no per-symbol cap
        off.dailyProfitTarget = 0.0;
        off.dailyLossLimit = 0.0;
        off.minStake = 0.0;

        const CandidateInput in = goodCandidate();
        BookState book = freshBook();
        book.day.date = in.now.date();
        // A book that would trip every disabled rule: recent opens, a recent close in
        // this very symbol, and a day already far past both limits.
        book.day.realized = 100000.0;
        book.day.opened = 99;
        // The pace and cooldown facts live on the CANDIDATE, not on the book.
        CandidateInput busy = in;
        busy.opensLastHour = 99;
        busy.lastClosedAt = in.now.addSecs(-60);
        const EntryVerdict verdict = paperEntryVerdict(busy, buildEntrySignal(busy, off), book,
                                                       off);
        QVERIFY2(verdict.take, qPrintable(verdict.code + QStringLiteral(": ") + verdict.why));

        // …and each rule switched back ON alone produces its own refusal code, so the
        // codes in the scan summary can be attributed to a rule rather than guessed at.
        struct Case {
            const char *what;
            BotConfig cfg;
            const char *code;
        };
        BotConfig pace = off;
        pace.maxOpensPerHour = 1;
        BotConfig cooldown = off;
        cooldown.reentryCooldownMinutes = 45;
        BotConfig target = off;
        target.dailyProfitTarget = 350.0;
        BotConfig loss = off;
        loss.dailyLossLimit = 350.0;
        const QList<Case> cases{{"pace limit", pace, "pace-limit"},
                                {"re-entry cooldown", cooldown, "cooldown"},
                                {"daily target", target, "day-target"}};
        for (const Case &c : cases) {
            const EntryVerdict v =
                paperEntryVerdict(busy, buildEntrySignal(busy, c.cfg), book, c.cfg);
            QVERIFY2(!v.take, c.what);
            QVERIFY2(!v.code.isEmpty(), c.what);
            QCOMPARE(v.code, QString::fromLatin1(c.code));
        }
        // The loss limit needs a losing day rather than a winning one to fire.
        BookState losing = book;
        losing.day.realized = -100000.0;
        const EntryVerdict lossVerdict =
            paperEntryVerdict(busy, buildEntrySignal(busy, loss), losing, loss);
        QVERIFY(!lossVerdict.take);
        QCOMPARE(lossVerdict.code, QStringLiteral("day-loss"));

        // A candidate with NO direction is refused whatever the rules say — there is
        // nothing to trade, not merely nothing allowed.
        CandidateInput flat = in;
        flat.dir = 0;
        flat.confidence = 0.0;
        const EntryVerdict noDir =
            paperEntryVerdict(flat, buildEntrySignal(flat, off), freshBook(), off);
        QVERIFY(!noDir.take);
        QVERIFY(!noDir.code.isEmpty());
    }
    //! @tstid TS-PT-036 @design DES-DOM-PAPER
    // @relation(REQ-F-029, scope=function)
    void TS_PT_036_theBookRefusesEveryOperationOnATradeItDoesNotHold()
    {
        // The book is addressed by trade id from several places — the marking timer,
        // the rollover accrual, the exit evaluation. An id it does not hold must be a
        // no-op EVERYWHERE, because the alternative is writing to whichever trade
        // happens to sit at that index.
        const BotConfig cfg;
        PaperBook book(cfg);
        const QDateTime now(QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC);

        // Opening refuses what it cannot price: an invalid signal, no stake, no fill.
        const EntrySignal bad;
        QCOMPARE(book.open(bad, 1000.0, now), 0);
        const EntrySignal good = buildEntrySignal(goodCandidate(), cfg);
        QVERIFY(good.valid);
        QCOMPARE(book.open(good, 0.0, now), 0);
        EntrySignal unpriced = good;
        unpriced.fillRate = 0.0;
        QCOMPARE(book.open(unpriced, 1000.0, now), 0);

        // A real open returns a NON-ZERO id, and the day ledger counts it.
        const qint64 id = book.open(good, 1000.0, now);
        QVERIFY(id != 0);
        QCOMPARE(book.state().openCount, 1);

        // Every by-id operation on an id the book does not hold changes nothing.
        const BookState before = book.state();
        book.setFeatures(9999, EntryFeatures{});
        book.setEntryCompositeConf(9999, 42.0);
        book.mark(9999, 5100.0, true, now);
        book.accrueRollover(9999, InstrumentFees{}, 1.0, now);
        QCOMPARE(book.state().openCount, before.openCount);
        QCOMPARE(book.state().equity, before.equity);
        QCOMPARE(book.state().openRisk, before.openRisk);

        // …and the same operations on the REAL id do change something.
        book.setEntryCompositeConf(id, 42.0);
        book.mark(id, good.fillRate * 1.01, true, now.addSecs(600));
        QVERIFY(book.state().equity > 0.0);

        // Marking at a non-positive rate is refused rather than booking a loss of
        // everything, and rollover of zero nights charges nothing.
        const BookState marked = book.state();
        book.mark(id, 0.0, true, now.addSecs(700));
        QCOMPARE(book.state().equity, marked.equity);
        // POSITIVE per-unit fees are a CHARGE; negative ones are a credit paid to the
        // holder (eToro's own convention, and the reason a credit never triggers the
        // carry exit). Both directions are exercised below.
        InstrumentFees fees;
        fees.buyOvernight = 0.5;
        fees.sellOvernight = 0.4;
        fees.buyWeekend = 1.5;
        fees.sellWeekend = 1.2;
        // No night has passed yet, so nothing is charged…
        book.accrueRollover(id, fees, 1.0, now.addSecs(800));
        QCOMPARE(book.state().equity, marked.equity);
        // …and a night that HAS passed costs money.
        book.accrueRollover(id, fees, 1.0, now.addDays(1));
        QVERIFY(book.state().equity < marked.equity);

        // A CREDIT moves it the other way: being paid to hold is not a cost, which is
        // exactly why the carry exit stays silent for one.
        const BookState charged = book.state();
        InstrumentFees credit;
        credit.buyOvernight = -0.5;
        credit.sellOvernight = -0.4;
        credit.buyWeekend = -1.5;
        credit.sellWeekend = -1.2;
        book.accrueRollover(id, credit, 1.0, now.addDays(2));
        QVERIFY(book.state().equity > charged.equity);

        // Closing an id the book does not hold is a no-op; closing the real one at a
        // non-positive rate is refused; closing it properly moves it to the record.
        QCOMPARE(book.close(9999, 5100.0, 0.02, CloseReason::AiExit, now.addSecs(1000)).id, 0);
        QCOMPARE(book.state().openCount, 1);
        // Closing the real one WORKS, and a close rate of zero is not a refusal: it
        // falls back to the position's last mark, because a forced close with no fresh
        // price still has to close — leaving it open would be the worse failure.
        const PaperClosedTrade record =
            book.close(id, good.fillRate * 1.02, 0.02, CloseReason::TakeProfit,
                       now.addSecs(3600));
        QCOMPARE(record.id, id);
        QCOMPARE(book.state().openCount, 0);

        PaperBook forced(cfg);
        const qint64 forcedId = forced.open(good, 1000.0, now);
        forced.mark(forcedId, good.fillRate * 1.03, true, now.addSecs(60));
        const PaperClosedTrade atLastMark =
            forced.close(forcedId, 0.0, 0.02, CloseReason::AiExit, now.addSecs(120));
        QCOMPARE(atLastMark.id, forcedId);
        QCOMPARE(forced.state().openCount, 0);
        QVERIFY(atLastMark.closeRate > 0.0);
        QCOMPARE(book.closedTrades().size(), 1);
        QCOMPARE(book.closedTrades().constFirst().reason, CloseReason::TakeProfit);
        QVERIFY(book.closedTrades().constFirst().heldHours() > 0.0);

        // An invalid clock at open time leaves the day ledger alone rather than
        // rolling it to an unknown date.
        PaperBook timeless(cfg);
        QVERIFY(timeless.open(good, 500.0, QDateTime()) != 0);
        QCOMPARE(timeless.state().openCount, 1);
    }
};

QTEST_GUILESS_MAIN(TestPaperTrader)
#include "tst_papertrader.moc"
