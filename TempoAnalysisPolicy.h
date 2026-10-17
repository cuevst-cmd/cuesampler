#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cuesampler
{
inline bool temposAgreeAtDifferentOctaves (double firstBpm, double secondBpm,
                                           double tolerance = 0.02) noexcept
{
    if (! std::isfinite (firstBpm) || ! std::isfinite (secondBpm)
        || firstBpm <= 0.0 || secondBpm <= 0.0)
        return false;
    const double ratio = std::max (firstBpm, secondBpm) / std::min (firstBpm, secondBpm);
    return std::abs (ratio - 2.0) / 2.0 <= tolerance;
}

// These decisions run only in the offline analysis job. A confidence value is
// evidence, not certainty: only a strong, steady classical grid skips the model.
template <typename Analysis>
bool canUseClassicalTempoWithoutNeuralCheck (const Analysis& analysis) noexcept
{
    if (! std::isfinite (analysis.confidence) || analysis.confidence < 0.75f
        || analysis.likelyDrifting || ! std::isfinite (analysis.beatPeriodSeconds)
        || analysis.beatPeriodSeconds <= 0.0 || analysis.beatPositionsSeconds.size() < 16)
        return false;

    const auto& beats = analysis.beatPositionsSeconds;
    if (beats.back() - beats.front() < 6.0)
        return false;
    for (size_t i = 1; i < beats.size(); ++i)
    {
        const auto relativeInterval = (beats[i] - beats[i - 1]) / analysis.beatPeriodSeconds;
        if (! std::isfinite (relativeInterval) || std::abs (relativeInterval - 1.0) > 0.025)
            return false;
    }
    return true;
}

template <typename Analysis>
double tempoAnalysisEvidenceScore (const Analysis& analysis) noexcept
{
    if (! std::isfinite (analysis.estimatedBpm) || analysis.estimatedBpm <= 0.0
        || ! std::isfinite (analysis.confidence)
        || analysis.beatPositionsSeconds.size() < 2)
        return 0.0;

    const auto& beats = analysis.beatPositionsSeconds;
    const auto duration = beats.back() - beats.front();
    if (! std::isfinite (duration) || duration <= 0.0)
        return 0.0;

    // Evidence duration is independent of tempo level. Counting beats/bars
    // rewards double-time, and penalising drift rejects valid live timing.
    return std::clamp ((double) analysis.confidence, 0.0, 1.0) * 0.85
         + std::clamp (duration / 12.0, 0.0, 1.0) * 0.15;
}
}
