// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_LEADGAUGE_H
#define TRADINGAPP_UI_LEADGAUGE_H

#include <QList>
#include <QSize>
#include <QString>
#include <QWidget>

class QPaintEvent;

namespace trading::ui {

// One index's cap-weighted lead, as the gauge draws it.
struct GaugeRow {
    QString label;              // "S&P 500" / "Nasdaq-100"
    double capWeightedPct = 0.0;
    int up = 0;
    int measured = 0;
    bool known = false;        // false => nothing measured; drawn as "—", never a zero bar
                               // (the absent-is-not-zero rule the whole app follows)
};

// The bar geometry for one value: how far it extends from the central zero line, and to
// which side. Pure and header-declared so it is TESTED headless — the visual claim the gauge
// makes ("this index is +0.26 %, this far up") rests entirely on it, and a gauge whose bar
// length is wrong lies exactly the way a mislabelled number would.
struct GaugeBar {
    int extentPx = 0;          // 0..halfTrackPx, clamped
    bool up = true;            // true => right of zero (>= 0), false => left (< 0)
};
[[nodiscard]] GaugeBar leadGaugeBar(double valuePct, double maxAbsPct, int halfTrackPx);

// A small diverging-bar gauge: one horizontal bar per index from a central zero line —
// right/▲ for a positive cap-weighted lead, left/▼ for negative, length = |lead| against a
// fixed full-scale. Direction is the SIDE, the arrow AND the sign, never colour alone, so it
// reads in a monochrome capture and for a colour-blind trader, the same discipline as the
// confluence meter and the constituent tables. Fed the same HeavyweightPulse numbers the
// tables use, so the bar and the row can never disagree.
class LeadGauge : public QWidget
{
    Q_OBJECT;   // ";" so tree-sitter/moc see the anchor

public:
    explicit LeadGauge(QWidget *parent = nullptr);

    void setRows(const QList<GaugeRow> &rows);
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QList<GaugeRow> m_rows;
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_LEADGAUGE_H
