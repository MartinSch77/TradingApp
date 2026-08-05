#include "ui/ScreenerDialog.h"

#include "domain/PositionMath.h"
#include "domain/SignalEnsemble.h"
#include "ui/Palette.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

ScreenerDialog::ScreenerDialog(QWidget *parent)
    : QDialog(parent), m_table(new QTableWidget(this)), m_status(new QLabel(this))
{
    setWindowTitle(QStringLiteral("Leverage screener"));
    resize(700, 540);

    auto *lay = new QVBoxLayout(this);

    auto *intro = new QLabel(
        QStringLiteral("Every tradable instrument with a BUY / SELL / NEUTRAL call from the same "
                       "indicator ensemble as the live panel — BUY calls first, then by "
                       "confidence. Double-click a row to switch the app to that instrument."),
        this);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Instrument"), QStringLiteral("Max lev"), QStringLiteral("Signal"),
         QStringLiteral("Confidence"), QStringLiteral("Trend"), QStringLiteral("Vol/bar"),
         QStringLiteral("Last")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    // Double-click a row -> trade that instrument (same as picking it in the header).
    static_cast<void>(connect(m_table, &QTableWidget::cellDoubleClicked, this,
                              [this](int row, int) {
                                  QTableWidgetItem *it = m_table->item(row, 0);
                                  if (it == nullptr) {
                                      return;
                                  }
                                  const QString sym = it->data(Qt::UserRole).toString();
                                  if (sym.isEmpty()) {
                                      return;
                                  }
                                  emit instrumentChosen(sym);
                                  accept();
                              }));
    lay->addWidget(m_table);

    auto *footer = new QHBoxLayout;
    
    m_rescan = new QPushButton(QStringLiteral("Rescan"), this);
    m_rescan->setObjectName(QStringLiteral("rescan"));
    static_cast<void>(
        connect(m_rescan, &QPushButton::clicked, this, &ScreenerDialog::rescanRequested));
    auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
    static_cast<void>(connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept));
    footer->addWidget(m_status);
    footer->addStretch();
    footer->addWidget(m_rescan);
    footer->addWidget(closeBtn);
    lay->addLayout(footer);
}

void ScreenerDialog::scanStarted()
{
    m_table->setRowCount(0);
    m_status->setText(QStringLiteral("Scanning…"));
    m_rescan->setEnabled(false);
}

void ScreenerDialog::scanProgress(qint32 done, qint32 total)
{
    m_status->setText(QStringLiteral("Scanning… %1 / %2").arg(done).arg(total));
}

void ScreenerDialog::scanFinished(qsizetype instrumentCount)
{
    m_rescan->setEnabled(true);
    m_status->setText(
        QStringLiteral("%1 instruments — BUY calls first, then by confidence. "
                       "Double-click a row to trade.")
            .arg(instrumentCount));
}

void ScreenerDialog::updateRows(const QList<ScreenerRow> &rows, bool vixValid, double vixLevel,
                                double vixChangePct)
{
    const QString green = trading::ui::greenHex();
    const QString red = trading::ui::redHex();
    const QString amber = trading::ui::amberHex();

    // Compute one view-row per scan result (the buy/sell ensemble + display fields).
    struct View {
        QString symbol;
        qint32 maxLev = 0;
        QString signal;
        qint32 signalDir = 0;    // +1 BUY / -1 SELL / 0 NEUTRAL
        double confidence = 0.0;
        qint32 dir = 0;          // ensemble direction (sign of the net score)
        double vol = 0.0;
        double last = 0.0;
        bool haveSignal = false;
    };
    QList<View> views;
    views.reserve(rows.size());
    for (const ScreenerRow &r : rows) {
        View v;
        v.symbol = r.symbol;
        v.maxLev = r.maxLeverage;
        v.last = r.lastPrice;
        if (r.ok && !r.closes.isEmpty()) {
            const trading::Ensemble e = trading::computeEnsemble(r.closes, vixValid, vixChangePct);
            if (e.valid) {
                v.signal = e.signal;
                v.signalDir = e.signalDir;
                // Same instrument-agnostic VIX-level haircut the live panel applies.
                v.confidence = trading::applyVixHaircut(e.confidence, vixValid, vixLevel);
                v.dir = e.dir;
                v.vol = e.vol;
                v.haveSignal = true;
            }
        }
        views.append(v);
    }

    // Rank: BUY calls first, then everything else (SELL / NEUTRAL / no-data), each
    // ordered by confidence descending; leverage then symbol break the remaining ties
    // so the order stays stable across rescans.
    const auto sortBegin = views.begin();
    const auto sortEnd = views.end();
    std::sort(sortBegin, sortEnd, [](const View &a, const View &b) {
        const bool aBuy = a.signalDir > 0;
        const bool bBuy = b.signalDir > 0;
        if (aBuy != bBuy) {
            return aBuy;
        }
        if (std::abs(a.confidence - b.confidence) > 1e-9) {
            return a.confidence > b.confidence;
        }
        if (a.maxLev != b.maxLev) {
            return a.maxLev > b.maxLev;
        }
        return a.symbol < b.symbol;
    });

    const auto viewCount = static_cast<qint32>(views.size());
    m_table->setRowCount(viewCount);
    for (qint32 row = 0; row < viewCount; ++row) {
        const View &v = views[row];
        auto setCell = [this, row](qint32 col, const QString &text, const QString &hex,
                                   Qt::Alignment align) -> QTableWidgetItem * {
            auto *item = new QTableWidgetItem(text);
            item->setTextAlignment(align);
            if (!hex.isEmpty()) {
                item->setForeground(QColor(hex));
            }
            m_table->setItem(row, col, item);
            return item;
        };

        QTableWidgetItem *nameItem =
            setCell(0, v.symbol, QString(), Qt::AlignLeft | Qt::AlignVCenter);
        nameItem->setData(Qt::UserRole, v.symbol);  // used by the double-click handler

        static_cast<void>(setCell(
            1, (v.maxLev > 0) ? QStringLiteral("x%1").arg(v.maxLev) : QStringLiteral("n/a"),
            QString(), Qt::AlignCenter));

        if (v.haveSignal) {
            const QString sigColor =
                (v.signalDir > 0) ? green : ((v.signalDir < 0) ? red : amber);
            static_cast<void>(setCell(2, v.signal, sigColor, Qt::AlignCenter));
            static_cast<void>(setCell(3, QStringLiteral("%1%").arg(v.confidence, 0, 'f', 0),
                                      sigColor, Qt::AlignCenter));
            const QString trend = (v.dir > 0) ? QStringLiteral("▲ up")
                                              : ((v.dir < 0) ? QStringLiteral("▼ down")
                                                             : QStringLiteral("→ flat"));
            static_cast<void>(setCell(4, trend,
                                      (v.dir > 0) ? green : ((v.dir < 0) ? red : amber),
                                      Qt::AlignCenter));
            static_cast<void>(setCell(5, QStringLiteral("±%1%").arg(v.vol, 0, 'f', 2),
                                      QString(), Qt::AlignCenter));
            static_cast<void>(
                setCell(6,
                        (v.last > 0.0) ? QLocale().toString(v.last, 'f',
                                                            trading::priceDecimals(v.last))
                                       : QStringLiteral("—"),
                        QString(), Qt::AlignRight | Qt::AlignVCenter));
        } else {
            static_cast<void>(setCell(2, QStringLiteral("no data"), amber, Qt::AlignCenter));
            for (qint32 c = 3; c <= 6; ++c) {
                static_cast<void>(setCell(c, QStringLiteral("—"), QString(), Qt::AlignCenter));
            }
        }
    }
}
