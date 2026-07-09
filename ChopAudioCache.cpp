#include "ChopAudioCache.h"

#include <bungee/Stream.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace cuesampler
{

namespace
{
// Block size used by the offline Bungee renderer. Mirrors the constant used by
// the live offline-render path in PluginProcessor.cpp.
constexpr int kBungeeBlockSize = 2048;

std::mutex& bungeeRenderMutex()
{
    static std::mutex mutex;
    return mutex;
}

bool hasReadableChannels (const juce::AudioBuffer<float>& buffer, int channels) noexcept
{
    for (int ch = 0; ch < channels; ++ch)
        if (buffer.getReadPointer (ch) == nullptr)
            return false;

    return true;
}

bool hasWritableChannels (juce::AudioBuffer<float>& buffer, int channels) noexcept
{
    for (int ch = 0; ch < channels; ++ch)
        if (buffer.getWritePointer (ch) == nullptr)
            return false;

    return true;
}

std::uint64_t mixHash (std::uint64_t hash, std::uint64_t value) noexcept
{
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

float readLinear (const float* sourceData, int sourceLength, double position) noexcept
{
    if (sourceData == nullptr || sourceLength <= 0)
        return 0.0f;

    if (position <= 0.0)
        return sourceData[0];

    if (position >= (double) (sourceLength - 1))
        return sourceData[sourceLength - 1];

    const auto i0 = (int) std::floor (position);
    const auto i1 = juce::jmin (i0 + 1, sourceLength - 1);
    const auto frac = position - (double) i0;
    return (float) ((double) sourceData[i0]
                    + ((double) sourceData[i1] - (double) sourceData[i0]) * frac);
}

void sanitiseAndNormalisePreparedBuffer (juce::AudioBuffer<float>& buffer) noexcept
{
    float peak = 0.0f;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        if (data == nullptr)
            continue;

        for (int frame = 0; frame < buffer.getNumSamples(); ++frame)
        {
            auto sample = data[frame];
            if (! std::isfinite (sample))
            {
                sample = 0.0f;
                data[frame] = 0.0f;
            }

            peak = juce::jmax (peak, std::abs (sample));
        }
    }

    if (peak > 1.0f)
        buffer.applyGain (0.98f / peak);
}

bool renderPreparedWithInterpolation (const juce::AudioBuffer<float>& base,
                                      juce::AudioBuffer<float>& prepared,
                                      double sourceFramesPerOutputFrame,
                                      int preRollSourceFrames = 0) noexcept
{
    const int channels = base.getNumChannels();
    const int sourceLength = base.getNumSamples();
    const int targetFrames = prepared.getNumSamples();

    if (channels <= 0 || sourceLength <= 0 || targetFrames <= 0
        || sourceFramesPerOutputFrame <= 0.0
        || prepared.getNumChannels() < channels
        || ! hasReadableChannels (base, channels)
        || ! hasWritableChannels (prepared, channels))
    {
        return false;
    }

    prepared.clear();

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* src = base.getReadPointer (ch);
        auto* dst = prepared.getWritePointer (ch);
        if (src == nullptr || dst == nullptr)
            return false;

        // base[] begins with preRollSourceFrames of pre-chop lead-in; the prepared
        // buffer starts at the chop, so skip the lead-in. (Linear interp is
        // stateless, so there is no startup latency to prime — just an offset.)
        double sourcePosition = (double) preRollSourceFrames;
        for (int frame = 0; frame < targetFrames; ++frame)
        {
            dst[frame] = readLinear (src, sourceLength, sourcePosition);
            sourcePosition += sourceFramesPerOutputFrame;
        }
    }

    sanitiseAndNormalisePreparedBuffer (prepared);
    return true;
}

bool renderPreparedWithBungee (const juce::AudioBuffer<float>& base,
                               juce::AudioBuffer<float>& prepared,
                               double sourceSampleRate,
                               double outputSampleRate,
                               float pitchSemitones,
                               float stretchRatio,
                               int preRollSourceFrames = 0)
{
    const int channels = base.getNumChannels();
    const int sourceLength = base.getNumSamples();
    const int targetFrames = prepared.getNumSamples();

    if (channels <= 0 || sourceLength <= 0 || targetFrames <= 0
        || sourceSampleRate <= 0.0 || outputSampleRate <= 0.0
        || stretchRatio <= 0.0f
        || prepared.getNumChannels() < channels
        || ! hasReadableChannels (base, channels)
        || ! hasWritableChannels (prepared, channels))
    {
        return false;
    }

    std::lock_guard<std::mutex> bungeeLock (bungeeRenderMutex());

    prepared.clear();

    Bungee::SampleRates rates { (int) std::round (sourceSampleRate),
                                (int) std::round (outputSampleRate) };

    Bungee::Stretcher<Bungee::Basic> stretcher (rates, channels, -1);
    const int maxInputFrames = juce::jmax (kBungeeBlockSize,
                                           stretcher.maxInputFrameCount());
    Bungee::Stream<Bungee::Basic> stream (stretcher, maxInputFrames, channels);

    std::vector<const float*> inPtrs ((size_t) channels, nullptr);
    std::vector<float*> outPtrs ((size_t) channels, nullptr);
    juce::AudioBuffer<float> inputBuffer (channels, maxInputFrames);
    juce::AudioBuffer<float> outputChunk (channels, kBungeeBlockSize + 16);

    const auto sourceFramesPerOutputFrame =
        (sourceSampleRate / outputSampleRate) / (double) stretchRatio;
    const auto pitchFactor = std::pow (2.0, (double) pitchSemitones / 12.0);

    // base[] starts with preRollSourceFrames of pre-chop lead-in. We render the
    // matching pre-roll OUTPUT first and discard it: that warms the stretcher's
    // look-ahead so the captured chop window begins at full amplitude instead of
    // Bungee's startup silence/ramp. The capture window in output-frame space is
    // [preRollOutputFrames, preRollOutputFrames + targetFrames).
    const int preRollOutputFrames = juce::jmax (0,
        (int) std::llround ((double) preRollSourceFrames / sourceFramesPerOutputFrame));
    const int totalOutputWanted = preRollOutputFrames + targetFrames;

    int outputRendered = 0; // total output produced so far, including discarded pre-roll
    int zeroRenderStreak = 0;
    double sourcePosition = 0.0;

    while (outputRendered < totalOutputWanted)
    {
        const int segmentOutputFrames =
            juce::jmin (kBungeeBlockSize, totalOutputWanted - outputRendered);
        const int inputFramesRequested = juce::jlimit (
            1, maxInputFrames,
            (int) std::ceil ((double) segmentOutputFrames * sourceFramesPerOutputFrame));
        const int sourceStart = juce::jlimit (0, juce::jmax (0, sourceLength - 1),
                                              (int) std::floor (sourcePosition));
        const int availableFrames = juce::jmax (0, sourceLength - sourceStart);
        const int copiedFrames = juce::jmin (availableFrames, inputFramesRequested);

        outputChunk.clear();

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* input = inputBuffer.getWritePointer (ch);
            auto* output = outputChunk.getWritePointer (ch);
            if (input == nullptr || output == nullptr)
                return false;

            std::fill (input, input + inputFramesRequested, 0.0f);
            if (copiedFrames > 0)
            {
                const auto* sourceData = base.getReadPointer (ch, sourceStart);
                if (sourceData == nullptr)
                    return false;
                std::copy (sourceData, sourceData + copiedFrames, input);
            }

            inPtrs[(size_t) ch] = input;
            outPtrs[(size_t) ch] = output;
        }

        const int renderedFrames = stream.process (inPtrs.data(),
                                                   outPtrs.data(),
                                                   inputFramesRequested,
                                                   (double) segmentOutputFrames,
                                                   pitchFactor);

        if (renderedFrames > 0)
        {
            // Keep only the part of this output block inside the capture window;
            // everything before preRollOutputFrames is primed-away startup latency.
            const int winStart = juce::jmax (outputRendered, preRollOutputFrames);
            const int winEnd   = juce::jmin (outputRendered + renderedFrames, totalOutputWanted);
            if (winEnd > winStart)
            {
                const int copyLen   = winEnd - winStart;
                const int srcOffset = winStart - outputRendered;       // into outputChunk
                const int dstOffset = winStart - preRollOutputFrames;  // into prepared
                for (int ch = 0; ch < channels; ++ch)
                {
                    const auto* src = outputChunk.getReadPointer (ch);
                    auto* dst = prepared.getWritePointer (ch, dstOffset);
                    if (src == nullptr || dst == nullptr)
                        return false;
                    std::copy (src + srcOffset, src + srcOffset + copyLen, dst);
                }
            }

            outputRendered += renderedFrames;
            zeroRenderStreak = 0;
        }
        else
        {
            ++zeroRenderStreak;
        }

        sourcePosition += (double) inputFramesRequested;

        if (sourcePosition >= (double) sourceLength)
            break;

        if (zeroRenderStreak >= 8)
            return false;
    }

    // Need at least some real (post-pre-roll) chop output to count as a success.
    if (outputRendered <= preRollOutputFrames)
        return false;

    sanitiseAndNormalisePreparedBuffer (prepared);
    return true;
}

// Per-segment Bungee-driven render. Walks WarpMap segments, feeds source
// samples at each segment's speed ratio, and writes pitch-preserved output
// into the target buffer. Returns true on success; false if rendering aborted
// (e.g., Bungee produced no frames for many consecutive process calls).
//
// pitchFactor is fixed at 1.0 because per-chop pitchSemitones is applied
// live on top of the cache, not baked into it.
bool renderWarpedChopBungee (const WarpMap&                 warpMap,
                                              const juce::AudioBuffer<float>& source,
                                              juce::AudioBuffer<float>&       target,
                                              double                          sampleRate)
{
    const int channels      = source.getNumChannels();
    const int sourceLength  = source.getNumSamples();
    const int targetFrames  = target.getNumSamples();

    if (channels <= 0 || sourceLength <= 0 || targetFrames <= 0 || sampleRate <= 0.0)
        return false;

    if (target.getNumChannels() < channels
        || ! hasReadableChannels (source, channels)
        || ! hasWritableChannels (target, channels))
    {
        return false;
    }

    const auto& nodes = warpMap.getNodes();
    if (nodes.size() < 2)
        return false;

    // Bungee rendering runs only on cache/export workers. Serialising these
    // renders avoids concurrent access to Bungee internals while keeping the
    // audio thread on the pre-rendered cache/fallback path.
    std::lock_guard<std::mutex> bungeeLock (bungeeRenderMutex());

    target.clear();

    Bungee::SampleRates rates { (int) std::round (sampleRate),
                                (int) std::round (sampleRate) };

    Bungee::Stretcher<Bungee::Basic> stretcher (rates, channels, -1);
    const int maxProcessInputFrames = juce::jmax (kBungeeBlockSize,
                                                  stretcher.maxInputFrameCount());
    Bungee::Stream<Bungee::Basic>    stream    (stretcher, maxProcessInputFrames, channels);

    const float pitchFactor = 1.0f;

    std::vector<const float*> inPtrs  ((size_t) channels, nullptr);
    std::vector<float*>       outPtrs ((size_t) channels, nullptr);

    // Output side capacity must cover the worst-case Bungee output for any
    // segment at the lower speed clamp (kMinSpeed = 0.25 → 4x the input).
    // Without this, Bungee can write past the chunk/discard buffers during
    // preroll on segments with extreme stretch and corrupt adjacent heap.
    const int maxStretchOutputFrames = (int) std::ceil ((double) kBungeeBlockSize
                                                        / WarpMap::kMinSpeed) + 16;

    juce::AudioBuffer<float> chunkBuffer (channels,
                                          juce::jmax (kBungeeBlockSize, maxStretchOutputFrames));
    juce::AudioBuffer<float> inputBuffer (channels, maxProcessInputFrames);
    inputBuffer.clear();

    // ---- Preroll ---------------------------------------------------------
    // Feed up to maxInputFrameCount samples from before the first node so
    // Bungee's grain history is populated before the first real output frame.
    // Use the first segment's speed for the preroll.
    const auto& firstA = nodes[0];
    const auto& firstB = nodes[1];
    const double firstLocalDur = firstB.localTimeSeconds - firstA.localTimeSeconds;
    const double firstSourceDurFrames = firstB.sourceSample - firstA.sourceSample;
    if (firstLocalDur <= 0.0 || firstSourceDurFrames <= 0.0)
        return false;
    const double firstSpeed = firstSourceDurFrames / (firstLocalDur * sampleRate);

    const int firstSrc        = (int) std::round (firstA.sourceSample);
    const int maxPreroll      = stretcher.maxInputFrameCount();
    const int prerollFrames   = juce::jmin (firstSrc, maxPreroll);

    if (prerollFrames > 0)
    {
        // Discard buffer must accommodate Bungee's worst-case output for any
        // input chunk at the smallest allowed speed. See note above.
        juce::AudioBuffer<float> discard (channels, maxStretchOutputFrames);
        int prerollRead = firstSrc - prerollFrames;

        while (prerollRead < firstSrc)
        {
            const int inFrames = juce::jmin (kBungeeBlockSize, firstSrc - prerollRead);
            // Cap the output request so it cannot exceed the discard buffer
            // even for pathological speed ratios that slipped past clamps.
            const double rawOut = std::ceil ((double) inFrames / firstSpeed);
            const double outFrames = juce::jlimit (1.0,
                                                    (double) maxStretchOutputFrames,
                                                    juce::jmax (1.0, rawOut));
            discard.clear();

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* srcChan = source.getReadPointer (ch);
                if (srcChan == nullptr)
                    return false;
                auto* discardOut = discard.getWritePointer (ch);
                if (discardOut == nullptr)
                    return false;
                inPtrs[(size_t) ch]  = srcChan + prerollRead;
                outPtrs[(size_t) ch] = discardOut;
            }

            stream.process (inPtrs.data(), outPtrs.data(),
                            inFrames, outFrames, pitchFactor);
            prerollRead += inFrames;
        }
    }

    // ---- Per-segment render ---------------------------------------------
    int outputWriteOffset = 0;

    for (size_t segIdx = 0; segIdx + 1 < nodes.size(); ++segIdx)
    {
        const auto& a = nodes[segIdx];
        const auto& b = nodes[segIdx + 1];

        const int segOutStart = (int) std::llround (a.localTimeSeconds * sampleRate);
        const int segOutEnd   = (int) std::llround (b.localTimeSeconds * sampleRate);
        const int segOutFrames = segOutEnd - segOutStart;
        if (segOutFrames <= 0)
            continue;

        const double segLocalDur = b.localTimeSeconds - a.localTimeSeconds;
        const double segSourceDurFrames = b.sourceSample - a.sourceSample;
        if (segLocalDur <= 0.0 || segSourceDurFrames <= 0.0)
            continue;

        const double segSpeed = segSourceDurFrames / (segLocalDur * sampleRate);

        double sourcePosition = a.sourceSample;
        const double segStopSource = b.sourceSample;

        int segWritten        = 0;
        int zeroRenderStreak  = 0;

        while (segWritten < segOutFrames)
        {
            const int remaining = segOutFrames - segWritten;
            const int blockOutFrames = juce::jmin (kBungeeBlockSize, remaining);

            const int inputFramesRequested = juce::jlimit (
                1,
                juce::jmax (kBungeeBlockSize, stretcher.maxInputFrameCount()),
                (int) std::ceil ((double) blockOutFrames * segSpeed));

            const int sourceStart = juce::jlimit (0,
                                                  juce::jmax (0, sourceLength - 1),
                                                  (int) std::floor (sourcePosition));
            const int availableFrames = juce::jmax (0, sourceLength - sourceStart);
            const int copiedFrames    = juce::jmin (availableFrames, inputFramesRequested);

            chunkBuffer.clear();

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* tempInput = inputBuffer.getWritePointer (ch);
                if (tempInput == nullptr)
                    return false;
                std::fill (tempInput, tempInput + inputFramesRequested, 0.0f);
                if (copiedFrames > 0)
                {
                    const auto* srcChan = source.getReadPointer (ch);
                    if (srcChan == nullptr)
                        return false;
                    const auto* srcData = srcChan + sourceStart;
                    std::copy (srcData, srcData + copiedFrames, tempInput);
                }

                inPtrs[(size_t) ch]  = tempInput;
                auto* chunkOut = chunkBuffer.getWritePointer (ch);
                if (chunkOut == nullptr)
                    return false;
                outPtrs[(size_t) ch] = chunkOut;
            }

            const int rendered = stream.process (inPtrs.data(), outPtrs.data(),
                                                 inputFramesRequested,
                                                 (double) blockOutFrames,
                                                 pitchFactor);

            if (rendered > 0)
            {
                const int remainingTargetFrames = targetFrames - outputWriteOffset;
                if (remainingTargetFrames <= 0)
                    return true;

                const int writable = juce::jmin (rendered,
                                                 juce::jmin (chunkBuffer.getNumSamples(),
                                                             remainingTargetFrames));
                if (writable > 0)
                {
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        const auto* src = chunkBuffer.getReadPointer (ch);
                        auto* dstChan = target.getWritePointer (ch);
                        if (src == nullptr || dstChan == nullptr)
                            return false;
                        auto* dst = dstChan + outputWriteOffset;
                        std::copy (src, src + writable, dst);
                    }
                    outputWriteOffset += writable;
                    segWritten        += writable;
                }
                zeroRenderStreak = 0;
            }
            else
            {
                ++zeroRenderStreak;
            }

            sourcePosition += (double) inputFramesRequested;

            if (sourcePosition >= segStopSource)
                break;

            if (zeroRenderStreak >= 8)
                return false;

            if (outputWriteOffset >= targetFrames)
                return true;
        }
    }

    return outputWriteOffset > 0;
}
} // namespace

std::shared_ptr<const ChopAudioCache::Entry> ChopAudioCache::get (int chopId) const noexcept
{
    const auto current = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
    if (current == nullptr)
        return nullptr;

    const auto it = current->entries.find (chopId);
    if (it == current->entries.end())
        return nullptr;

    return it->second;
}

std::shared_ptr<const ChopAudioCache::PreparedEntry>
ChopAudioCache::getPrepared (int chopId, const PreparedKey& key) const noexcept
{
    const auto current = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
    if (current == nullptr)
        return nullptr;

    const auto it = current->preparedEntries.find (chopId);
    if (it == current->preparedEntries.end() || it->second == nullptr)
        return nullptr;

    if (it->second->key != key)
        return nullptr;

    return it->second;
}

void ChopAudioCache::store (std::shared_ptr<const Entry> newEntry)
{
    if (newEntry == nullptr)
        return;

    std::lock_guard<std::mutex> lock (writerMutex);

    const auto current = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
    auto next = std::make_shared<Snapshot> (current != nullptr ? *current : Snapshot {});

    const auto it = next->entries.find (newEntry->chopId);
    if (it != next->entries.end() && it->second != nullptr
        && it->second->generation > newEntry->generation)
    {
        // Stale render; the cache already has a newer one.
        return;
    }

    next->entries[newEntry->chopId] = std::move (newEntry);
    std::atomic_store_explicit (&snapshot,
                                std::static_pointer_cast<const Snapshot> (next),
                                std::memory_order_release);
}

void ChopAudioCache::storePrepared (std::shared_ptr<const PreparedEntry> newEntry)
{
    if (newEntry == nullptr)
        return;

    std::lock_guard<std::mutex> lock (writerMutex);

    const auto current = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
    auto next = std::make_shared<Snapshot> (current != nullptr ? *current : Snapshot {});

    const auto it = next->preparedEntries.find (newEntry->chopId);
    if (it != next->preparedEntries.end() && it->second != nullptr
        && it->second->generation > newEntry->generation)
    {
        return;
    }

    next->preparedEntries[newEntry->chopId] = std::move (newEntry);
    std::atomic_store_explicit (&snapshot,
                                std::static_pointer_cast<const Snapshot> (next),
                                std::memory_order_release);
}

void ChopAudioCache::evict (int chopId)
{
    std::lock_guard<std::mutex> lock (writerMutex);

    const auto current = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
    if (current == nullptr)
        return;

    const bool hasWarpEntry = current->entries.find (chopId) != current->entries.end();
    const bool hasPreparedEntry = current->preparedEntries.find (chopId) != current->preparedEntries.end();
    if (! hasWarpEntry && ! hasPreparedEntry)
        return;

    auto next = std::make_shared<Snapshot> (*current);
    next->entries.erase (chopId);
    next->preparedEntries.erase (chopId);
    std::atomic_store_explicit (&snapshot,
                                std::static_pointer_cast<const Snapshot> (next),
                                std::memory_order_release);
}

void ChopAudioCache::evictPrepared (int chopId)
{
    std::lock_guard<std::mutex> lock (writerMutex);

    const auto current = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
    if (current == nullptr || current->preparedEntries.find (chopId) == current->preparedEntries.end())
        return;

    auto next = std::make_shared<Snapshot> (*current);
    next->preparedEntries.erase (chopId);
    std::atomic_store_explicit (&snapshot,
                                std::static_pointer_cast<const Snapshot> (next),
                                std::memory_order_release);
}

void ChopAudioCache::clear()
{
    std::lock_guard<std::mutex> lock (writerMutex);
    std::atomic_store_explicit (&snapshot,
                                std::static_pointer_cast<const Snapshot> (std::make_shared<Snapshot>()),
                                std::memory_order_release);
}

void ChopAudioCache::clearPrepared()
{
    std::lock_guard<std::mutex> lock (writerMutex);

    const auto current = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
    if (current == nullptr || current->preparedEntries.empty())
        return;

    auto next = std::make_shared<Snapshot> (*current);
    next->preparedEntries.clear();
    std::atomic_store_explicit (&snapshot,
                                std::static_pointer_cast<const Snapshot> (next),
                                std::memory_order_release);
}

std::shared_ptr<ChopAudioCache::Entry>
ChopAudioCache::renderChopSync (const juce::AudioBuffer<float>& source,
                                double sampleRate,
                                int chopId,
                                int chopStartSample,
                                int chopEndSample,
                                const std::vector<ChopWarpMarker>& markers,
                                std::uint64_t generation)
{
    auto entry = std::make_shared<Entry>();
    entry->chopId     = chopId;
    entry->generation = generation;

    if (chopEndSample <= chopStartSample
        || sampleRate <= 0.0
        || source.getNumSamples() <= 0
        || source.getNumChannels() <= 0)
    {
        entry->warpedBuffer = nullptr;
        entry->isIdentity = markers.empty();
        return entry;
    }

    WarpMap warpMap;
    warpMap.build (chopStartSample, chopEndSample, markers, sampleRate);
    entry->isIdentity = warpMap.isIdentity();

    const int frames = warpedChopFrameCount (warpMap);
    if (frames <= 0)
    {
        entry->warpedBuffer = nullptr;
        return entry;
    }

    const int channels   = source.getNumChannels();
    const int sourceSize = source.getNumSamples();

    auto warped = std::make_shared<juce::AudioBuffer<float>> (channels, frames);

    const bool renderOk = renderWarpedChopBungee (warpMap, source, *warped, sampleRate);
    if (! renderOk)
    {
        // Bungee render failed (very unlikely) — fall back to linear resampling.
        std::vector<const float*> srcPtrs ((size_t) channels, nullptr);
        std::vector<float*>       dstPtrs ((size_t) channels, nullptr);
        for (int ch = 0; ch < channels; ++ch)
        {
            srcPtrs[(size_t) ch] = source.getReadPointer (ch);
            dstPtrs[(size_t) ch] = warped->getWritePointer (ch);
        }
        renderWarpedChopLinear (warpMap,
                                srcPtrs.data(),
                                sourceSize,
                                channels,
                                dstPtrs.data(),
                                frames);
    }

    entry->warpedBuffer = warped;
    return entry;
}

ChopAudioCache::PreparedKey ChopAudioCache::makePreparedKey (
    int chopStartSample,
    int chopEndSample,
    int cueOffsetSamples,
    const std::vector<ChopWarpMarker>& markers,
    double sourceSampleRate,
    double outputSampleRate,
    float pitchSemitones,
    float stretchRatio) noexcept
{
    PreparedKey key;
    key.chopStartSample = chopStartSample;
    key.chopEndSample = chopEndSample;
    key.cueOffsetSamples = cueOffsetSamples;
    key.sourceSampleRate = (int) std::round (sourceSampleRate);
    key.outputSampleRate = (int) std::round (outputSampleRate);
    key.pitchCents = (int) std::round (pitchSemitones * 100.0f);
    key.stretchPpm = (int) std::round (juce::jlimit (0.25f, 4.0f, stretchRatio) * 100000.0f);

    std::uint64_t hash = 1469598103934665603ULL;
    hash = mixHash (hash, (std::uint64_t) markers.size());
    for (const auto& marker : markers)
    {
        hash = mixHash (hash, (std::uint64_t) (std::int64_t) marker.sourceSample);
        hash = mixHash (hash, (std::uint64_t) std::llround (marker.localTimeSeconds * 1000000000.0));
        hash = mixHash (hash, marker.snappedToGrid ? 1ULL : 0ULL);
        hash = mixHash (hash, (std::uint64_t) std::llround (marker.gridFingerprint * 1000000000.0));
    }
    key.warpHash = hash;

    return key;
}

std::shared_ptr<ChopAudioCache::PreparedEntry>
ChopAudioCache::renderPreparedChopSync (const juce::AudioBuffer<float>& source,
                                        double sourceSampleRate,
                                        double outputSampleRate,
                                        int chopId,
                                        int chopStartSample,
                                        int chopEndSample,
                                        int cueOffsetSamples,
                                        const std::vector<ChopWarpMarker>& markers,
                                        float pitchSemitones,
                                        float stretchRatio,
                                        std::uint64_t generation)
{
    auto entry = std::make_shared<PreparedEntry>();
    entry->chopId = chopId;
    entry->generation = generation;
    entry->key = makePreparedKey (chopStartSample,
                                  chopEndSample,
                                  cueOffsetSamples,
                                  markers,
                                  sourceSampleRate,
                                  outputSampleRate,
                                  pitchSemitones,
                                  stretchRatio);

    if (chopEndSample <= chopStartSample
        || sourceSampleRate <= 0.0
        || outputSampleRate <= 0.0
        || stretchRatio <= 0.0f
        || source.getNumSamples() <= 0
        || source.getNumChannels() <= 0)
    {
        return entry;
    }

    // Build the pitch-neutral "base" buffer that the pitch/time stretch runs on.
    // Non-warp chops are simply the source slice, so skip the redundant identity
    // Bungee pass that renderChopSync would perform — that roughly halves the bake
    // time (the streamed chops are exactly the non-warp ones) and makes the result
    // match the live fallback exactly, since that path also stretches straight from
    // the source. Warped chops still bake their marker timing first.
    // Pre-chop lead-in (source frames) baked into the FRONT of `base` so the Bungee
    // pass can prime its look-ahead and the captured chop starts at full amplitude
    // instead of Bungee's startup silence/ramp. Mirrors the live note-on prime.
    // 0 for warp chops (their base is a pre-rendered warped buffer — no source
    // lead-in available). chopBaseFrames is the chop length WITHOUT the lead-in.
    int preRollFrames  = 0;
    int chopBaseFrames = 0;

    std::shared_ptr<const juce::AudioBuffer<float>> baseHolder;
    if (markers.empty())
    {
        const int sliceStart = juce::jlimit (0, juce::jmax (0, source.getNumSamples() - 1), chopStartSample);
        const int sliceEnd   = juce::jlimit (sliceStart, source.getNumSamples(), chopEndSample);
        const int chopLen    = juce::jmax (0, sliceEnd - sliceStart);
        if (chopLen <= 0)
            return entry;

        // Grab up to ~0.37s of audio before the chop (>= Bungee's look-ahead) when
        // it exists; chops at the very start of the sample get whatever is available.
        const int preRollWanted = 16384;
        const int preRoll  = juce::jmin (sliceStart, preRollWanted);
        const int sliceLen = preRoll + chopLen;

        auto slice = std::make_shared<juce::AudioBuffer<float>> (source.getNumChannels(), sliceLen);
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            slice->copyFrom (ch, 0, source, ch, sliceStart - preRoll, sliceLen);
        baseHolder = std::move (slice);

        preRollFrames  = preRoll;
        chopBaseFrames = chopLen;
    }
    else
    {
        auto warpedEntry = renderChopSync (source,
                                           sourceSampleRate,
                                           chopId,
                                           chopStartSample,
                                           chopEndSample,
                                           markers,
                                           generation);
        if (warpedEntry == nullptr || warpedEntry->warpedBuffer == nullptr
            || warpedEntry->warpedBuffer->getNumSamples() <= 0)
        {
            return entry;
        }

        baseHolder = warpedEntry->warpedBuffer;
        chopBaseFrames = warpedEntry->warpedBuffer->getNumSamples(); // no lead-in
    }

    const auto& base = *baseHolder;
    const int baseFrames = base.getNumSamples();
    const int channels = base.getNumChannels();
    const auto clampedStretch = juce::jlimit (0.25f, 4.0f, stretchRatio);
    const auto sourceFramesPerOutputFrame =
        (sourceSampleRate / outputSampleRate) / (double) clampedStretch;
    // Output length is the CHOP only — base may carry pre-roll lead-in at the front.
    const int outputFrames = juce::jmax (
        1, (int) std::llround ((double) chopBaseFrames / sourceFramesPerOutputFrame));

    WarpMap warpMap;
    warpMap.build (chopStartSample, chopEndSample, markers, sourceSampleRate);

    const auto cueSource = juce::jlimit (chopStartSample,
                                         juce::jmax (chopStartSample, chopEndSample - 1),
                                         chopStartSample + cueOffsetSamples);
    const auto cueLocalSeconds = warpMap.localTimeAtSourceSample ((double) cueSource);
    const auto cueBaseFrame = juce::jlimit (0.0,
                                            (double) juce::jmax (0, baseFrames - 1),
                                            cueLocalSeconds * sourceSampleRate);

    auto prepared = std::make_shared<juce::AudioBuffer<float>> (channels, outputFrames);
    prepared->clear();

    const bool pitchIsUnity = std::abs (pitchSemitones) < 0.01f;
    const bool stretchIsUnity = std::abs (clampedStretch - 1.0f) < 0.005f;
    const bool ratesMatch = std::abs (sourceSampleRate - outputSampleRate) < 0.5;

    bool renderedWithBungee = false;
    if (pitchIsUnity && stretchIsUnity && ratesMatch)
    {
        // Skip the pre-roll lead-in baked into the front of base (chop starts there).
        for (int ch = 0; ch < channels; ++ch)
            prepared->copyFrom (ch, 0, base, ch, preRollFrames, juce::jmin (chopBaseFrames, outputFrames));
    }
    else
    {
        renderedWithBungee = renderPreparedWithBungee (base,
                                                       *prepared,
                                                       sourceSampleRate,
                                                       outputSampleRate,
                                                       pitchSemitones,
                                                       clampedStretch,
                                                       preRollFrames);
        if (! renderedWithBungee
            && ! renderPreparedWithInterpolation (base,
                                                  *prepared,
                                                  sourceFramesPerOutputFrame,
                                                  preRollFrames))
        {
            return entry;
        }
    }

    sanitiseAndNormalisePreparedBuffer (*prepared);

    const double cueOutputFrame = cueBaseFrame / sourceFramesPerOutputFrame;
    entry->cueFrame = juce::jlimit (0,
                                    juce::jmax (0, outputFrames - 1),
                                    (int) std::llround (cueOutputFrame));
    entry->buffer = prepared;
    entry->renderedWithBungee = renderedWithBungee;
    return entry;
}

} // namespace cuesampler
