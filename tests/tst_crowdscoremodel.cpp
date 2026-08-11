// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CrowdScoreModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace trading::crowd;
using trading::ui::CrowdScoreModel;

namespace {
ComponentReading reading(Source family, double zscore)
{
    ComponentReading out;
    out.family = family;
    out.measured = true;
    out.zscore = zscore;
    out.freshness = Freshness::Live;
    out.ageSec = 0;
    return out;
}
} // namespace

// The view-model computes nothing; it shapes a pushed result for binding. So the test checks it
// exposes the pushed numbers, emits changed once, and lists ALL FOUR factors (missing ones with
// measured=false), which is what keeps "no options data" visible in a view rather than a gap.
class TestCrowdScoreModel : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-CROWD-008 @design DES-UI-CROWDSCORE
    // @relation(REQ-F-040, scope=function)
    void TS_CROWD_008_viewModelShapesTheResult()
    {
        CrowdScoreModel model;
        const QSignalSpy spy(&model, &CrowdScoreModel::changed);
        QVERIFY(!model.hasData());

        const CrowdScoreConfig cfg;
        const CrowdScoreResult result =
            crowdScore({reading(Source::Options, 2.0), reading(Source::Social, 1.0)}, cfg);
        model.setResult(result);

        QCOMPARE(spy.count(), 1);
        QVERIFY(model.hasData());
        QCOMPARE(model.direction(), result.direction);
        QVERIFY(qFuzzyCompare(1.0 + model.score(), 1.0 + result.score));
        QVERIFY(!model.headline().isEmpty());

        // components() exposes all four factors with the fields a view binds to; the two missing
        // families are present (measured=false) and named in warnings.
        const QVariantList components = model.components();
        QCOMPARE(components.size(), 4);
        int measured = 0;
        for (const QVariant &value : components) {
            const QVariantMap map = value.toMap();
            QVERIFY(map.contains(QStringLiteral("label")));
            QVERIFY(map.contains(QStringLiteral("contribution")));
            QVERIFY(map.contains(QStringLiteral("freshness")));
            if (map.value(QStringLiteral("measured")).toBool()) {
                ++measured;
            }
        }
        QCOMPARE(measured, 2);
        QVERIFY(model.warnings().size() >= 2);
    }
};

QTEST_GUILESS_MAIN(TestCrowdScoreModel)
#include "tst_crowdscoremodel.moc"
