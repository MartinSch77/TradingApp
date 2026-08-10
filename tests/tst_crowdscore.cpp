// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/CrowdScore.h"

#include <QtTest/QtTest>

using namespace trading::crowd;

namespace {
ComponentReading reading(Source family, double zscore, Freshness freshness = Freshness::Live,
                         qint64 ageSec = 0)
{
    ComponentReading out;
    out.family = family;
    out.measured = true;
    out.zscore = zscore;
    out.freshness = freshness;
    out.ageSec = ageSec;
    return out;
}
} // namespace

// The transparent score is the baseline the model must beat, so its rules — the contrarian
// retail flip, missing-not-zero, coverage/confidence, clamping and renormalization — are pinned
// exactly. Pure domain, no store or network.
class TestCrowdScore : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-CROWD-006 @design DES-DOM-CROWDSCORE
    // @relation(REQ-F-040, scope=function)
    void TS_CROWD_006_transparentWeightedScore()
    {
        const CrowdScoreConfig cfg;   // defaults 0.35 / 0.30 / 0.20 / 0.15

        // No readings: no coverage, no data — but the four families are still listed, each named
        // as missing, so the view shows WHY there is nothing rather than an empty gap.
        const CrowdScoreResult empty = crowdScore({}, cfg);
        QVERIFY(empty.isEmpty());
        QCOMPARE(empty.coverage, 0.0);
        QCOMPARE(empty.components.size(), 4);
        QCOMPARE(empty.warnings.size(), 4);

        // RETAIL IS CONTRARIAN: a strongly crowd-long retail reading yields a BEARISH score, and
        // the component is flagged contrarian with a negative contribution.
        const CrowdScoreResult crowdLong = crowdScore({reading(Source::RetailPositioning, 3.0)}, cfg);
        QVERIFY(crowdLong.score < 0.0);
        QCOMPARE(crowdLong.direction, QStringLiteral("bearish"));
        QVERIFY(qAbs(crowdLong.coverage - 0.35) < 1e-9);
        QVERIFY(crowdLong.components.constFirst().contrarian);
        QVERIFY(crowdLong.components.constFirst().contribution < 0.0);

        // A bullish institutional-only field is bullish and NOT flipped.
        const CrowdScoreResult inst = crowdScore({reading(Source::InstitutionalPositioning, 3.0)}, cfg);
        QVERIFY(inst.score > 0.0);
        QCOMPARE(inst.direction, QStringLiteral("bullish"));
        QVERIFY(qAbs(inst.coverage - 0.20) < 1e-9);

        // MISSING is never zero: a full field has coverage 1 and high confidence; a partial field
        // has lower coverage AND confidence, with the absent families named.
        const CrowdScoreResult full = crowdScore({reading(Source::RetailPositioning, -2.0),
                                                  reading(Source::Options, 2.0),
                                                  reading(Source::InstitutionalPositioning, 2.0),
                                                  reading(Source::Social, 2.0)},
                                                 cfg);
        QCOMPARE(full.coverage, 1.0);
        QVERIFY(full.warnings.isEmpty());
        QVERIFY(full.confidence > 0.99);   // full coverage + all live
        QVERIFY(full.score > 0.0);         // crowd short (contrarian bullish) + bullish rest
        const CrowdScoreResult partial = crowdScore({reading(Source::Options, 2.0)}, cfg);
        QVERIFY(partial.coverage < full.coverage);
        QVERIFY(partial.confidence < full.confidence);
        QCOMPARE(partial.warnings.size(), 3);

        // STALE still counts toward the score but HALVES its confidence and is warned.
        const CrowdScoreResult stale =
            crowdScore({reading(Source::Options, 2.0, Freshness::Stale, 100000)}, cfg);
        QVERIFY(stale.score > 0.0);
        QVERIFY(qAbs(stale.confidence - 0.15) < 1e-9);   // coverage 0.30 x freshness 0.5
        QVERIFY(stale.warnings.join(QChar(u' ')).contains(QStringLiteral("stale")));

        // The z is CLAMPED: an extreme reading does not exceed the full-scale contribution.
        const CrowdScoreResult extreme =
            crowdScore({reading(Source::InstitutionalPositioning, 999.0)}, cfg);
        QVERIFY(qAbs(extreme.score - 1.0) < 1e-9);
    }
};

QTEST_APPLESS_MAIN(TestCrowdScore)
#include "tst_crowdscore.moc"
