#include "ui/HeavyweightsPanel.h"

#include "ui/Palette.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace trading::ui {

namespace {

// Columns of a constituent table.
enum Column : int { ColName = 0, ColChange = 1, ColState = 2, ColCount = 3 };

QColor colourFor(double changePct)
{
    if (changePct > 0.0) {
        return kGreen;
    }
    if (changePct < 0.0) {
        return kRed;
    }
    return kGrey;
}

// What the two pulses together suggest — stated as a leaning, never as a
// prediction, and explicitly silent when the two disagree or nothing was read.
QString verdictFor(const HeavyweightPulse &nasdaq, const HeavyweightPulse &sp)
{
    if (nasdaq.isEmpty() && sp.isEmpty()) {
        return QStringLiteral("No constituent prices yet — nothing to read.");
    }
    const auto leaning = [](const HeavyweightPulse &p) {
        if (p.isEmpty()) {
            return 0;
        }
        const double share = static_cast<double>(p.up) / static_cast<double>(p.measured);
        if ((share >= 0.7) && (p.averageChangePct > 0.0)) {
            return 1;
        }
        if ((share <= 0.3) && (p.averageChangePct < 0.0)) {
            return -1;
        }
        return 0;
    };
    const int n = leaning(nasdaq);
    const int s = leaning(sp);
    if ((n > 0) && (s > 0)) {
        return QStringLiteral("Both fields are broadly BID — the heavyweights of each index "
                              "are moving together, which is the case where the index "
                              "usually follows.");
    }
    if ((n < 0) && (s < 0)) {
        return QStringLiteral("Both fields are broadly OFFERED — selling is spread across the "
                              "biggest names rather than concentrated in one.");
    }
    if (n != s) {
        return QStringLiteral("The two fields DISAGREE — technology and the broad market are "
                              "not moving together, which is exactly when an index read taken "
                              "from the other one misleads.");
    }
    return QStringLiteral("A split field: the move is carried by some names and resisted by "
                          "others, so it says little about the index yet.");
}

} // namespace

HeavyweightsPanel::HeavyweightsPanel(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("heavyweightsPanel"));
    setWindowTitle(QStringLiteral("Index heavyweights — an early read on SPX500 and NSDQ100"));
    buildUi();
}

void HeavyweightsPanel::buildUi()
{
    auto *outer = new QVBoxLayout(this);

    auto *intro = new QLabel(
        QStringLiteral("The ten biggest constituents of each index, by weight. An index is the "
                       "weighted sum of its members: when the heavyweights are already moving "
                       "together, the index tends to follow — and when one name carries the "
                       "whole move, it tends not to."),
        this);
    intro->setObjectName(QStringLiteral("heavyIntro"));
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto *tables = new QHBoxLayout();
    const auto makeSide = [this, tables](const QString &title, QTableWidget *&table,
                                         QLabel *&summary, const QString &tableName,
                                         const QString &summaryName) {
        auto *column = new QVBoxLayout();
        auto *heading = new QLabel(title, this);
        heading->setObjectName(tableName + QStringLiteral("Heading"));
        column->addWidget(heading);

        table = new QTableWidget(0, ColCount, this);
        table->setObjectName(tableName);
        table->setHorizontalHeaderLabels(
            {QStringLiteral("Name"), QStringLiteral("Session"), QStringLiteral("State")});
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        column->addWidget(table);

        summary = new QLabel(QStringLiteral("…"), this);
        summary->setObjectName(summaryName);
        summary->setWordWrap(true);
        column->addWidget(summary);
        tables->addLayout(column);
    };
    makeSide(QStringLiteral("Nasdaq-100 (NSDQ100)"), m_nasdaqTable, m_nasdaqSummary,
             QStringLiteral("nasdaqHeavyTable"), QStringLiteral("nasdaqHeavySummary"));
    makeSide(QStringLiteral("S&P 500 (SPX500)"), m_spTable, m_spSummary,
             QStringLiteral("spHeavyTable"), QStringLiteral("spHeavySummary"));
    outer->addLayout(tables);

    m_verdict = new QLabel(QStringLiteral("…"), this);
    m_verdict->setObjectName(QStringLiteral("heavyVerdict"));
    m_verdict->setWordWrap(true);
    m_verdict->setTextFormat(Qt::RichText);
    outer->addWidget(m_verdict);

    // The caveat is part of the window, not a footnote to be trimmed: this view is a
    // stand-in for breadth, and presenting a stand-in as the real thing is worse than
    // an honest gap.
    m_caveat = new QLabel(
        QStringLiteral("<i>These ten names are a STAND-IN for market breadth. Real breadth is "
                       "the advance/decline line over every constituent with its index weight, "
                       "which this app does not fetch. A name whose price could not be read is "
                       "shown as unknown rather than as 0.00 %.</i>"),
        this);
    m_caveat->setObjectName(QStringLiteral("heavyCaveat"));
    m_caveat->setWordWrap(true);
    m_caveat->setTextFormat(Qt::RichText);
    outer->addWidget(m_caveat);

    resize(820, 560);
}

void HeavyweightsPanel::fillTable(QTableWidget *table, QLabel *summary,
                                  const HeavyweightPulse &pulse)
{
    if ((table == nullptr) || (summary == nullptr)) {
        return;
    }
    table->setRowCount(static_cast<int>(pulse.rows.size()));
    int row = 0;
    for (const HeavyweightRow &entry : pulse.rows) {
        auto *name = new QTableWidgetItem(entry.ticker);
        table->setItem(row, ColName, name);

        // Unknown is spelled out; a dash in the number column and the word in the
        // state column, so no reader mistakes it for a flat session.
        auto *change = new QTableWidgetItem(
            entry.known ? QStringLiteral("%1%2 %")
                              .arg((entry.changePct > 0.0) ? QStringLiteral("+") : QString())
                              .arg(entry.changePct, 0, 'f', 2)
                        : QStringLiteral("—"));
        change->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (entry.known) {
            change->setForeground(colourFor(entry.changePct));
        }
        table->setItem(row, ColChange, change);

        auto *state = new QTableWidgetItem(
            entry.known ? ((entry.changePct > 0.0) ? QStringLiteral("up")
                                                   : ((entry.changePct < 0.0)
                                                          ? QStringLiteral("down")
                                                          : QStringLiteral("flat")))
                        : QStringLiteral("not read"));
        if (!entry.known) {
            state->setForeground(kGrey);
        }
        table->setItem(row, ColState, state);
        ++row;
    }
    table->resizeColumnsToContents();
    summary->setText(pulse.headline());
}

void HeavyweightsPanel::setReferenceSeries(const QHash<QString, QList<double>> &series)
{
    const HeavyweightPulse nasdaq = heavyweightPulse(QStringLiteral("NSDQ100"), series);
    const HeavyweightPulse sp = heavyweightPulse(QStringLiteral("SPX500"), series);
    fillTable(m_nasdaqTable, m_nasdaqSummary, nasdaq);
    fillTable(m_spTable, m_spSummary, sp);

    const QString verdict = verdictFor(nasdaq, sp);
    const QString colour = verdict.contains(QStringLiteral("BID"))
                               ? greenHex()
                               : (verdict.contains(QStringLiteral("OFFERED")) ? redHex()
                                                                              : greyHex());
    m_verdict->setText(
        QStringLiteral("<span style='color:%1'><b>%2</b></span>").arg(colour, verdict));
}

} // namespace trading::ui
