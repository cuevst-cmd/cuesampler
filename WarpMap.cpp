#include "WarpMap.h"

#include <algorithm>
#include <cmath>

namespace cuesampler
{

namespace
{
constexpr double kEpsilon = 1.0e-9;
}

void WarpMap::build (int chopStartSample,
                     int chopEndSample,
                     const std::vector<ChopWarpMarker>& markers,
                     double sampleRate) noexcept
{
    nodes.clear();
    droppedMarkerCount = 0;
    sr = sampleRate;

    if (sampleRate <= 0.0 || chopEndSample <= chopStartSample)
    {
        totalLocal = 0.0;
        return;
    }

    const double startSrc  = (double) chopStartSample;
    const double endSrc    = (double) chopEndSample;
    const double endLocal  = (endSrc - startSrc) / sampleRate;

    totalLocal = endLocal;

    // Reserve up-front: 2 endpoints + at most one node per marker.
    nodes.reserve (markers.size() + 2);

    nodes.push_back ({ startSrc, 0.0 });

    // Markers are expected to be sorted by sourceSample by callers, but
    // we sort defensively here without mutating the caller's vector.
    std::vector<ChopWarpMarker> sorted = markers;
    std::sort (sorted.begin(), sorted.end(),
               [] (const ChopWarpMarker& a, const ChopWarpMarker& b)
               {
                   return a.sourceSample < b.sourceSample;
               });

    auto segmentSpeedValid = [] (double srcDuration, double localDuration)
    {
        if (srcDuration <= kEpsilon || localDuration <= kEpsilon)
            return false;
        const double speed = srcDuration / localDuration;
        return speed >= kMinSpeed - kEpsilon && speed <= kMaxSpeed + kEpsilon;
    };

    for (const auto& m : sorted)
    {
        const double mSrc   = (double) m.sourceSample;
        const double mLocal = m.localTimeSeconds;

        // Reject markers outside the open interval (startSrc, endSrc).
        if (mSrc <= startSrc + kEpsilon || mSrc >= endSrc - kEpsilon)
        {
            ++droppedMarkerCount;
            continue;
        }

        // Reject markers outside the open interval (0, endLocal).
        if (mLocal <= 0.0 + kEpsilon || mLocal >= endLocal - kEpsilon)
        {
            ++droppedMarkerCount;
            continue;
        }

        const auto& prev = nodes.back();

        // Strict monotonicity vs. the previously accepted node.
        if (mSrc <= prev.sourceSample + kEpsilon
            || mLocal <= prev.localTimeSeconds + kEpsilon)
        {
            ++droppedMarkerCount;
            continue;
        }

        // Speed clamp on the segment we'd be creating.
        const double srcDurationSec  = (mSrc - prev.sourceSample) / sampleRate;
        const double localDurationSec = mLocal - prev.localTimeSeconds;
        if (! segmentSpeedValid (srcDurationSec, localDurationSec))
        {
            ++droppedMarkerCount;
            continue;
        }

        nodes.push_back ({ mSrc, mLocal });
    }

    // Final segment from the last accepted node to the end endpoint must
    // also satisfy the speed clamp; if not, peel back markers until it does
    // (or we collapse to identity).
    while (nodes.size() > 1)
    {
        const auto& prev = nodes.back();
        const double srcDurationSec  = (endSrc - prev.sourceSample) / sampleRate;
        const double localDurationSec = endLocal - prev.localTimeSeconds;
        if (segmentSpeedValid (srcDurationSec, localDurationSec))
            break;

        nodes.pop_back();
        ++droppedMarkerCount;
    }

    nodes.push_back ({ endSrc, endLocal });
}

double WarpMap::sourceSampleAtLocalTime (double localTimeSec) const noexcept
{
    if (nodes.empty())
        return 0.0;

    if (localTimeSec <= nodes.front().localTimeSeconds)
        return nodes.front().sourceSample;
    if (localTimeSec >= nodes.back().localTimeSeconds)
        return nodes.back().sourceSample;

    // Binary search for the segment whose local-time range contains the query.
    auto upper = std::upper_bound (nodes.begin(), nodes.end(), localTimeSec,
                                   [] (double t, const Node& n)
                                   {
                                       return t < n.localTimeSeconds;
                                   });
    auto hi = upper;
    auto lo = upper - 1;

    const double localSpan = hi->localTimeSeconds - lo->localTimeSeconds;
    if (localSpan <= kEpsilon)
        return lo->sourceSample;

    const double t = (localTimeSec - lo->localTimeSeconds) / localSpan;
    return lo->sourceSample + t * (hi->sourceSample - lo->sourceSample);
}

int warpedChopFrameCount (const WarpMap& warpMap) noexcept
{
    const double sr = warpMap.sampleRate();
    if (sr <= 0.0)
        return 0;

    const double frames = warpMap.totalLocalDurationSeconds() * sr;
    if (frames <= 0.0)
        return 0;

    return (int) std::llround (frames);
}

void renderWarpedChopLinear (const WarpMap&     warpMap,
                             const float* const* sourceData,
                             int                 sourceFrames,
                             int                 channelCount,
                             float* const*       targetData,
                             int                 targetFrames) noexcept
{
    if (targetData == nullptr || targetFrames <= 0 || channelCount <= 0)
        return;

    // Zero the target up front so any early-exit paths leave a clean buffer.
    for (int ch = 0; ch < channelCount; ++ch)
        if (targetData[ch] != nullptr)
            std::fill (targetData[ch], targetData[ch] + targetFrames, 0.0f);

    if (sourceData == nullptr || sourceFrames <= 0)
        return;

    const double sr = warpMap.sampleRate();
    if (sr <= 0.0)
        return;

    const int producedFrames = std::min (targetFrames, warpedChopFrameCount (warpMap));
    if (producedFrames <= 0)
        return;

    const double maxSourceIndex = (double) (sourceFrames - 1);

    for (int frame = 0; frame < producedFrames; ++frame)
    {
        const double localTimeSec = (double) frame / sr;
        double srcPos = warpMap.sourceSampleAtLocalTime (localTimeSec);

        // Clamp into the legal source range to keep linear interp safe.
        if (srcPos < 0.0)
            srcPos = 0.0;
        else if (srcPos > maxSourceIndex)
            srcPos = maxSourceIndex;

        const int    i0 = (int) srcPos;
        const int    i1 = (i0 + 1 < sourceFrames) ? (i0 + 1) : i0;
        const double frac = srcPos - (double) i0;

        for (int ch = 0; ch < channelCount; ++ch)
        {
            if (sourceData[ch] == nullptr || targetData[ch] == nullptr)
                continue;

            const float a = sourceData[ch][i0];
            const float b = sourceData[ch][i1];
            targetData[ch][frame] = (float) (a + (b - a) * frac);
        }
    }
}

double WarpMap::localTimeAtSourceSample (double sourceSample) const noexcept
{
    if (nodes.empty())
        return 0.0;

    if (sourceSample <= nodes.front().sourceSample)
        return nodes.front().localTimeSeconds;
    if (sourceSample >= nodes.back().sourceSample)
        return nodes.back().localTimeSeconds;

    auto upper = std::upper_bound (nodes.begin(), nodes.end(), sourceSample,
                                   [] (double s, const Node& n)
                                   {
                                       return s < n.sourceSample;
                                   });
    auto hi = upper;
    auto lo = upper - 1;

    const double srcSpan = hi->sourceSample - lo->sourceSample;
    if (srcSpan <= kEpsilon)
        return lo->localTimeSeconds;

    const double t = (sourceSample - lo->sourceSample) / srcSpan;
    return lo->localTimeSeconds + t * (hi->localTimeSeconds - lo->localTimeSeconds);
}

} // namespace cuesampler
