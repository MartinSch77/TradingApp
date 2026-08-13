// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Architecture-fitness check (REQ-N-002, DES-ARCH-LAYERS): the domain layer is
// Qt Core only, and layering (ui -> services -> domain) is enforced by the
// LINKER — trading_domain's own CMakeLists.txt entry is
// `target_link_libraries(trading_domain PUBLIC Qt6::Core)`, nothing else, so a
// domain file that reaches for QNetworkAccessManager or QWidget simply does not
// compile. That linker enforcement is the ground truth; this test is a much
// FASTER, more PRECISE early warning layered on top of it — a source-level scan
// that names the exact file and header a violation would introduce, rather than
// a wall of undefined-reference linker errors days later. Deliberately does not
// forbid QFile/QDir/QTextStream: domain/PredictionLedger.cpp and
// domain/DecisionLog.cpp genuinely persist to a file (REQ-F-033, REQ-F-037), and
// QFile/QDir/QTextStream are themselves Qt CORE classes — "no I/O" in REQ-N-002's
// own text is about not reaching UP into services'/ui's Qt modules (network,
// widgets, QML), which is exactly what "Qt Core only" already states precisely.

#include <QDir>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

namespace {

// One entry per Qt module domain must never depend on, each as a regex over the
// bracket-included header name (`#include <QFoo>`). Prefixes rather than an
// exhaustive name list, because a new class in an already-forbidden module
// (say, Qt 6.12 adding QNetworkSomethingNew) must be caught without an edit here.
const QStringList &forbiddenHeaderPatterns()
{
    static const QStringList patterns{
        // QtNetwork
        QStringLiteral("^QNetwork"), QStringLiteral("^QSsl"),
        QStringLiteral("^QTcpSocket$"), QStringLiteral("^QUdpSocket$"),
        QStringLiteral("^QLocalSocket$"), QStringLiteral("^QAbstractSocket$"),
        QStringLiteral("^QHttp"),
        // QtWidgets
        QStringLiteral("^QWidget"), QStringLiteral("^QApplication$"),
        QStringLiteral("^QDialog"), QStringLiteral("^QMainWindow$"),
        QStringLiteral("^QPushButton$"), QStringLiteral("^QLabel$"),
        QStringLiteral("^QTable"), QStringLiteral("^QTree"),
        QStringLiteral("^QMessageBox$"), QStringLiteral("^QMenu"),
        QStringLiteral("^QToolBar$"), QStringLiteral("^QStatusBar$"),
        QStringLiteral("^QLayout$"), QStringLiteral("^Q.*BoxLayout$"),
        QStringLiteral("^QGroupBox$"), QStringLiteral("^QComboBox$"),
        QStringLiteral("^QSpinBox$"), QStringLiteral("^QCheckBox$"),
        QStringLiteral("^QRadioButton$"), QStringLiteral("^QLineEdit$"),
        QStringLiteral("^QTextEdit$"), QStringLiteral("^QListWidget$"),
        QStringLiteral("^QHeaderView$"), QStringLiteral("^QAbstractItemView$"),
        QStringLiteral("^QStyledItemDelegate$"), QStringLiteral("^QGraphicsView$"),
        QStringLiteral("^QSplitter$"), QStringLiteral("^QScrollArea$"),
        QStringLiteral("^QFrame$"), QStringLiteral("^QTabWidget$"),
        QStringLiteral("^QToolButton$"), QStringLiteral("^QSlider$"),
        QStringLiteral("^QProgressBar$"), QStringLiteral("^QDockWidget$"),
        // QtGui (beyond what QtCore itself needs)
        QStringLiteral("^QPainter"), QStringLiteral("^QPixmap$"),
        QStringLiteral("^QIcon$"), QStringLiteral("^QImage$"),
        QStringLiteral("^QColor$"), QStringLiteral("^QFont"),
        QStringLiteral("^QPen$"), QStringLiteral("^QBrush$"),
        QStringLiteral("^QCursor$"), QStringLiteral("^QScreen$"),
        QStringLiteral("^QGuiApplication$"), QStringLiteral("^QWindow$"),
        QStringLiteral("^QOpenGL"),
        // QtQml / QtQuick
        QStringLiteral("^QQml"), QStringLiteral("^QQuick"),
        QStringLiteral("^QJSEngine$"), QStringLiteral("^QJSValue$"),
        // QtSql
        QStringLiteral("^QSql"),
        // QtConcurrent (QThread/QThreadPool are QtCore and stay allowed)
        QStringLiteral("^QFuture"), QStringLiteral("^QtConcurrent"),
        // QtCharts / QtGraphs
        QStringLiteral("^QChart"), QStringLiteral("^QAbstractSeries$"),
        QStringLiteral("^QXYSeries$"), QStringLiteral("^QValueAxis$"),
        QStringLiteral("^QBarSeries$"), QStringLiteral("^QPieSeries$"),
        QStringLiteral("^Q3D"), QStringLiteral("^QCustomSeries$"),
    };
    return patterns;
}

// The patterns above, COMPILED ONCE rather than once per include line checked
// (this test's original form built a fresh QRegularExpression per pattern per
// matched #include across every domain file — thousands of PCRE2/JIT
// compilations for a handful of distinct patterns). Measured 2026-08-13: that
// churn is also what triggered ~1000 valgrind "Conditional jump depends on
// uninitialised value(s)" reports, all from the same unresolved JIT address —
// the documented PCRE2-JIT/Valgrind interaction, not a real defect (0 bytes
// definitely/possibly lost in the same run). Compiling once removes the churn
// this test controls rather than adding a suppression for noise it caused.
const QList<QRegularExpression> &forbiddenHeaderRegexes()
{
    static const QList<QRegularExpression> compiled = [] {
        QList<QRegularExpression> out;
        out.reserve(forbiddenHeaderPatterns().size());
        for (const QString &pattern : forbiddenHeaderPatterns()) {
            out.append(QRegularExpression(pattern));
        }
        return out;
    }();
    return compiled;
}

} // namespace

class TestArchitecture : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-ARCH-001 @design DES-ARCH-LAYERS
    // @relation(REQ-N-002, scope=function)
    //
    // No file under src/domain/ includes a header from a Qt module beyond Core —
    // the source-level shape of "the domain layer shall be pure (Qt Core only,
    // no I/O, no UI)". A violation is named by file, line and header, so a
    // regression points straight at the fix rather than a build-time linker wall.
    void TS_ARCH_001_domainLayerIncludesOnlyQtCore()
    {
        const QDir domainDir(QStringLiteral(TRADINGAPP_SOURCE_DIR "/src/domain"));
        QVERIFY(domainDir.exists());

        static const QRegularExpression includeRe(
            QStringLiteral(R"(^\s*#include\s*<(Q[A-Za-z0-9]+)>)"));

        QStringList violations;
        const QStringList files =
            domainDir.entryList({QStringLiteral("*.h"), QStringLiteral("*.cpp")},
                                QDir::Files, QDir::Name);
        QVERIFY(!files.isEmpty());   // a check that scans nothing proves nothing

        for (const QString &fileName : files) {
            QFile file(domainDir.filePath(fileName));
            QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
            QTextStream stream(&file);
            qint32 lineNo = 0;
            while (!stream.atEnd()) {
                ++lineNo;
                const QString line = stream.readLine();
                const QRegularExpressionMatch m = includeRe.match(line);
                if (!m.hasMatch()) {
                    continue;
                }
                const QString header = m.captured(1);
                for (const QRegularExpression &re : forbiddenHeaderRegexes()) {
                    if (re.match(header).hasMatch()) {
                        violations << QStringLiteral("%1:%2: #include <%3>")
                                          .arg(fileName)
                                          .arg(lineNo)
                                          .arg(header);
                    }
                }
            }
        }
        QVERIFY2(violations.isEmpty(), qUtf8Printable(violations.join(QStringLiteral("; "))));
    }
};

QTEST_APPLESS_MAIN(TestArchitecture)
#include "tst_architecture.moc"
