// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/HeavyweightsPanel.h"

#include "domain/LeadSignal.h"
#include "ui/Palette.h"

#include <QChart>
#include <QChartView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLegendMarker>
#include <QLineSeries>
#include <QPainter>
#include <QPen>
#include <QSet>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QValueAxis>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

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
    , m_liveTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("heavyweightsPanel"));
    setWindowTitle(QStringLiteral("Index heavyweights — an early read on SPX500 and NSDQ100"));
    buildUi();
    // 60 s is the data's OWN resolution: the constituent series are 1-minute closes,
    // so asking more often would fetch the same numbers and spend someone else's rate
    // limit doing it.
    m_liveTimer->setInterval(60 * 1000);
    static_cast<void>(connect(m_liveTimer, &QTimer::timeout, this,
                              &HeavyweightsPanel::refreshRequested));
}

void HeavyweightsPanel::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // State the combined indication IMMEDIATELY, from whatever is cached — which on a
    // first open is nothing. An empty book produces "No usable read: 0 of N inputs
    // measurable" plus each read listed as unmeasurable, and that is the honest answer;
    // the ellipsis these labels are built with is not. A window that shows "…" until a
    // feed happens to arrive cannot be told apart from one whose feeds are all down.
    updateLeadSignals(m_series);
    // Ask at once, then every minute: a window opened mid-session should not wait for
    // the next five-minute scan to show anything.
    emit refreshRequested();
    m_liveTimer->start();
}

void HeavyweightsPanel::hideEvent(QHideEvent *event)
{
    QDialog::hideEvent(event);
    m_liveTimer->stop();   // a closed window costs nothing
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

    // The movers as CURVES. Each constituent is normalised to its own session start,
    // so ten names at ten price levels share one axis and the question the chart is
    // for — are they moving TOGETHER — can be answered by looking at it.
    m_chart = new QChart();
    m_chart->setTitle(QStringLiteral("Nasdaq-100 heavyweights — % from their own session open"));
    m_chart->legend()->setAlignment(Qt::AlignBottom);
    m_axisX = new QValueAxis();
    m_axisX->setTitleText(QStringLiteral("minutes into the session"));
    m_axisY = new QValueAxis();
    m_axisY->setTitleText(QStringLiteral("% from open"));
    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    m_chartView = new QChartView(m_chart, this);
    m_chartView->setObjectName(QStringLiteral("heavyChart"));
    m_chartView->setRenderHint(QPainter::Antialiasing);
    m_chartView->setMinimumHeight(260);
    outer->addWidget(m_chartView, 1);

    // The combined indication, per index: what the independent reads, the constituent
    // field, the clock and the regime say TOGETHER, and the leverage that evidence
    // justifies (REQ-F-036).
    m_nasdaqLead = new QLabel(QStringLiteral("…"), this);
    m_nasdaqLead->setObjectName(QStringLiteral("nasdaqLeadSignal"));
    m_nasdaqLead->setWordWrap(true);
    m_nasdaqLead->setTextFormat(Qt::RichText);
    outer->addWidget(m_nasdaqLead);
    m_spLead = new QLabel(QStringLiteral("…"), this);
    m_spLead->setObjectName(QStringLiteral("spLeadSignal"));
    m_spLead->setWordWrap(true);
    m_spLead->setTextFormat(Qt::RichText);
    outer->addWidget(m_spLead);

    m_updated = new QLabel(QStringLiteral("waiting for the first constituent prices…"), this);
    m_updated->setObjectName(QStringLiteral("heavyUpdated"));
    outer->addWidget(m_updated);

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

    resize(980, 820);
}

void HeavyweightsPanel::fillTable(QTableWidget *table, QLabel *summary,
                                  const HeavyweightPulse &pulse)
{
    if ((table == nullptr) || (summary == nullptr)) {
        return;
    }
    // Ordered by the SIZE of the move, biggest first: these are the top movers, and a
    // name that has not moved says nothing about where the index goes next. Unknown
    // rows sort last — they are not "no movement", they are no reading.
    QList<HeavyweightRow> ordered = pulse.rows;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const HeavyweightRow &a, const HeavyweightRow &b) {
                         if (a.known != b.known) {
                             return a.known;
                         }
                         return std::abs(a.changePct) > std::abs(b.changePct);
                     });
    table->setRowCount(static_cast<int>(ordered.size()));
    int row = 0;
    for (const HeavyweightRow &entry : ordered) {
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

void HeavyweightsPanel::fillChart(const QHash<QString, QList<double>> &series)
{
    if (m_chart == nullptr) {
        return;
    }
    // Rebuilt rather than updated in place: this window is opened to be looked at,
    // not polled at 1 Hz, so the simple thing is the right thing here (the per-tick
    // allocation-free rule of REQ-N-006 governs the open-trades table, not this).
    for (QLineSeries *curve : std::as_const(m_curves)) {
        m_chart->removeSeries(curve);
        delete curve;
    }
    m_curves.clear();

    // ALL TEN Nasdaq-100 constituents, every one of them — this is the field whose
    // movement is meant to lead NSDQ100, so a chart that dropped the quiet ones would
    // answer a different question ("who moved") than the one it is for ("are they
    // moving together"). Ordered by size of move so the legend reads top-down.
    struct Curve {
        QString ticker;
        QList<double> pct;
        double move = 0.0;
    };
    QList<Curve> curves;
    for (const QString &ticker : nasdaqHeavyweights()) {
        const QList<double> raw = series.value(ticker);
        if ((raw.size() < 2) || (raw.constFirst() <= 0.0)) {
            continue;   // unreadable: absent from the chart rather than drawn flat
        }
        // EXACTLY two points means the series is the meta-block fallback
        // (MarketFeeds::yahooMetaSessionChange): yesterday's close and the live price,
        // because the feed served no intraday bars for this ticker. That is a perfectly
        // good SESSION CHANGE — the table above shows it — but it is not a session SHAPE,
        // and drawing it would render a straight diagonal that reads as "this name moved
        // in one smooth line all day". The chart answers "are they moving together", which
        // needs real bars, so a two-point series is left out of it on purpose.
        if (raw.size() == 2) {
            continue;
        }
        Curve c;
        c.ticker = ticker;
        c.pct.reserve(raw.size());
        const double base = raw.constFirst();
        for (const double price : raw) {
            c.pct.append(((price - base) / base) * 100.0);
        }
        c.move = std::abs(c.pct.constLast());
        curves.append(c);
    }
    std::stable_sort(curves.begin(), curves.end(),
                     [](const Curve &a, const Curve &b) { return a.move > b.move; });

    double lo = 0.0;
    double hi = 0.0;
    qsizetype longest = 0;
    for (const Curve &c : curves) {
        auto *line = new QLineSeries();
        line->setName(c.ticker);
        for (qsizetype x = 0; x < c.pct.size(); ++x) {
            line->append(static_cast<double>(x), c.pct.at(x));
            lo = std::min(lo, c.pct.at(x));
            hi = std::max(hi, c.pct.at(x));
        }
        longest = std::max(longest, c.pct.size());
        m_chart->addSeries(line);
        line->attachAxis(m_axisX);
        line->attachAxis(m_axisY);
        m_curves.append(line);
    }

    if (m_curves.isEmpty()) {
        m_chart->setTitle(QStringLiteral("No constituent series yet — nothing to plot"));
        return;
    }
    m_chart->setTitle(QStringLiteral("Nasdaq-100: all %1 heavyweights, %% from their own "
                                     "session open")
                          .arg(m_curves.size()));
    m_axisX->setRange(0.0, static_cast<double>(std::max<qsizetype>(longest - 1, 1)));
    // A little headroom, and never a zero-height axis on a flat morning.
    const double pad = std::max(0.1, (hi - lo) * 0.1);
    m_axisY->setRange(lo - pad, hi + pad);
}

void HeavyweightsPanel::updateLeadSignals(const QHash<QString, QList<double>> &series)
{
    const auto describe = [this, &series](QLabel *label, const QString &symbol) {
        if (label == nullptr) {
            return;
        }
        LeadInputs in;
        in.symbol = symbol;
        // Through the one factory, so this window reads the SAME books the bot does.
        // It used to pass the ticker book where a per-symbol series was wanted, which
        // silently reduced this display to the reads that need neither the futures nor
        // the instrument's own session.
        in.reads = indexReads(symbol, readInputsFor(symbol, series, m_volumes, m_symbolSeries));
        in.pulse = heavyweightPulse(symbol, series);
        in.now = QDateTime::currentDateTimeUtc();
        in.vixValid = m_vixValid;
        in.vix = m_vix;
        in.eventRisk = m_eventRisk;
        in.term = termStructure(series);
        const LeadSignal signal = leadSignal(in);
        const QString colour = signal.actionable()
                                   ? ((signal.dir > 0) ? greenHex() : redHex())
                                   : greyHex();
        label->setText(QStringLiteral("<span style='color:%1'><b>%2</b></span><br/>"
                                      "<span style='color:%3'>%4</span>")
                           .arg(colour, signal.headline, greyHex(),
                                signal.reasons.join(QStringLiteral(" · "))));
        label->setToolTip(
            QStringLiteral("The combined indication (REQ-F-036): the nine independent reads, "
                           "the constituent field, the session phase and the regime, scored "
                           "together. An input that could not be measured contributes NOTHING "
                           "and is listed as unmeasurable — and the strength is capped by how "
                           "much was measurable, so a strong-looking read built from two feeds "
                           "cannot be mistaken for one built from six. The leverage it names is "
                           "an UPPER BOUND from the evidence; the risk budget, the correlation "
                           "bucket cap and the instrument's own ladder all still apply."));
    };
    describe(m_nasdaqLead, QStringLiteral("NSDQ100"));
    describe(m_spLead, QStringLiteral("SPX500"));
}

void HeavyweightsPanel::setRegime(bool vixValid, double vix, bool eventRisk)
{
    m_vixValid = vixValid;
    m_vix = vix;
    m_eventRisk = eventRisk;
    if (!m_series.isEmpty()) {
        updateLeadSignals(m_series);
    }
}

void HeavyweightsPanel::setVolumeSeries(const QHash<QString, trading::VolumeSeries> &volumes)
{
    m_volumes = volumes;
    // No redraw here: the closes for these very tickers arrive from the same sweep and
    // setReferenceSeries — which does redraw — is called for each of them.
}

void HeavyweightsPanel::setSymbolSeries(const QHash<QString, QList<double>> &series)
{
    m_symbolSeries = series;
}

void HeavyweightsPanel::setReferenceSeries(const QHash<QString, QList<double>> &series)
{
    m_series = series;
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
    fillChart(series);
    updateLeadSignals(series);
    if (m_updated != nullptr) {
        const HeavyweightPulse nasdaqPulse =
            heavyweightPulse(QStringLiteral("NSDQ100"), series);
        m_updated->setText(
            QStringLiteral("Updated %1 · %2 of %3 Nasdaq-100 constituents reading · refreshing "
                           "every 60 s while this window is open")
                .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")))
                .arg(nasdaqPulse.measured)
                .arg(nasdaqPulse.rows.size()));
    }
}

} // namespace trading::ui
