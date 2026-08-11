// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/RollingZScore.h"

#include <cmath>
#include <numeric>

namespace trading::crowd {

std::optional<double> zScore(double value, const QList<double> &history, int minSamples)
{
    if (history.size() < minSamples) {
        return std::nullopt;
    }
    const double sum = std::accumulate(history.cbegin(), history.cend(), 0.0);
    const double mean = sum / static_cast<double>(history.size());
    double squares = 0.0;
    for (const double sample : history) {
        const double delta = sample - mean;
        squares += delta * delta;
    }
    // Population standard deviation (÷N), documented: the history IS the population we are placing
    // the new value within, not a sample of a larger one.
    const double stddev = std::sqrt(squares / static_cast<double>(history.size()));
    if (stddev <= 0.0) {
        return std::nullopt;   // a constant history has no meaningful z-score
    }
    return (value - mean) / stddev;
}

RollingZScore::RollingZScore(int window, int minSamples)
    : m_window(window > 0 ? window : 1), m_minSamples(minSamples)
{
}

std::optional<double> RollingZScore::push(double value)
{
    const std::optional<double> result = zScore(value, m_values, m_minSamples);
    m_values.append(value);
    while (m_values.size() > m_window) {
        m_values.removeFirst();
    }
    return result;
}

} // namespace trading::crowd
