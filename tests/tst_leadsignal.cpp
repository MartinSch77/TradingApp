// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The combined indication (DES-DOM-LEAD, REQ-F-036): the nine independent reads, the
// constituent field, the session phase and the regime scored into ONE answer.
//
// What is tested here is not "does it produce a number" but the three properties that
// make the number worth acting on — an unmeasurable input contributes nothing, the
// strength is capped by how much was measurable, and the leverage it names can only
// ever be an upper bound. A combined signal without those is an aggregate that hides
// exactly the information a leveraged trade needs.

#include "domain/LeadSignal.h"

#include <QTest>

using namespace trading;

namespace {

Read agreeing(qint32 dir, const QString &detail = QStringLiteral("x"))
{
    Read r;
    r.known = true;
    r.dir = dir;
    r.detail = detail;
    return r;
}

// All nine reads pointing the same way.
IndexReads allAgree(qint32 dir)
{
    IndexReads reads;
    reads.futuresLead = agreeing(dir);
    reads.futuresMomentum = agreeing(dir);
    reads.volatility = agreeing(dir);
    reads.yields = agreeing(dir);
    reads.curve = agreeing(dir);
    reads.participation = agreeing(dir);
    reads.aboveVwap = agreeing(dir);
    reads.upDownVolume = agreeing(dir);
    reads.structure = agreeing(dir);
    return reads;
}

// A broad constituent field moving `dir`, with `up` of ten up.
HeavyweightPulse fieldOf(qint32 up, double average, const QString &leader = QStringLiteral("NVDA"),
                         double leaderMove = 1.0)
{
    HeavyweightPulse p;
    p.indexName = QStringLiteral("Nasdaq-100");
    p.measured = 10;
    p.up = up;
    p.averageChangePct = average;
    p.leader = leader;
    p.leaderChangePct = leaderMove;
    p.laggard = QStringLiteral("COST");
    p.laggardChangePct = -0.2;
    for (int i = 0; i < 10; ++i) {
        HeavyweightRow row;
        row.ticker = QStringLiteral("T%1").arg(i);
        row.known = true;
        row.changePct = average;
        p.rows.append(row);
    }
    return p;
}

// A deliberately quiet moment: 11:00 UTC on a Tuesday is neither an open, a close nor
// a macro slot, so the phase factor leaves the reads alone.
QDateTime quietMoment()
{
    return {QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC};
}

LeadInputs baseInputs()
{
    LeadInputs in;
    in.symbol = QStringLiteral("NSDQ100");
    in.now = quietMoment();
    in.vixValid = true;
    in.vix = 15.0;
    // A measured, non-inverted term structure: part of a calm regime, and measurable so
    // that the tests below isolate the ONE factor each of them varies.
    in.term.known = true;
    in.term.inverted = false;
    in.term.nearFarRatio = 0.85;
    in.term.detail = QStringLiteral("term structure normal");
    in.compositeDir = 1;
    in.compositeConfidence = 60.0;
    return in;
}

} // namespace

class TestLeadSignal : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-LEAD-001 @design DES-DOM-LEAD
    // @relation(REQ-F-036, scope=function)
    void TS_LEAD_001_agreementAcrossIndependentReadsIsWhatMakesItStrong()
    {
        // Everything agreeing, everything measured, a quiet session and a calm regime:
        // this is the only situation that may read as STRONG.
        LeadInputs in = baseInputs();
        in.reads = allAgree(1);
        in.pulse = fieldOf(9, 1.2);
        const LeadSignal strong = leadSignal(in);
        QCOMPARE(strong.dir, 1);
        QCOMPARE(strong.grade, LeadGrade::Strong);
        QCOMPARE(strong.unknowns, 0);
        QCOMPARE(strong.measured, 10);
        QVERIFY(strong.actionable());
        QVERIFY(strong.suggestedLeverage > 1);
        QVERIFY(strong.headline.contains(QStringLiteral("LONG")));
        // Every input is named in the reasons, so the number can be argued with.
        QCOMPARE(strong.reasons.size(), 13);   // 9 reads + field + phase + 2 regime lines

        // The same reads on the SHORT side are worth exactly as much: the arithmetic
        // is symmetric, which is what makes a short a first-class trade here.
        LeadInputs shortIn = baseInputs();
        shortIn.compositeDir = -1;
        shortIn.reads = allAgree(-1);
        shortIn.pulse = fieldOf(1, -1.2);
        const LeadSignal shortSignal = leadSignal(shortIn);
        QCOMPARE(shortSignal.dir, -1);
        QCOMPARE(shortSignal.grade, LeadGrade::Strong);
        QVERIFY(qAbs(shortSignal.strength - strong.strength) < 1e-9);

        // Reads that CONTRADICT the side eat the strength: the same field with three
        // reads against it cannot stay strong.
        LeadInputs mixed = in;
        mixed.reads.yields = agreeing(-1);
        mixed.reads.volatility = agreeing(-1);
        mixed.reads.structure = agreeing(-1);
        const LeadSignal contested = leadSignal(mixed);
        QVERIFY(contested.strength < strong.strength);
        QVERIFY(contested.grade < LeadGrade::Strong);
    }

    //! @tstid TS-LEAD-002 @design DES-DOM-LEAD
    // @relation(REQ-F-036, scope=function)
    void TS_LEAD_002_whatCouldNotBeMeasuredNeverAddsStrength()
    {
        // THE rule of this module. Two reads available and both agreeing must NOT
        // produce the same answer as ten available and all agreeing — otherwise a
        // quiet feed outage would look like conviction.
        LeadInputs full = baseInputs();
        full.reads = allAgree(1);
        full.pulse = fieldOf(9, 1.2);
        const LeadSignal complete = leadSignal(full);

        LeadInputs thin = baseInputs();
        thin.reads.futuresLead = agreeing(1);
        thin.reads.volatility = agreeing(1);   // the other seven, and the field, unknown
        const LeadSignal partial = leadSignal(thin);
        QCOMPARE(partial.measured, 2);
        QCOMPARE(partial.unknowns, 8);
        QVERIFY(partial.strength < complete.strength);
        // …and it can never be graded strong, however unanimous the two are.
        QVERIFY(partial.grade < LeadGrade::Strong);
        QVERIFY(!partial.actionable() || (partial.grade == LeadGrade::Fair));
        // The unmeasurable ones are NAMED, not silently dropped.
        QVERIFY(partial.reasons.join(QStringLiteral(" ")).contains(QStringLiteral("not measurable")));

        // Nothing measurable at all: no side, no strength, and a headline that says so
        // rather than a confident zero.
        const LeadSignal nothing = leadSignal(LeadInputs{});
        QCOMPARE(nothing.dir, 0);
        QCOMPARE(nothing.strength, 0.0);
        QCOMPARE(nothing.grade, LeadGrade::None);
        QCOMPARE(nothing.suggestedLeverage, 1);
        QVERIFY(!nothing.actionable());
        QVERIFY(nothing.headline.contains(QStringLiteral("No usable read")));

        // With no composite call, the signal reports the side the READS lean to rather
        // than refusing — the reads are evidence whether or not the app has a view.
        LeadInputs noCall = baseInputs();
        noCall.compositeDir = 0;
        noCall.reads = allAgree(-1);
        noCall.pulse = fieldOf(1, -1.5);
        QCOMPARE(leadSignal(noCall).dir, -1);
    }

    //! @tstid TS-LEAD-003 @design DES-DOM-LEAD
    // @relation(REQ-F-036, scope=function)
    void TS_LEAD_003_theClockAndTheRegimeOnlyEverReduce()
    {
        LeadInputs calm = baseInputs();
        calm.reads = allAgree(1);
        calm.pulse = fieldOf(9, 1.2);
        const double quiet = leadSignal(calm).strength;

        // The opening's first minutes, a macro window and a policy window each cut it,
        // and the policy window cuts hardest: a statement outranks any structure.
        LeadInputs opening = calm;
        opening.now = QDateTime(QDate(2026, 8, 4), QTime(13, 35), QTimeZone::UTC);  // just open
        const double atOpen = leadSignal(opening).strength;
        QVERIFY(atOpen < quiet);

        LeadInputs policy = calm;
        policy.now = QDateTime(QDate(2026, 8, 4), QTime(18, 15), QTimeZone::UTC);   // 14:15 NY
        QVERIFY(leadSignal(policy).strength <= quiet);

        // An elevated VIX reduces it; a calm one does not raise it above the baseline.
        LeadInputs fearful = calm;
        fearful.vix = 32.0;
        QVERIFY(leadSignal(fearful).strength < quiet);
        LeadInputs elevated = calm;
        elevated.vix = 26.0;
        QVERIFY(leadSignal(elevated).strength < quiet);
        QVERIFY(leadSignal(elevated).strength > leadSignal(fearful).strength);

        // An imminent high-impact release halves it, whatever the reads say.
        LeadInputs risky = calm;
        risky.eventRisk = true;
        QVERIFY(leadSignal(risky).strength < quiet);

        // An UNKNOWN VIX is not a calm market: it is reported as unmeasurable and
        // leaves the strength alone rather than flattering it.
        LeadInputs noVix = calm;
        noVix.vixValid = false;
        QCOMPARE(leadSignal(noVix).strength, quiet);
        QVERIFY(leadSignal(noVix).reasons.join(QStringLiteral(" "))
                    .contains(QStringLiteral("VIX not measurable")));

        // An unknown CLOCK likewise: it cannot decide a phase, so it does not pretend
        // to — and says which it is.
        LeadInputs noClock = calm;
        noClock.now = QDateTime();
        QCOMPARE(leadSignal(noClock).strength, quiet);
        QVERIFY(leadSignal(noClock).reasons.join(QStringLiteral(" "))
                    .contains(QStringLiteral("clock unknown")));

        // An INVERTED term structure reduces it too, and independently of the level: the
        // VIX here is the same calm 15, but the market is paying more for the next nine
        // days than for the next three months, so whatever structure exists is being
        // priced as temporary.
        LeadInputs stressed = calm;
        stressed.term.inverted = true;
        stressed.term.nearFarRatio = 1.15;
        stressed.term.detail = QStringLiteral("term structure INVERTED");
        QVERIFY(leadSignal(stressed).strength < quiet);
        QVERIFY(leadSignal(stressed).reasons.join(QStringLiteral(" "))
                    .contains(QStringLiteral("INVERTED")));

        // …and an UNMEASURABLE term structure changes nothing, but is named rather than
        // silently treated as normal.
        LeadInputs noTerm = calm;
        noTerm.term = TermStructure{};
        QCOMPARE(leadSignal(noTerm).strength, quiet);
        QVERIFY(leadSignal(noTerm).reasons.join(QStringLiteral(" "))
                    .contains(QStringLiteral("term structure: not measurable")));
    }

    //! @tstid TS-LEAD-004 @design DES-DOM-LEAD
    // @relation(REQ-F-036, scope=function)
    void TS_LEAD_004_theLeverageItNamesIsAnUpperBoundAndOneNameCannotCarryIt()
    {
        // The leverage rises with the GRADE and never above the ceiling of the top
        // grade — this number is intersected with the risk budget, the bucket cap and
        // the instrument's ladder, so it may only ever be a bound.
        LeadInputs in = baseInputs();
        in.reads = allAgree(1);
        in.pulse = fieldOf(9, 1.2);
        const LeadSignal strong = leadSignal(in);

        LeadInputs weak = baseInputs();
        weak.reads.futuresLead = agreeing(1);   // one read only
        const LeadSignal thin = leadSignal(weak);
        QVERIFY(thin.suggestedLeverage <= strong.suggestedLeverage);
        QVERIFY(thin.suggestedLeverage >= 1);
        QCOMPARE(leadSignal(LeadInputs{}).suggestedLeverage, 1);
        QVERIFY(strong.suggestedLeverage <= 10);

        // The classic false positive: the average looks strong because ONE name ran
        // away with it. The field's weight is halved, so the same average with the
        // move spread across the ten is worth more than with it concentrated.
        LeadInputs spread = in;
        spread.pulse = fieldOf(9, 1.2, QStringLiteral("NVDA"), 1.4);   // leader ≈ the average
        LeadInputs carried = in;
        carried.pulse = fieldOf(9, 1.2, QStringLiteral("NVDA"), 11.0); // one name is the move
        QVERIFY(leadSignal(carried).strength < leadSignal(spread).strength);
        QVERIFY(leadSignal(carried).reasons.join(QStringLiteral(" "))
                    .contains(QStringLiteral("carrying it")));

        // Every grade has a word, and they are distinct — the window prints these.
        QVERIFY(!leadGradeWord(LeadGrade::None).isEmpty());
        QVERIFY(leadGradeWord(LeadGrade::Weak) != leadGradeWord(LeadGrade::Fair));
        QVERIFY(leadGradeWord(LeadGrade::Fair) != leadGradeWord(LeadGrade::Strong));
    }
};

QTEST_GUILESS_MAIN(TestLeadSignal)
#include "tst_leadsignal.moc"
