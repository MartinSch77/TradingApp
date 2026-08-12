// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The prediction ledger (DES-DOM-LEDGER, REQ-F-037): the record that turns an evidence
// score into a probability, and the scoring that is allowed to say "no".
//
// What is tested here is not that a number comes out. It is the four properties that
// decide whether the number may be believed: a probability is refused until enough
// COMPARABLE calls have been resolved, an outcome is never manufactured out of a gap
// that is not the horizon asked about, the baselines are measured on exactly the same
// samples as the app's own call, and the rows that decided to STAY OUT survive the
// round trip — because a record of only the trades taken measures the gate, not the
// signal.

#include "domain/PredictionLedger.h"

#include <QTemporaryDir>
#include <QTest>

using namespace trading;

namespace {

QDateTime base()
{
    return {QDate(2026, 8, 4), QTime(14, 0), QTimeZone::UTC};
}

Prediction callAt(qint32 minute, qint32 dir, double price, double strength = 50.0)
{
    Prediction p;
    p.at = base().addSecs(qint64{minute} * 60);
    p.symbol = QStringLiteral("NSDQ100");
    p.dir = dir;
    p.strength = strength;
    p.measured = 9;
    p.price = price;
    p.regime = Regime::Trend;
    p.priorMoveDir = 1;
    p.vwapSide = 1;
    return p;
}

// A ledger of `count` five-minute-spaced calls in which the app's own direction is right
// `rightEvery`-th time. Prices move by ±0.1% so every outcome has a clear sign.
QList<Prediction> ledgerWith(qint32 count, qint32 rightEvery, double strength)
{
    QList<Prediction> out;
    double price = 20000.0;
    for (qint32 i = 0; i < count; ++i) {
        const bool shouldBeRight = ((i % rightEvery) == 0);
        // The call is recorded first, then the price moves to make it right or wrong.
        const Prediction call = callAt(i * 5, 1, price, strength);
        out.append(call);
        price *= shouldBeRight ? 1.001 : 0.999;
    }
    // One final row so the last call can be resolved too.
    out.append(callAt(count * 5, 1, price, strength));
    return out;
}

} // namespace

class TestPredictionLedger : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-LEDGER-001 @design DES-DOM-LEDGER
    // @relation(REQ-F-037, scope=function)
    void TS_LEDGER_001_anOutcomeIsOnlyResolvedAgainstTheHorizonAsked()
    {
        const Prediction call = callAt(0, 1, 20000.0);

        // A row fifteen minutes later resolves the 15-minute horizon, and the outcome
        // carries the REAL gap rather than the nominal one.
        const QList<Prediction> later{callAt(15, 1, 20100.0)};
        const std::optional<Outcome> fifteen = resolveOutcome(call, Horizon::M15, later);
        QVERIFY(fifteen.has_value());
        const Outcome atFifteen = fifteen.value_or(Outcome{});
        QCOMPARE(atFifteen.actualDir, 1);
        QVERIFY(qAbs(atFifteen.movePct - 0.5) < 1e-9);
        QCOMPARE(atFifteen.elapsedMinutes, 15);

        // The same row cannot resolve the 5-minute horizon: fifteen minutes is more than
        // half again past it, so it is not an answer to the question asked.
        QVERIFY(!resolveOutcome(call, Horizon::M5, later).has_value());

        // The EARLIEST qualifying row wins. With rows at 5 and 7 minutes, the 5-minute
        // horizon must use the 5-minute one — taking the latest available would silently
        // lengthen every horizon whenever the ledger is dense.
        const QList<Prediction> dense{callAt(7, 1, 20500.0), callAt(5, 1, 20100.0)};
        const std::optional<Outcome> five = resolveOutcome(call, Horizon::M5, dense);
        QVERIFY(five.has_value());
        QCOMPARE(five.value_or(Outcome{}).elapsedMinutes, 5);

        // An overnight gap is not a 5-minute outcome, however tempting the pairing.
        const QList<Prediction> tomorrow{callAt(20 * 60, 1, 21000.0)};
        QVERIFY(!resolveOutcome(call, Horizon::M5, tomorrow).has_value());
        QVERIFY(!resolveOutcome(call, Horizon::M180, tomorrow).has_value());

        // Another instrument's rows are not this instrument's history.
        Prediction other = callAt(15, 1, 20100.0);
        other.symbol = QStringLiteral("SPX500");
        QVERIFY(!resolveOutcome(call, Horizon::M15, {other}).has_value());

        // An invalid row resolves nothing, and an unpriced call cannot be resolved at all.
        const Prediction unpriced = callAt(0, 1, 0.0);
        QVERIFY(!unpriced.isValid());
        QVERIFY(!resolveOutcome(unpriced, Horizon::M5, later).has_value());
    }

    //! @tstid TS-LEDGER-002 @design DES-DOM-LEDGER
    // @relation(REQ-F-037, scope=function)
    void TS_LEDGER_002_noProbabilityIsClaimedBeforeTheRecordSupportsOne()
    {
        // THE rule. A thin record must produce an explicit refusal, never a placeholder
        // percentage — the same discipline the live-money gate uses.
        const QList<Prediction> thin = ledgerWith(4, 1, 50.0);
        const Prediction now = callAt(100, 1, 20000.0, 50.0);
        const QList<double> bars = [] {
            QList<double> out;
            for (int i = 0; i < 120; ++i) {
                out.append(20000.0 * (1.0 + (((i % 5) - 2) * 0.0002)));
            }
            return out;
        }();

        const QList<HorizonProbability> thinAnswers = horizonProbabilities(now, thin, bars);
        QCOMPARE(thinAnswers.size(), 4);
        for (const HorizonProbability &answer : thinAnswers) {
            QVERIFY(!answer.calibrated);
            QVERIFY(answer.sentence.contains(QStringLiteral("UNCALIBRATED")));
            // The expected RANGE is still reported: how far it can move is a property of
            // the series and needs no record.
            QVERIFY(answer.rangeKnown);
            QVERIFY(answer.p5 < answer.p95);
        }

        // A record with enough comparable calls in the same strength band does produce a
        // number — and the number is the MEASURED frequency of that band, not the score.
        // Two of every three calls right, at strength 50, over enough samples.
        const QList<Prediction> rich = ledgerWith(90, 2, 50.0);
        const QList<HorizonProbability> answers = horizonProbabilities(now, rich, bars);
        const HorizonProbability &five = answers.constFirst();
        QCOMPARE(five.horizon, Horizon::M5);
        QVERIFY(five.calibrated);
        QVERIFY(five.samples >= 15);
        QVERIFY(qAbs(five.pUp - 50.0) < 15.0);   // near the constructed hit rate, not the strength
        QVERIFY(five.sentence.contains(QStringLiteral("comparable calls")));

        // A SHORT call in the same band reports the complement: the question is P(up),
        // so a 60%-right short is a 40% chance of being higher.
        Prediction shortCall = now;
        shortCall.dir = -1;
        const HorizonProbability shortAnswer =
            horizonProbabilities(shortCall, rich, bars).constFirst();
        QVERIFY(shortAnswer.calibrated);
        QVERIFY(qAbs((shortAnswer.pUp + five.pUp) - 100.0) < 1e-9);

        // A row with NO call gets no probability, however rich the record: there is no
        // side whose frequency could be quoted.
        Prediction stayOut = now;
        stayOut.dir = 0;
        for (const HorizonProbability &answer : horizonProbabilities(stayOut, rich, bars)) {
            QVERIFY(!answer.calibrated);
        }
    }

    //! @tstid TS-LEDGER-003 @design DES-DOM-LEDGER
    // @relation(REQ-F-037, scope=function)
    void TS_LEDGER_003_theBaselinesAreScoredOnTheSameSamples()
    {
        // A hit rate without a baseline is a boast. This is the test that the comparison
        // is honest: the same resolved rows, four ways of picking a side.
        //
        // Every call here is long, and the market rises two thirds of the time, so
        // "always long" scores exactly what the app's own call does — which is precisely
        // the embarrassing case the baseline exists to expose.
        const QList<Prediction> history = ledgerWith(90, 2, 70.0);
        const HorizonScore score = scoreHorizon(history, Horizon::M5);
        QVERIFY(score.trustworthy());
        QVERIFY(score.samples >= 40);
        QCOMPARE(score.hitRate, score.alwaysLongHitRate);
        QVERIFY(!score.beatsBaselines());   // it ties, and a tie is not a win
        QVERIFY(score.headline().contains(QStringLiteral("does NOT beat every measured baseline")));
        QVERIFY(score.headline().contains(QStringLiteral("always-long")));

        // The Brier score is a real number about the confidence, and a 70-strength call
        // that is wrong a third of the time is worse than saying 50% every time (0.25).
        QVERIFY(score.brier > 0.0);
        QVERIFY(score.brier > 0.25);

        // Each baseline reports the number of rows on which it HAD a side. The VWAP side
        // is unknown for an index CFD (no volume on its candles), and an unmeasurable
        // baseline must be NAMED as such rather than scored 0% and counted as beaten —
        // otherwise a missing measurement becomes a victory.
        QCOMPARE(score.alwaysLongSamples, score.samples);
        QVERIFY(score.priorMoveSamples > 0);
        QList<Prediction> noVwap = history;
        for (Prediction &row : noVwap) {
            row.vwapSide = 0;
        }
        const HorizonScore blind = scoreHorizon(noVwap, Horizon::M5);
        QCOMPARE(blind.vwapSideSamples, 0);
        QCOMPARE(blind.vwapSideHitRate, 0.0);
        QVERIFY(blind.headline().contains(QStringLiteral("VWAP side not measurable")));
        // …and it is still not "beating" that baseline, because the one it ties with
        // (always-long) was measured.
        QVERIFY(!blind.beatsBaselines());

        // A thin record reports itself untrustworthy and makes no claim either way.
        const HorizonScore thin = scoreHorizon(ledgerWith(6, 2, 70.0), Horizon::M5);
        QVERIFY(!thin.trustworthy());
        QVERIFY(!thin.beatsBaselines());
        QVERIFY(thin.headline().contains(QStringLiteral("no claim yet")));

        // An empty record is not an error and not a zero-percent hit rate: it is nothing
        // measured, and the Brier score stays at the coin-flip reference.
        const HorizonScore empty = scoreHorizon({}, Horizon::M60);
        QCOMPARE(empty.samples, 0);
        QCOMPARE(empty.hits, 0);
        QCOMPARE(empty.brier, 0.25);
        QVERIFY(!empty.trustworthy());

        // The calibration curve covers the whole 0..100 range in bands, and a band with
        // too few samples says so rather than reporting a hit rate from three calls.
        QCOMPARE(score.buckets.size(), 5);
        qint32 populated = 0;
        for (const CalibrationBucket &bucket : score.buckets) {
            QVERIFY(bucket.highStrength > bucket.lowStrength);
            if (bucket.samples > 0) {
                ++populated;
                QCOMPARE(bucket.lowStrength, 60);   // strength 70 lands in [60, 80)
            } else {
                QVERIFY(!bucket.trustworthy());
                QCOMPARE(bucket.hitRate, 0.0);
            }
        }
        QCOMPARE(populated, 1);
    }

    //! @tstid TS-LEDGER-004 @design DES-DOM-LEDGER
    // @relation(REQ-F-037, scope=function)
    void TS_LEDGER_004_theRowsThatStayedOutSurviveTheRoundTrip()
    {
        // The rows that did NOT trade are the reason this ledger is worth keeping: they
        // are the ones a record of executed trades can never contain, and they carry the
        // refusal code that says WHY. Losing them on save would quietly turn the whole
        // measurement back into a study of the gate.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));

        Prediction stayOut = callAt(0, 0, 20000.0, 12.0);
        stayOut.regime = Regime::EventWindow;
        stayOut.refusal = QStringLiteral("no-confluence");
        stayOut.taken = false;
        Prediction taken = callAt(5, -1, 20010.0, 71.5);
        taken.regime = Regime::HighVolatility;
        taken.taken = true;
        taken.priorMoveDir = -1;
        taken.vwapSide = 0;

        QVERIFY(appendPrediction(path, stayOut));
        QVERIFY(appendPrediction(path, taken));

        const QList<Prediction> loaded = loadPredictions(path);
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded.at(0).dir, 0);
        QCOMPARE(loaded.at(0).refusal, QStringLiteral("no-confluence"));
        QCOMPARE(loaded.at(0).regime, Regime::EventWindow);
        QVERIFY(!loaded.at(0).taken);
        QCOMPARE(loaded.at(1).dir, -1);
        QVERIFY(loaded.at(1).taken);
        QCOMPARE(loaded.at(1).regime, Regime::HighVolatility);
        QVERIFY(qAbs(loaded.at(1).strength - 71.5) < 1e-9);
        QCOMPARE(loaded.at(1).priorMoveDir, -1);
        QCOMPARE(loaded.at(1).vwapSide, 0);
        QCOMPARE(loaded.at(1).at, taken.at);

        // A truncated final line — a kill during a write — costs that line and nothing
        // else. A ledger that refused to load after one bad byte would lose the record it
        // exists to protect.
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        QVERIFY(file.write(QByteArray("{\"symbol\":\"NSDQ100\",\"pri")) > 0);
        file.close();
        QCOMPARE(loadPredictions(path).size(), 2);

        // A row that is not a prediction at all is skipped, not guessed at.
        QVERIFY(!predictionFromJson(QJsonObject{}).has_value());
        // …and an unwritable path is REPORTED rather than silently dropping the row.
        QVERIFY(!appendPrediction(QString{}, taken));
        QVERIFY(!appendPrediction(path, Prediction{}));
        QVERIFY(loadPredictions(dir.filePath(QStringLiteral("absent.jsonl"))).isEmpty());
    }

    //! @tstid TS-LEDGER-005 @design DES-DOM-LEDGER
    // @relation(REQ-F-037, scope=function)
    void TS_LEDGER_005_theRegimeLabelNeverGuessesAndSaysWhatOutranksWhat()
    {
        // The label exists so a hit rate can be read per regime instead of averaged
        // across two markets that behave differently. Its ordering is the point: what can
        // overrule what.
        RegimeInputs trending;
        trending.hurstKnown = true;
        trending.hurst = 0.62;
        trending.vixValid = true;
        trending.vix = 15.0;
        QCOMPARE(regimeFor(trending), Regime::Trend);

        RegimeInputs reverting = trending;
        reverting.hurst = 0.38;
        QCOMPARE(regimeFor(reverting), Regime::Range);

        // The band between them is a random walk and deserves neither label.
        RegimeInputs walk = trending;
        walk.hurst = 0.5;
        QCOMPARE(regimeFor(walk), Regime::Unknown);

        // Violent volatility outranks the trend/range question entirely.
        RegimeInputs violent = trending;
        violent.vix = 34.0;
        QCOMPARE(regimeFor(violent), Regime::HighVolatility);

        // …and a scheduled release outranks even that: the next print owns the horizon.
        RegimeInputs release = violent;
        release.eventWindow = true;
        QCOMPARE(regimeFor(release), Regime::EventWindow);

        // An unmeasurable persistence is UNKNOWN, never a comfortable "range".
        const RegimeInputs blind;
        QCOMPARE(regimeFor(blind), Regime::Unknown);
        QCOMPARE(regimeWord(Regime::Unknown), QStringLiteral("unknown"));

        // The horizons are the ones the module claims to score, and they are named in the
        // words the window shows.
        QCOMPARE(allHorizons().size(), 4);
        QCOMPARE(horizonMinutes(Horizon::M5), 5);
        QCOMPARE(horizonMinutes(Horizon::M15), 15);
        QCOMPARE(horizonMinutes(Horizon::M60), 60);
        QCOMPARE(horizonMinutes(Horizon::M180), 180);
        QCOMPARE(horizonWord(Horizon::M15), QStringLiteral("15 min"));
    }

    //! @tstid TS-LEDGER-006 @design DES-DOM-LEDGER
    // @relation(REQ-F-037, scope=function)
    void TS_LEDGER_006_strategyVersionRoundTripsAndDefaultsOnAnOlderRow()
    {
        // A ledger that mixes several strategies' calls under one set of numbers would
        // average away exactly the comparison it exists to make (2026-08-12 redesign:
        // multiple strategies now need to be run and scored separately).
        Prediction p = callAt(0, 1, 20000.0, 60.0);
        p.strategyVersion = QStringLiteral("swing-pullback-v1");
        const QJsonObject json = predictionToJson(p);
        QCOMPARE(json.value(QStringLiteral("strategyVersion")).toString(),
                 QStringLiteral("swing-pullback-v1"));
        const std::optional<Prediction> back = predictionFromJson(json);
        QVERIFY(back.has_value());
        QCOMPARE(back->strategyVersion, QStringLiteral("swing-pullback-v1"));

        // A row written before this field existed has no such key at all — it must load
        // as "not attributed" (empty), not fail to parse.
        QJsonObject older = json;
        older.remove(QStringLiteral("strategyVersion"));
        const std::optional<Prediction> fromOlder = predictionFromJson(older);
        QVERIFY(fromOlder.has_value());
        QVERIFY(fromOlder->strategyVersion.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPredictionLedger)
#include "tst_predictionledger.moc"
