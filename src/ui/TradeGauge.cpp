#include "ui/TradeGauge.h"

#include "domain/PositionMath.h"
#include "ui/Palette.h"

#include <QConicalGradient>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
using trading::ui::kGreen;
using trading::ui::kGrey;
using trading::ui::kRed;

const QColor kAmber(0xe0, 0xb0, 0x00);
const QColor kFace(0x20, 0x24, 0x2a);
// The gauge sweeps 270°: 7:30 o'clock (-225° in Qt's angle system) → 4:30 (45°).
constexpr double kStartAngle = 225.0;  // degrees, Qt: 0° = 3 o'clock, CCW positive
constexpr double kSpanAngle = -270.0;

QString formatMoney(const QString &ccy, double v)
{
    const QString amount = QLocale().toString(qAbs(v), 'f', 2);
    return ((v < 0.0) ? QStringLiteral("-") : QString()) + ccy + amount;
}
}  // namespace

TradeGaugeWidget::TradeGaugeWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(280, 240);
}

void TradeGaugeWidget::setTrade(const Position &position, double currentPrice)
{
    m_pos = position;
    m_price = (currentPrice > 0.0) ? currentPrice : position.openRate;
    update();
}

void TradeGaugeWidget::setCurrentPrice(double price)
{
    if (price > 0.0) {
        m_price = price;
        update();
    }
}

double TradeGaugeWidget::valueToAngle(double value, double lo, double hi)
{
    const double span = hi - lo;
    const double frac = (span > 0.0) ? std::clamp((value - lo) / span, 0.0, 1.0) : 0.5;
    return kStartAngle + (frac * kSpanAngle);
}

void TradeGaugeWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const double side = std::min(width(), height() - 20);
    const QRectF face((width() - side) / 2.0 + 10, 10, side - 20, side - 20);

    // Scale: SL and TP bound the dial; without them, ±3% around the open rate.
    const double open = m_pos.openRate;
    const double sl = (m_pos.stopLossRate > 0.0) ? m_pos.stopLossRate : open * 0.97;
    const double tp = (m_pos.takeProfitRate > 0.0) ? m_pos.takeProfitRate : open * 1.03;
    double lo = std::min({sl, tp, open, m_price});
    double hi = std::max({sl, tp, open, m_price});
    const double pad = (hi - lo) * 0.06 + 1e-9;
    lo -= pad;
    hi += pad;

    // Face ring with a soft conical sheen.
    QConicalGradient sheen(face.center(), 90.0);
    sheen.setColorAt(0.0, kFace.lighter(140));
    sheen.setColorAt(0.5, kFace);
    sheen.setColorAt(1.0, kFace.lighter(140));
    p.setPen(Qt::NoPen);
    p.setBrush(sheen);
    p.drawEllipse(face.adjusted(-8, -8, 8, 8));

    // Loss and win zones: from SL to open in red, open to TP in green. For a
    // short position the zones mirror automatically because SL sits above and
    // TP below the open rate on the price scale.
    auto drawZone = [&](double fromValue, double toValue, QColor color) {
        const double a0 = valueToAngle(fromValue, lo, hi);
        const double a1 = valueToAngle(toValue, lo, hi);
        QPen pen(color, side * 0.055, Qt::SolidLine, Qt::FlatCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(face, qRound(a0 * 16.0), qRound((a1 - a0) * 16.0));
    };
    drawZone(sl, open, kRed);
    drawZone(open, tp, kGreen);

    // Open-rate marker (the "buy value" anchor on the dial).
    const double openAngle = valueToAngle(open, lo, hi) * M_PI / 180.0;
    const QPointF c = face.center();
    const double rOuter = face.width() / 2.0 + 10.0;
    const double rInner = face.width() / 2.0 - 14.0;
    p.setPen(QPen(kAmber, 3));
    const double openCos = std::cos(openAngle);
    const double openSin = -std::sin(openAngle);
    const QPointF openDir(openCos, openSin);
    p.drawLine(c + openDir * rInner, c + openDir * rOuter);

    // Needle = live price.
    const double needleAngle = valueToAngle(m_price, lo, hi) * M_PI / 180.0;
    const double needleCos = std::cos(needleAngle);
    const double needleSin = -std::sin(needleAngle);
    const QPointF dir(needleCos, needleSin);
    const QPointF ortho(-dir.y(), dir.x());
    QPainterPath needle;
    needle.moveTo(c + ortho * 4.0);
    needle.lineTo(c + dir * (rInner - 6.0));
    needle.lineTo(c - ortho * 4.0);
    needle.closeSubpath();
    const bool winning = m_pos.isBuy ? (m_price >= open) : (m_price <= open);
    p.setPen(Qt::NoPen);
    p.setBrush(winning ? kGreen : kRed);
    p.drawPath(needle);
    p.setBrush(kFace.lighter(180));
    p.drawEllipse(c, 7.0, 7.0);

    // Centre read-out: live price, and the side badge.
    p.setPen(QColor(0xea, 0xea, 0xea));
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 1.5);
    f.setBold(true);
    p.setFont(f);
    const QString priceText =
        QLocale().toString(m_price, 'f', trading::priceDecimals(m_price));
    p.drawText(face.adjusted(0, face.height() * 0.30, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
               priceText);
    f.setPointSizeF(f.pointSizeF() * 0.6);
    p.setFont(f);
    p.setPen(m_pos.isBuy ? kGreen : kRed);
    p.drawText(face.adjusted(0, face.height() * 0.52, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
               m_pos.isBuy ? QStringLiteral("LONG") : QStringLiteral("SHORT"));

    // Dial end labels: SL and TP rates at the arc ends.
    p.setPen(kGrey);
    f.setBold(false);
    p.setFont(f);
    p.drawText(QRectF(face.left() - 6, face.bottom() - 14, face.width() * 0.4, 20),
               Qt::AlignLeft, QLocale().toString(sl, 'f', trading::priceDecimals(sl)));
    p.drawText(QRectF(face.left() + face.width() * 0.6, face.bottom() - 14,
                      face.width() * 0.4 + 6, 20),
               Qt::AlignRight, QLocale().toString(tp, 'f', trading::priceDecimals(tp)));
}

TradeGaugeDialog::TradeGaugeDialog(QWidget *parent)
    : QDialog(parent)
    , m_gauge(new TradeGaugeWidget(this))
    , m_title(new QLabel(this))
    , m_open(new QLabel(this))
    , m_current(new QLabel(this))
    , m_pl(new QLabel(this))
    , m_targets(new QLabel(this))
{
    setWindowTitle(QStringLiteral("Trade gauge"));
    auto *lay = new QVBoxLayout(this);
    m_title->setAlignment(Qt::AlignHCenter);
    QFont f = m_title->font();
    f.setBold(true);
    m_title->setFont(f);
    lay->addWidget(m_title);
    lay->addWidget(m_gauge, 1);
    for (QLabel *l : {m_open, m_current, m_pl, m_targets}) {
        l->setAlignment(Qt::AlignHCenter);
        lay->addWidget(l);
    }
    resize(340, 420);
}

void TradeGaugeDialog::showTrade(const Position &position, double currentPrice,
                                 const QString &ccySymbol, double eurPerUsd)
{
    m_pos = position;
    m_price = (currentPrice > 0.0) ? currentPrice : position.openRate;
    m_ccy = ccySymbol;
    if (eurPerUsd > 0.0) {
        m_eurPerUsd = eurPerUsd;
    }
    m_gauge->setTrade(m_pos, m_price);
    renderLabels();
    show();
    raise();
}

void TradeGaugeDialog::updatePrice(double price)
{
    if (price <= 0.0) {
        return;
    }
    m_price = price;
    m_gauge->setCurrentPrice(price);
    renderLabels();
}

void TradeGaugeDialog::renderLabels()
{
    m_title->setText(QStringLiteral("%1 · #%2 · %3 x%4")
                         .arg(m_pos.symbol, m_pos.positionId,
                              m_pos.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                         .arg(m_pos.leverage, 0, 'f', 0));
    const QString openRateText =
        QLocale().toString(m_pos.openRate, 'f', trading::priceDecimals(m_pos.openRate));
    const QString stakeText = formatMoney(m_ccy, std::ceil(m_pos.amount * m_eurPerUsd));
    m_open->setText(QStringLiteral("Buy value (open rate): %1 · stake %2")
                        .arg(openRateText, stakeText));
    m_current->setText(QStringLiteral("Current value: %1")
                           .arg(QLocale().toString(m_price, 'f',
                                                   trading::priceDecimals(m_price))));
    // Live P/L: the last API P/L re-priced with the price move, exactly like
    // the main table (accountValuePerPoint keeps foreign-quote instruments right).
    const double perPoint = trading::accountValuePerPoint(m_pos);
    const double delta =
        (m_pos.isBuy ? 1.0 : -1.0) * perPoint * (m_price - m_pos.openRate);
    const double plUsd = (perPoint > 0.0) ? delta : m_pos.profit;
    const double plDisp = plUsd * m_eurPerUsd;
    m_pl->setText(QStringLiteral("P/L: %1").arg(formatMoney(m_ccy, plDisp)));
    QPalette pal = m_pl->palette();
    pal.setColor(QPalette::WindowText, (plDisp >= 0.0) ? kGreen : kRed);
    m_pl->setPalette(pal);
    const QString slText = trading::slSignedAmountText(m_pos, m_eurPerUsd);
    const QString tpText = trading::slTpAmountText(m_pos, m_pos.takeProfitRate, m_eurPerUsd);
    const QString slLabel = slText.isEmpty() ? QStringLiteral("—") : m_ccy + slText;
    const QString tpLabel = tpText.isEmpty() ? QStringLiteral("—") : m_ccy + tpText;
    m_targets->setText(
        QStringLiteral("SL %1 · TP (take-profit value) %2").arg(slLabel, tpLabel));
}
