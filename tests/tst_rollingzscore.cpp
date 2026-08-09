// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/RollingZScore.h"

#include <QtTest/QtTest>

#include <cmath>

using namespace trading::crowd;

// Normalization is where look-ahead bias sneaks in, so the past-only guarantee and the
// under-sampled/constant-history refusals are pinned directly.
class TestRollingZScore : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-CROWD-005 @design DES-DOM-CROWDSCORE
    // @relation(REQ-F-040, scope=function)
    void TS_CROWD_005_zScoreIsPastOnlyAndRefusesDegenerateHistory()
    {
        // Below minSamples → nothing (uncalibrated), never a fabricated number.
        QVERIFY(!zScore(5.0, {1.0, 2.0}, 3).has_value());
        // A constant history has no spread → nothing (an unmoving crowd is not a signal).
        QVERIFY(!zScore(5.0, {2.0, 2.0, 2.0, 2.0}, 3).has_value());

        // history {2,4,6,8}: mean 5, population stddev sqrt(5). A value AT the mean is z 0…
        const QList<double> history = {2.0, 4.0, 6.0, 8.0};
        const auto atMean = zScore(5.0, history, 3);
        QVERIFY(atMean.has_value());
        QVERIFY(qAbs(*atMean) < 1e-9);
        // …and one population-stddev above is z +1.
        const auto oneUp = zScore(5.0 + std::sqrt(5.0), history, 3);
        QVERIFY(oneUp.has_value());
        QVERIFY(qAbs(*oneUp - 1.0) < 1e-9);

        // The rolling class scores each value against the window BEFORE it, and evicts beyond its
        // size — so a value never sees itself or anything later.
        RollingZScore roll(3, /*minSamples=*/2);
        QVERIFY(!roll.push(10.0).has_value());   // no prior
        QVERIFY(!roll.push(10.0).has_value());   // one prior < minSamples
        QVERIFY(!roll.push(10.0).has_value());   // prior {10,10} constant → no z
        QVERIFY(!roll.push(16.0).has_value());   // prior {10,10,10} constant → no z; window evicts
        QVERIFY(roll.push(10.0).has_value());    // prior {10,10,16} has spread → a z at last
        QCOMPARE(roll.size(), 3);
    }
};

QTEST_APPLESS_MAIN(TestRollingZScore)
#include "tst_rollingzscore.moc"
