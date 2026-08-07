// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/EventInsight.h"

#include <QLatin1Char>
#include <QLatin1String>

#include <algorithm>
#include <initializer_list>

namespace trading {

std::optional<double> parseNum(const QString &s)
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
    bool ok = false;
    const double value = clean.toDouble(&ok);
    if (ok) {
        return value;
    }
    return std::nullopt;
}

ImpactGuess guessImpact(const EconomicEvent &e)
{
    const QString t = e.title.toLower();
    auto has = [&t](const char *k) { return t.contains(QLatin1String(k)); };
    // Keyword groups are checked via any_of instead of long || chains: keeps
    // every decision within the 6 conditions clang-18 can instrument for MC/DC.
    auto hasAny = [&has](std::initializer_list<const char *> keys) {
        return std::any_of(keys.begin(), keys.end(), has);
    };

    const std::optional<double> f = parseNum(e.forecast);
    const std::optional<double> p = parseNum(e.previous);
    const bool haveDelta = f.has_value() && p.has_value();
    const double delta = haveDelta ? (*f - *p) : 0.0;

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

    if (hasAny({"fomc", "interest rate", "federal funds", "fed funds", "rate decision"})) {
        // Higher policy rate is bearish for equities.
        fromDelta(false, QStringLiteral("rate hike expected"), QStringLiteral("rate cut expected"),
                  QStringLiteral("rate hold expected"), QStringLiteral("policy event — expect swings"));
    } else if (hasAny({"cpi", "inflation", "ppi", "pce"})) {
        // Hotter inflation is bearish (tighter-policy fears).
        fromDelta(false, QStringLiteral("hotter inflation"), QStringLiteral("cooler inflation"),
                  QStringLiteral("inflation steady"), QStringLiteral("inflation data — expect swings"));
    } else if (has("unemploy")) {
        // Rising unemployment is bearish for growth.
        fromDelta(false, QStringLiteral("rising unemployment"),
                  QStringLiteral("falling unemployment"), QStringLiteral("unemployment steady"),
                  QStringLiteral("labor data — expect swings"));
    } else if (hasAny({"payroll", "employment", "gdp", "retail sales", "ism", "pmi",
                       "confidence", "sentiment"})) {
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

EventProposal proposeActivity(const EconomicEvent &e)
{
    EventProposal p;
    const ImpactGuess g = guessImpact(e);
    const QString when = e.when.isValid()
                             ? e.when.toLocalTime().toString(QStringLiteral("HH:mm"))
                             : QStringLiteral("the release");

    // Only the parse-success flags matter here — the direction itself comes
    // from guessImpact(), and the rationale quotes the raw feed strings.
    const bool haveDelta =
        parseNum(e.forecast).has_value() && parseNum(e.previous).has_value();
    const bool highImpact = e.impact.compare(QLatin1String("High"), Qt::CaseInsensitive) == 0;

    if (!haveDelta || (g.dir == 0)) {
        // No forecast-vs-previous comparison (or the class of event gives no
        // direction): the reaction depends purely on the surprise — a coin flip.
        p.action = QStringLiteral("STAY OUT");
        p.timing = QStringLiteral("from ~30 min before %1 until the market has digested the print")
                       .arg(when);
        p.rationale = haveDelta
                          ? QStringLiteral("the event class gives no directional read (%1)")
                                .arg(g.text)
                          : QStringLiteral("no forecast/previous comparison available — the "
                                           "reaction depends entirely on the surprise");
        return p;
    }

    p.actionable = true;
    p.dir = g.dir;
    p.action = (g.dir > 0) ? QStringLiteral("BUY") : QStringLiteral("SELL");
    const QString deltaText =
        QStringLiteral("forecast %1 vs previous %2 → %3")
            .arg(e.forecast, e.previous, g.text);
    if (highImpact) {
        // High-impact prints gap and whipsaw: being positioned into the release is
        // a bet on the surprise, not on the consensus. Enter once the print confirms.
        p.timing = QStringLiteral("AFTER the %1 print, once it confirms this direction").arg(when);
        p.rationale = deltaText
                      + QStringLiteral("; high-impact releases gap and whipsaw — entering "
                                       "before is a bet on the surprise, not the consensus");
    } else {
        // Lower-impact consensus tends to get priced in gradually into the release.
        p.timing = QStringLiteral("BEFORE %1, with a tight stop").arg(when);
        p.rationale = deltaText
                      + QStringLiteral("; lower-impact consensus tends to be priced in "
                                       "gradually ahead of the release");
    }
    return p;
}

QString eventAbout(const EconomicEvent &e, const QString &symbol)
{
    const QString t = e.title.toLower();
    auto has = [&t](const char *k) { return t.contains(QLatin1String(k)); };
    auto hasAny = [&has](std::initializer_list<const char *> keys) {
        return std::any_of(keys.begin(), keys.end(), has);
    };

    if (hasAny({"fomc", "interest rate", "federal funds", "fed funds", "rate decision"})) {
        return QStringLiteral(
            "A central bank's policy interest-rate decision (with its statement / "
            "press conference). Higher rates raise borrowing costs and usually weigh on "
            "equities; cuts or dovish guidance tend to lift them. One of the highest-"
            "volatility events for %1.").arg(symbol);
    }
    if (hasAny({"cpi", "inflation", "ppi", "pce"})) {
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
    if (hasAny({"payroll", "nonfarm", "non-farm", "jobless", "employment"})) {
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
    if (hasAny({"ism", "pmi"})) {
        return QStringLiteral(
            "A purchasing-managers survey of business activity. Above 50 means "
            "expansion, below 50 contraction; stronger figures are generally bullish.");
    }
    if (hasAny({"confidence", "sentiment"})) {
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
