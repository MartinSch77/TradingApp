// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_FORECASTING_H
#define TRADINGAPP_DOMAIN_FORECASTING_H

#include <QList>

// Statistical forecasting models over a close-price series (oldest first).
// Pure computation — no I/O, no UI. Shared by the live signals panel, the
// AI decision-support panel and the decision window.
namespace trading {

// --- Least-squares (OLS) regression forecast --------------------------------
// Fits a line to the last n closes: the slope is the trend (as %/bar) and R²
// measures how well the trend explains the data (0 = noise, 1 = perfect line).
struct Regression {
    double slopePct = 0.0;  // slope per bar, in percent of the mean price
    double r2 = 0.0;        // goodness of fit, 0..1
    bool valid = false;
};

[[nodiscard]] Regression linRegForecast(const QList<double> &values, qsizetype n);

// --- k-Nearest-Neighbors analog forecast ------------------------------------
// Finds the k historical return-windows most similar to the current one and
// predicts the next move as the average of what followed those analogs.
struct Knn {
    double retPct = 0.0;  // predicted next-bar return, percent
    double agree = 0.0;   // fraction of neighbours agreeing on direction
    qint32 k = 0;
};

[[nodiscard]] Knn knnForecast(const QList<double> &values, qsizetype window, qsizetype k);

// Hurst exponent via rescaled-range (R/S) over the last returns. ~0.5 = random
// walk, >0.5 = trending/persistent, <0.5 = mean-reverting. A regime hint.
[[nodiscard]] double hurstExponent(const QList<double> &series);

// Non-parametric Monte-Carlo outlook: bootstrap future paths by resampling the
// series' own recent per-bar returns (captures its real distribution, fat tails
// and all). Reports P(price higher) and a 5–95% range after `horizon` bars, plus
// the probability that a long / short reaches its take-profit before its
// stop-loss (tpFrac / slFrac are the barrier distances as fractions of price)
// and the mirror probability of the stop being struck first. Paths that reach
// NEITHER barrier within the horizon count towards neither figure — the
// remainder 1 − pWin − pLose is the chance the trade simply expires between
// the barriers; expiryRetLong/Short report those paths' measured mean final
// move (signed fraction of price), so expected values need not assume the
// expiring paths close flat (with a directional drift they don't).
struct McOutlook {
    bool valid = false;
    double pUp = 0.5;
    double p5 = 0.0;
    double p95 = 0.0;
    double pWinLong = 0.0;
    double pWinShort = 0.0;
    double pLoseLong = 0.0;   // long's stop-loss struck before its take-profit
    double pLoseShort = 0.0;  // short's stop-loss struck before its take-profit
    double expiryRetLong = 0.0;   // mean final move of paths that hit neither long barrier
    double expiryRetShort = 0.0;  // mean final move of paths that hit neither short barrier
};

// The scalar knobs of one Monte-Carlo run, as one named bundle (the series
// travels separately — it is the data, these are the question). Designated
// initializers keep call sites self-describing:
//   monteCarlo(closes, {.price = p, .horizon = 24, .tpFrac = t, .slFrac = s,
//                       .paths = 6000});
struct McParams {
    double price = 0.0;   // price the paths start from (must be > 0)
    qint32 horizon = 0;   // bars to simulate (must be > 0)
    double tpFrac = 0.0;  // take-profit barrier as a fraction of price (0 = no barriers)
    double slFrac = 0.0;  // stop-loss barrier as a fraction of price (0 = no barriers)
    qint32 paths = 0;     // simulated paths (must be > 0)
    // Nonzero makes the resampling deterministic (tests); 0 = securely seeded,
    // a fresh sequence every call (QRandomGenerator::global() cannot be
    // reseeded, so determinism needs an owned generator — same pattern as
    // SimulationEngine::seedRng).
    quint32 seed = 0;
};

[[nodiscard]] McOutlook monteCarlo(const QList<double> &series, const McParams &params);

// Logistic squash of x into (0, 1).
[[nodiscard]] double sigmoid(double x) noexcept;

} // namespace trading

#endif // TRADINGAPP_DOMAIN_FORECASTING_H
