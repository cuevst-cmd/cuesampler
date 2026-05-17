#pragma once

#include <vector>

namespace cuesampler
{

// Per-marker warp anchor. Lives in source-sample coordinates (matching
// ChopDefinition::startSample) and clip-local playback time (relative to the
// chop's start, in seconds).
struct ChopWarpMarker
{
    int    sourceSample      = 0;
    double localTimeSeconds  = 0.0;
    bool   snappedToGrid     = false;
    double gridFingerprint   = 0.0;

    bool operator== (const ChopWarpMarker& other) const noexcept
    {
        return sourceSample == other.sourceSample
            && localTimeSeconds == other.localTimeSeconds
            && snappedToGrid == other.snappedToGrid
            && gridFingerprint == other.gridFingerprint;
    }

    bool operator!= (const ChopWarpMarker& other) const noexcept
    {
        return !(*this == other);
    }
};

// Piecewise-linear time map from clip-local playback time to source-buffer
// sample (and back). Built from the chop's bounds and its warp markers; the
// chop bounds become implicit endpoint nodes that pin clip duration.
//
// The map filters invalid markers defensively at build time:
//   - markers outside (startSample, endSample)
//   - markers whose localTimeSeconds is non-monotonic
//   - segments whose stretch ratio is outside [kMinSpeed, kMaxSpeed]
// Filtering never aborts the build; in the worst case the result is an
// identity map (no internal nodes) and isIdentity() reports true.
class WarpMap
{
public:
    struct Node
    {
        double sourceSample;     // fractional; integer at endpoints
        double localTimeSeconds;
    };

    // Stretch ratio = sourceDurationSec / localDurationSec.
    // ratio > 1.0 = playback faster than source (audio sped up)
    // ratio < 1.0 = playback slower than source (audio stretched)
    static constexpr double kMinSpeed = 0.25;
    static constexpr double kMaxSpeed = 4.0;

    WarpMap() = default;

    // Build the node list from chop bounds + markers. Always succeeds.
    // sampleRate must be > 0 for the map to be considered valid; with a
    // non-positive sample rate the map collapses to identity at zero duration.
    void build (int chopStartSample,
                int chopEndSample,
                const std::vector<ChopWarpMarker>& markers,
                double sampleRate) noexcept;

    // Forward: clip-local playback time → fractional source sample.
    // Out-of-range times are clamped to the closest endpoint.
    double sourceSampleAtLocalTime (double localTimeSec) const noexcept;

    // Inverse: fractional source sample → clip-local playback time.
    // Out-of-range samples are clamped to the closest endpoint.
    double localTimeAtSourceSample (double sourceSample) const noexcept;

    double totalLocalDurationSeconds() const noexcept { return totalLocal; }
    double sampleRate() const noexcept                { return sr; }
    bool   isIdentity() const noexcept                { return nodes.size() <= 2; }
    int    numInternalMarkers() const noexcept        { return (int) nodes.size() - 2; }
    int    numDroppedMarkers() const noexcept         { return droppedMarkerCount; }

    const std::vector<Node>& getNodes() const noexcept { return nodes; }

private:
    std::vector<Node> nodes;
    double            sr                  = 0.0;
    double            totalLocal          = 0.0;
    int               droppedMarkerCount  = 0;
};

// Number of output frames a fully-rendered warped chop will produce, given a
// built WarpMap. Returns 0 if the map has no duration or no sample rate.
int warpedChopFrameCount (const WarpMap& warpMap) noexcept;

// Renders a warped chop into preallocated planar float buffers using linear
// interpolation between source samples. JUCE-independent.
//
//   warpMap          Built map describing the time mapping for this chop.
//   sourceData       Planar source channels: sourceData[ch][frame].
//   sourceFrames     Total length of each source channel buffer.
//   channelCount     Number of channels (>= 1). Common to source and target.
//   targetData       Planar target channels: targetData[ch][frame].
//   targetFrames     Length of each target channel buffer; should equal
//                    warpedChopFrameCount(warpMap). If smaller, render is
//                    truncated; if larger, the extra frames are silenced.
//
// Identity warps produce bit-exact copies of the source range
// [chopStart, chopEnd) when source positions land on integer samples.
void renderWarpedChopLinear (const WarpMap&     warpMap,
                             const float* const* sourceData,
                             int                 sourceFrames,
                             int                 channelCount,
                             float* const*       targetData,
                             int                 targetFrames) noexcept;

} // namespace cuesampler
