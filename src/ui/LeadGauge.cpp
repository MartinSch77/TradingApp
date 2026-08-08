// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/LeadGauge.h"

#include "ui/Palette.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRect>

#include <algorithm>
#include <cmath>

namespace trading::ui {

namespace {
// ±2 % of cap-weighted move fills the bar; a larger move clamps to the end rather than
// running off the track. The megacap fields rarely move more than that intraday, so the
// full width is used without the scale jumping around (a fixed scale reads more honestly
// than one that rescales every tick).
constexpr double kFullScalePct = 2.0;
constexpr int kRowHeight = 34;
constexpr int kLabelWidth = 104;
constexpr int kValueWidth = 168;
constexpr int kBarHeight = 12;
constexpr int kSideMargin = 8;
} // namespace

GaugeBar leadGaugeBar(double valuePct, double maxAbsPct, int halfTrackPx)
{
    GaugeBar bar;
    bar.up = valuePct >= 0.0;
    if ((maxAbsPct <= 0.0) || (halfTrackPx <= 0)) {
        return bar;
    }
    const double frac = std::min(std::abs(valuePct) / maxAbsPct, 1.0);
    bar.extentPx = static_cast<int>(std::lround(frac * static_cast<double>(halfTrackPx)));
    return bar;
}

LeadGauge::LeadGauge(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("leadGauge"));
    setMinimumHeight((kRowHeight * 2) + 8);
}

void LeadGauge::setRows(const QList<GaugeRow> &rows)
{
    m_rows = rows;
    updateGeometry();
    update();
}

QSize LeadGauge::sizeHint() const
{
    const int rows = std::max<int>(1, static_cast<int>(m_rows.size()));
    return {kLabelWidth + 220 + kValueWidth, (kRowHeight * rows) + 8};
}

void LeadGauge::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = palette().color(QPalette::WindowText);

    const int trackX0 = kLabelWidth;
    const int trackX1 = width() - kValueWidth;
    const int trackW = std::max(20, trackX1 - trackX0);
    const int centerX = trackX0 + (trackW / 2);
    const int halfTrack = (trackW / 2) - kSideMargin;

    for (int i = 0; i < m_rows.size(); ++i) {
        const GaugeRow &row = m_rows.at(i);
        const int y = i * kRowHeight;
        const int midY = y + (kRowHeight / 2);

        // The index name, left, in text ink (never a series colour).
        painter.setPen(ink);
        painter.drawText(QRect(0, y, kLabelWidth - 6, kRowHeight),
                         Qt::AlignVCenter | Qt::AlignLeft, row.label);

        // The central zero line the bars diverge from.
        painter.setPen(QPen(kGrey, 1));
        painter.drawLine(centerX, y + 6, centerX, y + kRowHeight - 6);

        if (!row.known) {
            // Absent, not flat: a dash and the word, never a zero-length bar at the centre.
            painter.setPen(kGrey);
            painter.drawText(QRect(trackX0, y, trackW, kRowHeight), Qt::AlignCenter,
                             QStringLiteral("—"));
            painter.drawText(QRect(trackX1 + 6, y, kValueWidth - 6, kRowHeight),
                             Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("no prices"));
            continue;
        }

        const GaugeBar bar = leadGaugeBar(row.capWeightedPct, kFullScalePct, halfTrack);
        const bool positive = row.capWeightedPct > 0.0;
        const QColor barColour = (row.capWeightedPct > 0.0) ? kGreen
                                 : (row.capWeightedPct < 0.0) ? kRed
                                                              : kGrey;
        const int barY = midY - (kBarHeight / 2);
        const int barX = bar.up ? centerX : (centerX - bar.extentPx);
        painter.setPen(Qt::NoPen);
        painter.setBrush(barColour);
        painter.drawRoundedRect(QRect(barX, barY, std::max(2, bar.extentPx), kBarHeight), 3, 3);

        // Direction is the bar SIDE, the arrow AND the sign — colour is only a fourth cue.
        const QChar arrow = positive ? QChar(0x25B2) : QChar(0x25BC);   // ▲ / ▼
        const QString sign = positive ? QStringLiteral("+") : QString();
        const QString text = QStringLiteral("%1 %2%3%  %4/%5 up")
                                 .arg(arrow)
                                 .arg(sign)
                                 .arg(row.capWeightedPct, 0, 'f', 2)
                                 .arg(row.up)
                                 .arg(row.measured);
        painter.setPen(barColour);
        painter.drawText(QRect(trackX1 + 6, y, kValueWidth - 6, kRowHeight),
                         Qt::AlignVCenter | Qt::AlignLeft, text);
    }
}

} // namespace trading::ui
