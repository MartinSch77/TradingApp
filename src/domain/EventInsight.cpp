#include "domain/EventInsight.h"

#include <QLatin1Char>
#include <QLatin1String>

namespace trading {

double parseNum(const QString &s, bool *ok)
{
    QString clean;
    for (const QChar &c : s) {
        if (c.isDigit() || (c == QLatin1Char('.')) || (c == QLatin1Char('-'))
            || (c == QLatin1Char('+'))) {
            clean += c;
        } else if (!clean.isEmpty()) {
            break;
        } else {
            // separator before the number starts — keep scanning
        }
    }
    return clean.toDouble(ok);
}

ImpactGuess guessImpact(const EconomicEvent &e)
{
    const QString t = e.title.toLower();
    auto has = [&t](const char *k) { return t.contains(QLatin1String(k)); };

    bool haveF = false;
    bool haveP = false;
    const double f = parseNum(e.forecast, &haveF);
    const double p = parseNum(e.previous, &haveP);
    const bool haveDelta = haveF && haveP;
    const double delta = haveDelta ? (f - p) : 0.0;

    qint32 dir = 0;  // +1 bullish, -1 bearish, 0 uncertain/volatile
    QString reason;

    auto fromDelta = [&dir, &reason, haveDelta, delta](
                         bool higherIsBullish, const QString &up, const QString &down,
                         const QString &flat, const QString &noData) {
        if (!haveDelta) {
            dir = 0;
            reason = noData;
            return;
        }
        if (qFuzzyIsNull(delta)) {
            dir = 0;
            reason = flat;
        } else {
            const bool higher = delta > 0.0;
            dir = (higher == higherIsBullish) ? 1 : -1;
            reason = higher ? up : down;
        }
    };

    if (has("fomc") || has("interest rate") || has("federal funds") || has("fed funds")
        || has("rate decision")) {
        // Higher policy rate is bearish for equities.
        fromDelta(false, QStringLiteral("rate hike expected"), QStringLiteral("rate cut expected"),
                  QStringLiteral("rate hold expected"), QStringLiteral("policy event — expect swings"));
    } else if (has("cpi") || has("inflation") || has("ppi") || has("pce")) {
        // Hotter inflation is bearish (tighter-policy fears).
        fromDelta(false, QStringLiteral("hotter inflation"), QStringLiteral("cooler inflation"),
                  QStringLiteral("inflation steady"), QStringLiteral("inflation data — expect swings"));
    } else if (has("unemploy")) {
        // Rising unemployment is bearish for growth.
        fromDelta(false, QStringLiteral("rising unemployment"),
                  QStringLiteral("falling unemployment"), QStringLiteral("unemployment steady"),
                  QStringLiteral("labor data — expect swings"));
    } else if (has("payroll") || has("employment") || has("gdp") || has("retail sales")
               || has("ism") || has("pmi") || has("confidence") || has("sentiment")) {
        // Stronger growth/activity is bullish.
        fromDelta(true, QStringLiteral("stronger data"), QStringLiteral("weaker data"),
                  QStringLiteral("in line with prior"), QStringLiteral("growth data — expect swings"));
    } else {
        dir = 0;
        reason = QStringLiteral("expect swings");
    }

    QString strength = QStringLiteral("minor");
    if (e.impact.compare(QLatin1String("High"), Qt::CaseInsensitive) == 0) {
        strength = QStringLiteral("strong");
    } else if (e.impact.compare(QLatin1String("Medium"), Qt::CaseInsensitive) == 0) {
        strength = QStringLiteral("moderate");
    } else {
        // low impact — keep the "minor" default
    }

    ImpactGuess g;
    g.dir = dir;
    if (dir > 0) {
        g.text = QStringLiteral("↑ bullish (%1) — %2").arg(strength, reason);
    } else if (dir < 0) {
        g.text = QStringLiteral("↓ bearish (%1) — %2").arg(strength, reason);
    } else {
        g.text = QStringLiteral("↕ volatile (%1) — %2").arg(strength, reason);
    }
    return g;
}

QString eventAbout(const EconomicEvent &e, const QString &symbol)
{
    const QString t = e.title.toLower();
    auto has = [&t](const char *k) { return t.contains(QLatin1String(k)); };

    if (has("fomc") || has("interest rate") || has("federal funds") || has("fed funds")
        || has("rate decision")) {
        return QStringLiteral(
            "A central bank's policy interest-rate decision (with its statement / "
            "press conference). Higher rates raise borrowing costs and usually weigh on "
            "equities; cuts or dovish guidance tend to lift them. One of the highest-"
            "volatility events for %1.").arg(symbol);
    }
    if (has("cpi") || has("inflation") || has("ppi") || has("pce")) {
        return QStringLiteral(
            "An inflation gauge — how fast consumer or producer prices are rising. "
            "Hotter-than-expected inflation revives fears of tighter monetary policy and "
            "pressures equities; a cooler print is usually risk-on.");
    }
    if (has("unemploy")) {
        return QStringLiteral(
            "The share of the labour force that is jobless. A rising rate points to a "
            "cooling economy (bearish for growth); a falling rate signals strength.");
    }
    if (has("payroll") || has("nonfarm") || has("non-farm") || has("jobless")
        || has("employment")) {
        return QStringLiteral(
            "Jobs data (e.g. payrolls or jobless claims). Stronger job growth signals a "
            "healthy economy and is generally bullish, though a very hot print can revive "
            "rate-hike fears.");
    }
    if (has("gdp")) {
        return QStringLiteral(
            "Gross Domestic Product — the broadest measure of an economy's output. "
            "Stronger growth is generally bullish for equities.");
    }
    if (has("retail sales")) {
        return QStringLiteral(
            "How much consumers spent at retailers — a timely read on demand, which "
            "drives much of the economy. Stronger sales are generally bullish.");
    }
    if (has("ism") || has("pmi")) {
        return QStringLiteral(
            "A purchasing-managers survey of business activity. Above 50 means "
            "expansion, below 50 contraction; stronger figures are generally bullish.");
    }
    if (has("confidence") || has("sentiment")) {
        return QStringLiteral(
            "A survey of how optimistic consumers or businesses feel. Higher readings "
            "point to more spending and investment ahead and are generally bullish.");
    }
    return QStringLiteral(
               "A scheduled macro-economic release. A surprise versus the forecast can "
               "move %1; the direction depends on the actual figure when it prints.")
        .arg(symbol);
}

} // namespace trading
