#ifndef TRADINGAPP_UI_TRADEGAUGE_H
#define TRADINGAPP_UI_TRADEGAUGE_H

#include "domain/Models.h"

#include <QDialog>
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QLabel)

// Circular price gauge for one open trade: the needle is the live price, the
// arc spans stop-loss (red zone) → open rate (marker) → take-profit (green
// zone). Pure QPainter — no extra Qt modules, works on any backend (REQ-F-024).
class TradeGaugeWidget : public QWidget
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees markers
public:
    explicit TradeGaugeWidget(QWidget *parent = nullptr);

    void setTrade(const Position &position, double currentPrice);
    void setCurrentPrice(double price);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    [[nodiscard]] static double valueToAngle(double value, double lo, double hi);

    Position m_pos;
    double m_price = 0.0;
};

// Non-modal detail window opened by clicking an open-trades row: gauge plus
// the buy (open) value, current value, live P/L and the SL/TP amounts.
class TradeGaugeDialog : public QDialog
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees markers
public:
    explicit TradeGaugeDialog(QWidget *parent = nullptr);

    // Show/refresh the dialog for one trade. ccySymbol/eurPerUsd render the
    // account-currency figures in the display currency, like the main table.
    void showTrade(const Position &position, double currentPrice,
                   const QString &ccySymbol, double eurPerUsd);
    // Live price tick for the shown trade's instrument.
    void updatePrice(double price);
    [[nodiscard]] QString symbol() const { return m_pos.symbol; }

private:
    void renderLabels();

    TradeGaugeWidget *m_gauge = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_open = nullptr;
    QLabel *m_current = nullptr;
    QLabel *m_pl = nullptr;
    QLabel *m_targets = nullptr;
    Position m_pos;
    double m_price = 0.0;
    QString m_ccy;
    double m_eurPerUsd = 1.0;
};

#endif // TRADINGAPP_UI_TRADEGAUGE_H
