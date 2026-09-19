#pragma once

#include "WarpMap.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cuesampler
{

// Per-chop cache of warped audio buffers. Each entry stores a fully rendered
// AudioBuffer<float> (the chop's audio with warp baked in) plus the generation
// number that produced it. The audio thread reads via get(); the render
// pipeline writes via store(). Stale generations are silently rejected.
//
class ChopAudioCache
{
public:
    struct Entry
    {
        int chopId = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<const juce::AudioBuffer<float>> warpedBuffer;
        // True for entries that represent identity warps (no markers). The
        // audio thread can still use them; this flag lets the playback path
        // know it can fast-path back to reading the source buffer if needed.
        bool isIdentity = false;
        bool usedFallback = false;
    };

    struct PreparedKey
    {
        int chopStartSample = 0;
        int chopEndSample = 0;
        int cueOffsetSamples = 0;
        int sourceSampleRate = 0;
        int outputSampleRate = 0;
        int pitchCents = 0;
        int stretchPpm = 100000;
        std::uint64_t warpHash = 0;

        bool operator== (const PreparedKey& other) const noexcept
        {
            return chopStartSample == other.chopStartSample
                && chopEndSample == other.chopEndSample
                && cueOffsetSamples == other.cueOffsetSamples
                && sourceSampleRate == other.sourceSampleRate
                && outputSampleRate == other.outputSampleRate
                && pitchCents == other.pitchCents
                && stretchPpm == other.stretchPpm
                && warpHash == other.warpHash;
        }

        bool operator!= (const PreparedKey& other) const noexcept
        {
            return ! (*this == other);
        }
    };

    struct PreparedEntry
    {
        int chopId = 0;
        std::uint64_t generation = 0;
        PreparedKey key;
        std::shared_ptr<const juce::AudioBuffer<float>> buffer;
        int cueFrame = 0;
        bool renderedWithBungee = false;
        bool usedFallback = false;
    };

    ChopAudioCache() = default;

    // Look up the latest entry for a chop. Returns nullptr if not present.
    std::shared_ptr<const Entry> get (int chopId) const noexcept;

    // Look up the currently prepared playback variant for a chop. The key must
    // match exactly so stale pitch/time renders are never used by the audio
    // thread.
    std::shared_ptr<const PreparedEntry> getPrepared (int chopId,
                                                      const PreparedKey& key) const noexcept;

    // Store/replace an entry. If the cached entry's generation is greater
    // than newEntry->generation, the new entry is discarded.
    void store (std::shared_ptr<const Entry> newEntry);
    void storePrepared (std::shared_ptr<const PreparedEntry> newEntry);

    // Drop a single chop's cached entry (e.g., chop deleted).
    void evict (int chopId);
    void evictPrepared (int chopId);

    // Drop everything (e.g., new sample loaded).
    void clear();
    void clearPrepared();

    // Non-realtime render through Bungee. Exports disable the interpolation
    // fallback so a failed pitch-preserving render cannot silently change pitch.
    // Generation is provided by playback-cache callers; exports never publish.
    static std::shared_ptr<Entry> renderChopSync (
        const juce::AudioBuffer<float>& source,
        double sampleRate,
        int chopId,
        int chopStartSample,
        int chopEndSample,
        const std::vector<ChopWarpMarker>& markers,
        std::uint64_t generation,
        bool allowFallback = true);

    static PreparedKey makePreparedKey (
        int chopStartSample,
        int chopEndSample,
        int cueOffsetSamples,
        const std::vector<ChopWarpMarker>& markers,
        double sourceSampleRate,
        double outputSampleRate,
        float pitchSemitones,
        float stretchRatio) noexcept;

    static std::shared_ptr<PreparedEntry> renderPreparedChopSync (
        const juce::AudioBuffer<float>& source,
        double sourceSampleRate,
        double outputSampleRate,
        int chopId,
        int chopStartSample,
        int chopEndSample,
        int cueOffsetSamples,
        const std::vector<ChopWarpMarker>& markers,
        float pitchSemitones,
        float stretchRatio,
        std::uint64_t generation,
        bool allowFallback = true);

private:
    struct Snapshot
    {
        std::unordered_map<int, std::shared_ptr<const Entry>> entries;
        std::unordered_map<int, std::shared_ptr<const PreparedEntry>> preparedEntries;
    };

    mutable std::mutex writerMutex;
    std::shared_ptr<const Snapshot> snapshot { std::make_shared<Snapshot>() };
};

} // namespace cuesampler
