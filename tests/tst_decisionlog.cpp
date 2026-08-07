// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The bot's decision log (REQ-F-029, DES-DOM-DECLOG): one human-readable line per
// instrument the bot considered, saying whether it was traded and why.
//
// What is worth testing here is not the formatting for its own sake but the HONESTY of
// the rendering. This file is what someone reads after a long weekend to find out why an
// instrument was never traded, so a line that quietly defaults a missing side to "long",
// or prints a strength for a note with no reads behind it, would mislead exactly the
// reader it exists for.

#include "domain/DecisionLog.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

QDateTime moment()
{
    return {QDate(2026, 8, 6), QTime(13, 45), QTimeZone::UTC};
}

trading::DecisionNote refusalNote()
{
    trading::DecisionNote note;
    note.at = moment();
    note.symbol = QStringLiteral("GOLD");
    note.dir = -1;
    note.confidence = 41.0;
    note.leadStrength = 22.0;
    note.leadMeasured = 4;
    note.leadUnknowns = 5;
    note.traded = false;
    note.code = QStringLiteral("no-confluence");
    note.why = QStringLiteral("only 4 of 9 reads agree, 5 required");
    return note;
}

trading::DecisionNote tradedNote()
{
    trading::DecisionNote note;
    note.at = moment();
    note.symbol = QStringLiteral("SPX500");
    note.dir = 1;
    note.confidence = 72.0;
    note.leadStrength = 68.0;
    note.leadMeasured = 9;
    note.leadUnknowns = 0;
    note.traded = true;
    note.why = QStringLiteral("7 of 9 reads agree and the round trip clears its cost");
    note.stake = 250.0;
    note.leverage = 10;
    note.fillRate = 5012.4;
    note.slRate = 4998.1;
    note.tpRate = 5040.3;
    note.openCost = 1.25;
    return note;
}

} // namespace

class TestDecisionLog : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-DECLOG-001 @design DES-DOM-DECLOG
    // @relation(REQ-F-029, scope=function)
    void TS_DECLOG_001_everyConsideredInstrumentSaysWhatHappenedAndWhy()
    {
        // A refusal leads with the CODE, because a week of these is read with grep and a
        // code buried inside prose cannot be counted.
        const QString refused = trading::decisionLine(refusalNote());
        QVERIFY(refused.contains(QStringLiteral("REFUSED")));
        QVERIFY(refused.contains(QStringLiteral("GOLD")));
        QVERIFY(refused.contains(QStringLiteral("no-confluence")));
        QVERIFY(refused.contains(QStringLiteral("short")));
        // The rule's own sentence survives, unedited: it is the part that explains.
        QVERIFY(refused.contains(QStringLiteral("only 4 of 9 reads agree, 5 required")));
        // …and the timestamp is UTC and sortable, so the file can be read in order.
        QVERIFY(refused.startsWith(QStringLiteral("2026-08-06T13:45:00")));

        // A trade carries the GEOMETRY beside the evidence, so a reader can check the
        // numbers against the reason rather than taking the reason on trust.
        const QString traded = trading::decisionLine(tradedNote());
        QVERIFY(traded.contains(QStringLiteral("TRADED")));
        QVERIFY(traded.contains(QStringLiteral("long")));
        QVERIFY(traded.contains(QStringLiteral("250.00")));
        QVERIFY(traded.contains(QStringLiteral("x10")));
        QVERIFY(traded.contains(QStringLiteral("5012.4")));
        QVERIFY(traded.contains(QStringLiteral("4998.1")));   // the stop
        QVERIFY(traded.contains(QStringLiteral("5040.3")));   // the target
        QVERIFY(traded.contains(QStringLiteral("1.25")));     // what it cost to open
        // The two outcomes must never be confusable with one another.
        QVERIFY(!traded.contains(QStringLiteral("REFUSED")));
        QVERIFY(!refused.contains(QStringLiteral("TRADED")));

        // A refusal that never reached a direction says SO. Defaulting it to "long" would
        // put a side in the record that no rule ever chose.
        trading::DecisionNote sideless = refusalNote();
        sideless.dir = 0;
        const QString noSide = trading::decisionLine(sideless);
        QVERIFY(noSide.contains(QStringLiteral("no side")));
        QVERIFY(!noSide.contains(QStringLiteral("long")));

        // An empty code is stated rather than leaving a blank column that reads as a
        // formatting fault.
        trading::DecisionNote uncoded = refusalNote();
        uncoded.code.clear();
        QVERIFY(trading::decisionLine(uncoded).contains(QStringLiteral("(no code)")));
    }

    //! @tstid TS-DECLOG-002 @design DES-DOM-DECLOG
    // @relation(REQ-F-029, scope=function)
    void TS_DECLOG_002_theEvidenceIsReportedAsMeasuredAndTheFileExplainsItself()
    {
        // "4 of 9" is not the same fact as "4 of 9, five of them unmeasurable", and the
        // line must not collapse the two.
        const QString refused = trading::decisionLine(refusalNote());
        QVERIFY(refused.contains(QStringLiteral("4 of 9 reads measured")));
        QVERIFY(refused.contains(QStringLiteral("5 unmeasurable")));

        // With every read available there is no unmeasurable remainder to report.
        const QString traded = trading::decisionLine(tradedNote());
        QVERIFY(traded.contains(QStringLiteral("9 of 9 reads measured")));
        QVERIFY(!traded.contains(QStringLiteral("unmeasurable")));

        // A note with NO reads behind it must not print a strength — that number would be
        // a claim about evidence that was never gathered.
        trading::DecisionNote blind = refusalNote();
        blind.leadMeasured = 0;
        blind.leadUnknowns = 0;
        blind.leadStrength = 0.0;
        const QString unevaluated = trading::decisionLine(blind);
        QVERIFY(unevaluated.contains(QStringLiteral("no reads evaluated")));
        QVERIFY(!unevaluated.contains(QStringLiteral("strength")));

        // An unusable note yields NO line rather than a malformed one.
        trading::DecisionNote broken;
        QVERIFY(trading::decisionLine(broken).isEmpty());
        broken.symbol = QStringLiteral("SPX500");   // still no timestamp
        QVERIFY(trading::decisionLine(broken).isEmpty());

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("botsim-decisions.log"));

        // The file explains its own format, ONCE — a header repeated per append would be
        // most of the file after a week.
        QVERIFY(trading::appendDecision(path, refusalNote()));
        QVERIFY(trading::appendDecision(path, tradedNote()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString contents = QString::fromUtf8(file.readAll());
        file.close();
        QCOMPARE(contents.count(QStringLiteral("# TradingApp bot decision log")), 1);
        // The header states the two facts a reader must not get wrong about this file.
        QVERIFY(contents.contains(QStringLiteral("SIMULATED")));
        QVERIFY(contents.contains(QStringLiteral("CONSIDERED")));
        QVERIFY(contents.contains(QStringLiteral("GOLD")));
        QVERIFY(contents.contains(QStringLiteral("SPX500")));

        // An invalid note is not appended at all, so it cannot leave a blank line behind.
        const qint64 before = QFileInfo(path).size();
        QVERIFY(!trading::appendDecision(path, trading::DecisionNote{}));
        QCOMPARE(QFileInfo(path).size(), before);

        // A missing directory is CREATED rather than refused: on a first run the app's
        // config directory may not exist yet, and losing the first day of decisions to
        // that would defeat the point of keeping them.
        const QString nested =
            QDir(dir.path()).filePath(QStringLiteral("fresh/config/botsim-decisions.log"));
        QVERIFY(trading::appendDecision(nested, refusalNote()));
        QVERIFY(QFileInfo::exists(nested));

        // An unwritable path is REPORTED rather than silently swallowing the decision.
        // Empty is one such path; the other is a parent that exists but is a FILE, so no
        // directory can be made there.
        QVERIFY(!trading::appendDecision(QString(), refusalNote()));
        const QString throughAFile = path + QStringLiteral("/decisions.log");
        QVERIFY(!trading::appendDecision(throughAFile, refusalNote()));
    }
};

QTEST_GUILESS_MAIN(TestDecisionLog)
#include "tst_decisionlog.moc"
