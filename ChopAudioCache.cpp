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

// Renders `base` through Bungee at the given pitch/stretch into `prepared`.
//
// base[] is laid out as [preRollSourceFrames of lead-in | chop | tail], and
// `prepared` receives only the chop. Bungee::Stream emits, at any moment, the
// input position Stream::latency() frames BEHIND what has been fed (roughly
// maxInputFrameCount/2 plus a grain hop — about 4200 frames, ~95 ms, at
// 44.1 kHz), so the feed cursor is kept that far ahead of the output timeline.
// Skipping the pre-roll in output-frame space alone, as this used to do, left
// that delay uncompensated: every prepared chop started ~95 ms of pre-chop
// audio early (silence when the chop sat near the start of the sample) and
// lost the same amount off its tail.
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

    // Feeds `frameCount` base frames starting at `fromBase` (zero-padded past
    // the end of base) and pulls `outputFrames` of output into outputChunk.
    // Returns the frames Bungee emitted, or -1 on a buffer-pointer failure.
    auto feedBlock = [&] (double fromBase, int frameCount, double outputFrames) -> int
    {
        const int copyStart = juce::jlimit (0, sourceLength, (int) std::floor (fromBase));
        const int copyLen   = juce::jlimit (0, frameCount, sourceLength - copyStart);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* input  = inputBuffer.getWritePointer (ch);
            auto* output = outputChunk.getWritePointer (ch);
            if (input == nullptr || output == nullptr)
                return -1;

            if (copyLen > 0)
            {
                const auto* sourceData = base.getReadPointer (ch);
                if (sourceData == nullptr)
                    return -1;
                std::copy (sourceData + copyStart, sourceData + copyStart + copyLen, input);
            }
            if (copyLen < frameCount)
                std::fill (input + copyLen, input + frameCount, 0.0f);

            inPtrs[(size_t) ch] = input;
            outPtrs[(size_t) ch] = output;
        }

        outputChunk.clear();
        return stream.process (inPtrs.data(), outPtrs.data(),
                               frameCount, outputFrames, pitchFactor);
    };

    // ---- Prime -----------------------------------------------------------
    // Push the feed cursor to `latency` frames into base with all output
    // discarded, so the first kept output frame lands on base[0]... which is
    // the start of the pre-roll lead-in, not the chop. The lead-in is then
    // skipped in output-frame space by preRollOutputFrames below, exactly as
    // before — the prime is purely about removing the pipeline delay.
    double feedCursor    = 0.0;
    int    latencyFrames = juce::jmax (1, stretcher.maxInputFrameCount() / 2);
    bool   latencyKnown  = false;

    const int primeFrameBudget = 6 * stretcher.maxInputFrameCount() + 16;
    int       primeFed         = 0;

    // Capping the input per call at (block * ratio) keeps process()'s in/out
    // ratio — which IS request.speed — at the render's ratio; feeding more than
    // the output request supports would ask Bungee for a wildly wrong speed.
    const int primeFeedCap = juce::jmax (1,
        (int) std::ceil ((double) kBungeeBlockSize
                         * juce::jlimit (0.05, 20.0, sourceFramesPerOutputFrame)));

    while (feedCursor < (double) latencyFrames && primeFed < primeFrameBudget)
    {
        const double remaining = (double) latencyFrames - feedCursor;
        const int feedNow = juce::jlimit (1,
                                          juce::jmin (juce::jmin (maxInputFrames, primeFeedCap),
                                                      primeFrameBudget - primeFed),
                                          (int) std::ceil (remaining));
        const double outWanted = juce::jlimit (1.0,
                                               (double) kBungeeBlockSize,
                                               std::ceil ((double) feedNow
                                                          / juce::jmax (1.0e-3, sourceFramesPerOutputFrame)));

        if (feedBlock (feedCursor, feedNow, outWanted) < 0)
            return false;

        feedCursor += (double) feedNow;
        primeFed   += feedNow;

        // Re-read every pass, not just the first: latency() is only valid once
        // a grain exists, and the value it reports climbs to its steady state
        // as the pipeline fills.
        const int measured = juce::jlimit (0,
                                           2 * stretcher.maxInputFrameCount(),
                                           (int) std::ceil (stream.latency()));
        latencyFrames = latencyKnown ? juce::jmax (latencyFrames, measured) : measured;
        latencyKnown  = true;
    }

    // Whatever the prime actually reached is the delay the render must assume,
    // so a short probe block or the budget guard can never desynchronise the
    // feed cursor from the output timeline.
    const double effectiveLatency = juce::jmax (0.0, feedCursor);

    // ---- Render ----------------------------------------------------------
    // Output frame o corresponds to base position o * sourceFramesPerOutputFrame;
    // the feed cursor tracks that plus the pipeline latency. Deriving the feed
    // amount from an absolute target each block keeps the walk self-correcting
    // rather than accumulating per-block rounding.
    const int preRollOutputFrames = juce::jmax (0,
        (int) std::llround ((double) preRollSourceFrames / sourceFramesPerOutputFrame));
    const int totalOutputWanted = preRollOutputFrames + targetFrames;

    int outputRendered = 0; // total output produced so far, including discarded pre-roll
    int zeroRenderStreak = 0;

    while (outputRendered < totalOutputWanted)
    {
        const int segmentOutputFrames =
            juce::jmin (kBungeeBlockSize, totalOutputWanted - outputRendered);

        const double desiredFeedEnd =
            (double) (outputRendered + segmentOutputFrames) * sourceFramesPerOutputFrame
            + effectiveLatency;

        const int inputFramesRequested = juce::jlimit (
            1, maxInputFrames, (int) std::llround (desiredFeedEnd - feedCursor));

        const int renderedFrames = feedBlock (feedCursor, inputFramesRequested,
                                              (double) segmentOutputFrames);
        if (renderedFrames < 0)
            return false;

        feedCursor += (double) inputFramesRequested;

        if (renderedFrames > 0)
        {
            // Keep only the part of this output block inside the capture window;
            // everything before preRollOutputFrames is the lead-in.
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
        else if (++zeroRenderStreak >= 8)
        {
            return false;
        }
    }

    // Need at least some real (post-pre-roll) chop output to count as a success.
    if (outputRendered <= preRollOutputFrames)
        return false;

    sanitiseAndNormalisePreparedBuffer (prepared);
    return true;
}

// Bungee-driven warp render. Walks the chop's output timeline and feeds source
// samples so that the input consumed per output block matches the WarpMap's
// local speed, writing pitch-preserved output into the target buffer. Returns
// true on success; false if rendering aborted (e.g., Bungee produced no frames
// for many consecutive process calls).
//
// pitchFactor is fixed at 1.0 because per-chop pitchSemitones is applied
// live on top of the cache, not baked into it.
//
// Bungee::Stream is a pipeline: the frame it emits corresponds to an input
// position Stream::latency() frames BEHIND the input already fed (roughly
// maxInputFrameCount/2 plus a grain hop — about 4200 frames, ~95 ms, at
// 44.1 kHz). The renderer therefore feeds past the chop start by that latency
// before keeping any output, and past the chop end by the same amount to flush
// the tail. Priming only with the audio *before* the chop, as this used to do,
// left the delay uncompensated: every baked warp buffer began ~95 ms early
// (silence for a chop near the start of the file) and lost 95 ms off its end,
// so warp markers never lined up with the audio they were placed on.
bool renderWarpedChopBungee (const WarpMap&                  warpMap,
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

    // Output block size for the warp walk. Small blocks keep the in/out ratio
    // tracking the WarpMap closely, so a marker's speed change lands within a
    // few milliseconds of where the user placed it. Bungee's synthesis hop is
    // 256 frames, so going finer than this buys nothing.
    const int kWarpOutBlock = 256;

    std::vector<const float*> inPtrs  ((size_t) channels, nullptr);
    std::vector<float*>       outPtrs ((size_t) channels, nullptr);

    juce::AudioBuffer<float> chunkBuffer (channels, kWarpOutBlock + 16);
    juce::AudioBuffer<float> inputBuffer (channels, maxProcessInputFrames);
    inputBuffer.clear();

    const double startSrc = nodes.front().sourceSample;
    const double endSrc   = nodes.back().sourceSample;
    if (endSrc <= startSrc)
        return false;

    // Feeds `frameCount` source frames starting at `fromSource` (zero-padded
    // where that runs outside the source buffer) and pulls `outputFrames` of
    // output into chunkBuffer. Returns the number of frames Bungee emitted, or
    // -1 on a buffer-pointer failure.
    auto feedBlock = [&] (double fromSource, int frameCount, double outputFrames) -> int
    {
        const int copyStart = juce::jlimit (0, sourceLength, (int) std::floor (fromSource));
        const int copyLen   = juce::jlimit (0, frameCount, sourceLength - copyStart);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* tempInput = inputBuffer.getWritePointer (ch);
            auto* chunkOut  = chunkBuffer.getWritePointer (ch);
            if (tempInput == nullptr || chunkOut == nullptr)
                return -1;

            if (copyLen > 0)
            {
                const auto* srcChan = source.getReadPointer (ch);
                if (srcChan == nullptr)
                    return -1;
                std::copy (srcChan + copyStart, srcChan + copyStart + copyLen, tempInput);
            }
            if (copyLen < frameCount)
                std::fill (tempInput + copyLen, tempInput + frameCount, 0.0f);

            inPtrs[(size_t) ch]  = tempInput;
            outPtrs[(size_t) ch] = chunkOut;
        }

        chunkBuffer.clear();
        return stream.process (inPtrs.data(), outPtrs.data(),
                               frameCount, outputFrames, pitchFactor);
    };

    // ---- Prime -----------------------------------------------------------
    // Feed [startSrc - leadIn, startSrc + latency) with the output discarded.
    // The lead-in gives the first kept grain real history to analyse; the
    // latency portion is what pushes Bungee's emit point up to startSrc.
    const double firstLocalDur        = nodes[1].localTimeSeconds - nodes[0].localTimeSeconds;
    const double firstSourceDurFrames = nodes[1].sourceSample - nodes[0].sourceSample;
    if (firstLocalDur <= 0.0 || firstSourceDurFrames <= 0.0)
        return false;
    const double firstSpeed = firstSourceDurFrames / (firstLocalDur * sampleRate);

    const int leadInWanted = juce::jmax (1, stretcher.maxInputFrameCount() / 2);
    const int leadIn       = juce::jmin ((int) std::floor (juce::jmax (0.0, startSrc)), leadInWanted);

    double feedCursor = startSrc - (double) leadIn;

    // Provisional until the first process() call makes Stream::latency()
    // readable (it dereferences the last synthesised grain, so it must not be
    // called before one exists).
    int  latencyFrames = leadInWanted;
    bool latencyKnown  = false;

    // Never let a bad latency reading turn the prime into an unbounded loop.
    const int primeFrameBudget = 6 * stretcher.maxInputFrameCount() + 16;
    int       primeFed         = 0;

    // Prime at the first segment's speed. Capping the input per call at
    // (block * speed) keeps process()'s in/out ratio — which IS request.speed —
    // at that value; feeding more than the output request supports would ask
    // Bungee for a speed far outside its range and poison the grain history.
    const double primeSpeed = juce::jlimit (WarpMap::kMinSpeed, WarpMap::kMaxSpeed, firstSpeed);
    const int    primeFeedCap = juce::jmax (1, (int) std::ceil ((double) kWarpOutBlock * primeSpeed));

    while (feedCursor < startSrc + (double) latencyFrames && primeFed < primeFrameBudget)
    {
        const double remaining = startSrc + (double) latencyFrames - feedCursor;
        const int feedNow = juce::jlimit (1,
                                          juce::jmin (juce::jmin (maxProcessInputFrames, primeFeedCap),
                                                      primeFrameBudget - primeFed),
                                          (int) std::ceil (remaining));
        const double outWanted = juce::jlimit (1.0,
                                               (double) kWarpOutBlock,
                                               std::ceil ((double) feedNow / primeSpeed));

        if (feedBlock (feedCursor, feedNow, outWanted) < 0)
            return false;

        feedCursor += (double) feedNow;
        primeFed   += feedNow;

        // Re-read every pass, not just the first: latency() is only valid once
        // a grain exists, and the value it reports climbs to its steady state
        // as the pipeline fills.
        const int measured = juce::jlimit (0,
                                           2 * stretcher.maxInputFrameCount(),
                                           (int) std::ceil (stream.latency()));
        latencyFrames = latencyKnown ? juce::jmax (latencyFrames, measured) : measured;
        latencyKnown  = true;
    }

    // Whatever the prime actually reached is the delay the render must assume,
    // so a short first block (or the budget guard) can never desynchronise the
    // feed cursor from the output timeline.
    const double effectiveLatency = juce::jmax (0.0, feedCursor - startSrc);

    // ---- Output-driven render -------------------------------------------
    // For output frame o the chop should be sounding source position
    // warpMap.sourceSampleAtLocalTime(o / sampleRate); the feed cursor has to
    // sit `effectiveLatency` frames ahead of that. Deriving the feed amount
    // from that target each block makes the walk self-correcting: rounding
    // never accumulates, and the in/out ratio automatically becomes the local
    // warp speed, including across marker boundaries.
    int outWritten       = 0;
    int zeroRenderStreak = 0;

    // Bungee's pipeline delay shifts with the speed it is running at, so a
    // marker that changes speed also changes the delay — holding it fixed
    // leaves a constant ~16 ms step on the far side of every marker. Track it
    // instead, heavily smoothed: the raw reading is grain-quantised, and
    // feeding to chase it block-by-block oscillates, but a slow tracker
    // converges and keeps the whole chop within about a millisecond.
    double smoothedLatency = effectiveLatency;

    while (outWritten < targetFrames)
    {
        const int blockOut = juce::jmin (kWarpOutBlock, targetFrames - outWritten);

        smoothedLatency += 0.02 * (juce::jmax (0.0, stream.latency()) - smoothedLatency);

        const double srcAtBlockEnd =
            warpMap.sourceSampleAtLocalTime ((double) (outWritten + blockOut) / sampleRate);

        // Clamp to the WarpMap's own speed limits so a self-correction can
        // never ask Bungee for a ratio it does not support.
        const int minIn = juce::jmax (1, (int) std::floor ((double) blockOut * WarpMap::kMinSpeed));
        const int maxIn = juce::jmin (maxProcessInputFrames,
                                      (int) std::ceil ((double) blockOut * WarpMap::kMaxSpeed) + 8);

        const int inFrames = juce::jlimit (juce::jmin (minIn, maxIn),
                                           maxIn,
                                           (int) std::llround (srcAtBlockEnd + smoothedLatency - feedCursor));

        const int rendered = feedBlock (feedCursor, inFrames, (double) blockOut);
        if (rendered < 0)
            return false;

        feedCursor += (double) inFrames;

        if (rendered > 0)
        {
            const int writable = juce::jmin (rendered,
                                             juce::jmin (chunkBuffer.getNumSamples(),
                                                         targetFrames - outWritten));
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* src     = chunkBuffer.getReadPointer (ch);
                auto*       dstChan = target.getWritePointer (ch);
                if (src == nullptr || dstChan == nullptr)
                    return false;
                std::copy (src, src + writable, dstChan + outWritten);
            }
            outWritten += writable;
            zeroRenderStreak = 0;
        }
        else if (++zeroRenderStreak >= 8)
        {
            return false;
        }
    }

    return outWritten > 0;
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
    // `base` is laid out as [preRollFrames of lead-in | chop | tail]. The lead-in
    // gives the Bungee pass real audio to warm its grain history on, and the tail
    // gives it enough look-ahead to flush the chop's final frames instead of
    // fading them into the zero-padding. chopBaseFrames is the chop length only.
    int preRollFrames  = 0;
    int chopBaseFrames = 0;

    // ~0.37 s each side, comfortably beyond Bungee's look-ahead (about 0.1 s).
    // Chops at the very start or end of the sample get whatever is available.
    const int kContextWanted = 16384;

    std::shared_ptr<const juce::AudioBuffer<float>> baseHolder;
    if (markers.empty())
    {
        const int sliceStart = juce::jlimit (0, juce::jmax (0, source.getNumSamples() - 1), chopStartSample);
        const int sliceEnd   = juce::jlimit (sliceStart, source.getNumSamples(), chopEndSample);
        const int chopLen    = juce::jmax (0, sliceEnd - sliceStart);
        if (chopLen <= 0)
            return entry;

        const int preRoll  = juce::jmin (sliceStart, kContextWanted);
        const int postRoll = juce::jmin (source.getNumSamples() - sliceEnd, kContextWanted);
        const int sliceLen = preRoll + chopLen + postRoll;

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

        const auto& warped = *warpedEntry->warpedBuffer;
        const int warpedFrames = warped.getNumSamples();
        const int chanCount = juce::jmax (warped.getNumChannels(), source.getNumChannels());

        // Splice raw source context either side of the warped chop. Its only job
        // is to give the pitch/stretch pass grain history and flush room; both
        // regions are discarded, so the seam at the chop edges is never heard.
        const int sliceStart = juce::jlimit (0, juce::jmax (0, source.getNumSamples() - 1), chopStartSample);
        const int sliceEnd   = juce::jlimit (sliceStart, source.getNumSamples(), chopEndSample);
        const int preRoll    = juce::jmin (sliceStart, kContextWanted);
        const int postRoll   = juce::jmin (source.getNumSamples() - sliceEnd, kContextWanted);

        auto padded = std::make_shared<juce::AudioBuffer<float>> (chanCount,
                                                                  preRoll + warpedFrames + postRoll);
        padded->clear();
        for (int ch = 0; ch < chanCount; ++ch)
        {
            if (preRoll > 0)
                padded->copyFrom (ch, 0, source, juce::jmin (ch, source.getNumChannels() - 1),
                                  sliceStart - preRoll, preRoll);
            padded->copyFrom (ch, preRoll, warped, juce::jmin (ch, warped.getNumChannels() - 1),
                              0, warpedFrames);
            if (postRoll > 0)
                padded->copyFrom (ch, preRoll + warpedFrames, source,
                                  juce::jmin (ch, source.getNumChannels() - 1),
                                  sliceEnd, postRoll);
        }

        baseHolder     = std::move (padded);
        preRollFrames  = preRoll;
        chopBaseFrames = warpedFrames;
    }

    const auto& base = *baseHolder;
    const int channels = base.getNumChannels();
    const auto clampedStretch = juce::jlimit (0.25f, 4.0f, stretchRatio);
    const auto sourceFramesPerOutputFrame =
        (sourceSampleRate / outputSampleRate) / (double) clampedStretch;
    // Output length is the CHOP only — base carries lead-in at the front and a
    // flush tail at the back, neither of which reaches the prepared buffer.
    const int outputFrames = juce::jmax (
        1, (int) std::llround ((double) chopBaseFrames / sourceFramesPerOutputFrame));

    WarpMap warpMap;
    warpMap.build (chopStartSample, chopEndSample, markers, sourceSampleRate);

    const auto cueSource = juce::jlimit (chopStartSample,
                                         juce::jmax (chopStartSample, chopEndSample - 1),
                                         chopStartSample + cueOffsetSamples);
    const auto cueLocalSeconds = warpMap.localTimeAtSourceSample ((double) cueSource);
    // Chop-relative (local time 0 == chop start), so the lead-in never enters it.
    const auto cueBaseFrame = juce::jlimit (0.0,
                                            (double) juce::jmax (0, chopBaseFrames - 1),
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
