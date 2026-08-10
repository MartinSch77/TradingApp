// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_ROLLINGZSCORE_H
#define TRADINGAPP_DOMAIN_ROLLINGZSCORE_H

#include <QList>

#include <optional>

// Rolling normalization for the Crowd Score (REQ-F-040, Phase 2). A component is only comparable
// across families once it is normalized, and the ONLY leakage-safe normalization is one computed
// from PAST values: the value being scored is measured against the history that PRECEDED it,
// never against itself or anything later. That is what these do, and nothing here fabricates a
// number when there is too little history — an under-sampled series returns nothing, the same
// absent-is-not-zero rule the rest of the app follows.
namespace trading::crowd {

// The z-score of `value` against `history`: (value - mean) / population-stddev, computed over the
// history the caller passes (which must PRECEDE `value`). Returns nothing when the history has
// fewer than `minSamples` points, or no spread — a constant series has no meaningful z, and
// inventing one would make an unmoving crowd look like a signal.
[[nodiscard]] std::optional<double> zScore(double value, const QList<double> &history,
                                           int minSamples = 3);

// A rolling z-score over the last `window` values, for a CHRONOLOGICAL pass: push each value in
// time order; push() returns the z of that value against the window that PRECEDED it and only
// THEN adds it (evicting beyond `window`). Same no-look-ahead guarantee as the free function,
// carried as state so a whole series can be normalized in one sweep — which is exactly what
// building leakage-safe training features will need later.
class RollingZScore
{
public:
    explicit RollingZScore(int window, int minSamples = 3);

    [[nodiscard]] std::optional<double> push(double value);
    [[nodiscard]] int size() const { return static_cast<int>(m_values.size()); }
    void clear() { m_values.clear(); }

private:
    int m_window;
    int m_minSamples;
    QList<double> m_values;   // the prior window, oldest first
};

} // namespace trading::crowd

#endif // TRADINGAPP_DOMAIN_ROLLINGZSCORE_H
