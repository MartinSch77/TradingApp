// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/LeadGauge.h"

#include <QtTest/QtTest>

using trading::ui::leadGaugeBar;

// The lead gauge's only NUMERIC claim is the bar geometry: how far a bar reaches from the
// zero line, and to which side. It is a pure function precisely so it can be checked without
// rendering — a gauge whose bar length is wrong misleads exactly the way a mislabelled number
// would, and a screenshot cannot prove a bar is the right length.
class TestLeadGauge : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-GAUGE-001 @design DES-UI-LEADGAUGE
    // @relation(REQ-F-035, scope=function)
    void TS_GAUGE_001_barGeometryIsSignedProportionalAndClamped()
    {
        // SIGN: >= 0 extends right (up); < 0 extends left (down). Zero is up, never "down".
        QVERIFY(leadGaugeBar(0.5, 2.0, 100).up);
        QVERIFY(leadGaugeBar(0.0, 2.0, 100).up);
        QVERIFY(!leadGaugeBar(-0.5, 2.0, 100).up);

        // PROPORTIONAL: half of full-scale is half the track; the extent is the MAGNITUDE, so a
        // +1 % and a -1 % reach equally far — only the side differs.
        QCOMPARE(leadGaugeBar(1.0, 2.0, 100).extentPx, 50);
        QCOMPARE(leadGaugeBar(-1.0, 2.0, 100).extentPx, 50);
        QCOMPARE(leadGaugeBar(0.5, 2.0, 100).extentPx, 25);

        // CLAMPED: a move beyond full-scale fills the track and never overruns the widget.
        QCOMPARE(leadGaugeBar(5.0, 2.0, 100).extentPx, 100);
        QCOMPARE(leadGaugeBar(-9.9, 2.0, 100).extentPx, 100);

        // A flat field is a zero-length bar (the widget draws a minimal stub; the geometry is 0).
        QCOMPARE(leadGaugeBar(0.0, 2.0, 100).extentPx, 0);

        // Degenerate inputs never divide by zero or draw off the track.
        QCOMPARE(leadGaugeBar(1.0, 0.0, 100).extentPx, 0);
        QCOMPARE(leadGaugeBar(1.0, 2.0, 0).extentPx, 0);
    }
};

QTEST_APPLESS_MAIN(TestLeadGauge)
#include "tst_leadgauge.moc"
