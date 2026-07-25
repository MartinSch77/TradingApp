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

Regression linRegForecast(const QList<double> &values, qsizetype n);

// --- k-Nearest-Neighbors analog forecast ------------------------------------
// Finds the k historical return-windows most similar to the current one and
// predicts the next move as the average of what followed those analogs.
struct Knn {
    double retPct = 0.0;  // predicted next-bar return, percent
    double agree = 0.0;   // fraction of neighbours agreeing on direction
    qint32 k = 0;
};

Knn knnForecast(const QList<double> &values, qsizetype window, qsizetype k);

// Hurst exponent via rescaled-range (R/S) over the last returns. ~0.5 = random
// walk, >0.5 = trending/persistent, <0.5 = mean-reverting. A regime hint.
double hurstExponent(const QList<double> &series);

// Non-parametric Monte-Carlo outlook: bootstrap future paths by resampling the
// series' own recent per-bar returns (captures its real distribution, fat tails
// and all). Reports P(price higher) and a 5–95% range after `horizon` bars, plus
// the probability that a long / short reaches its take-profit before its
// stop-loss (tpFrac / slFrac are the barrier distances as fractions of price)
// and the mirror probability of the stop being struck first. Paths that reach
// NEITHER barrier within the horizon count towards neither figure — the
// remainder 1 − pWin − pLose is the chance the trade simply expires between
// the barriers.
struct McOutlook {
    bool valid = false;
    double pUp = 0.5;
    double p5 = 0.0;
    double p95 = 0.0;
    double pWinLong = 0.0;
    double pWinShort = 0.0;
    double pLoseLong = 0.0;   // long's stop-loss struck before its take-profit
    double pLoseShort = 0.0;  // short's stop-loss struck before its take-profit
};

McOutlook monteCarlo(const QList<double> &series, double price, qint32 horizon,
                     double tpFrac, double slFrac, qint32 paths);

// Logistic squash of x into (0, 1).
double sigmoid(double x);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_FORECASTING_H
