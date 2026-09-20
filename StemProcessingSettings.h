#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdlib>

namespace cuesampler
{
// Shared by inference and cache identity: tuning overlap must never reuse an
// entry produced with different boundary processing. Older saved keys still load.
struct StemProcessingSettings
{
    static constexpr int segmentSamples = 343980;
    static constexpr double defaultOverlap = 0.10;
    double overlap = defaultOverlap;
    int requestedSegmentSamples = segmentSamples;

    static StemProcessingSettings fromEnvironment()
    {
        StemProcessingSettings settings;
        if (const char* value = std::getenv ("CUE_STEM_OVERLAP"))
        {
            char* end = nullptr;
            const double parsed = std::strtod (value, &end);
            if (end != value && *end == '\0' && std::isfinite (parsed) && parsed >= 0.0)
                settings.overlap = juce::jlimit (0.0, 0.5, parsed);
        }
        if (const char* value = std::getenv ("CUE_STEM_SEGMENT"))
        {
            char* end = nullptr;
            const double parsed = std::strtod (value, &end);
            if (end != value && *end == '\0' && std::isfinite (parsed) && parsed >= 44100.0)
                settings.requestedSegmentSamples = (int) juce::jlimit (44100.0, (double) segmentSamples, parsed);
        }
        return settings;
    }

    juce::String cacheTag() const
    {
        return "sinc-v4|overlap=" + juce::String (overlap, 9)
            + "|segment=" + juce::String (requestedSegmentSamples);
    }

    // Stop once the last window covers the source, rather than running another
    // inference on an already-covered tail simply because its start is in range.
    static int windowCount (int length, int segment, int stride) noexcept
    {
        return length <= 0 ? 0 : 1 + (int) (((int64_t) juce::jmax (0, length - segment)
                                           + stride - 1) / stride);
    }
};
}
