#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cstdint>

namespace cuesampler
{

// On-disk cache of HTDemucs separation results.
//
// Separation is a tens-of-seconds offline pass, and its output used to live
// only in RAM: closing a project (or reloading the same file) threw the stems
// away and the user had to run the whole pass again. This stores each result
// under a key derived from the audio it was computed from plus the identity of
// the model that computed it, so reopening a project rehydrates the stems from
// disk in well under a second.
//
// Entries are three FLAC streams in one container file, written atomically via
// a temp file + rename so a crash mid-write can never leave a half-entry that
// later reads as valid. Everything here is blocking file I/O — call it from a
// background thread, never the message or audio thread.
class StemCache
{
public:
    struct Entry
    {
        juce::AudioBuffer<float> drums, bass, vocals;
        double sampleRate = 0.0;
    };

    // Key for a separation of 'buffer' by the model identified by 'modelId'.
    // Hashes the raw PCM, so two different samples can't collide onto one
    // entry, and a changed model invalidates every key it produced.
    //
    // The key must be PERSISTED (see the processor's "stemCacheKey" state
    // property) rather than recomputed on restore: a project embeds its sample
    // as 16-bit FLAC, so the buffer that comes back from a restore is a
    // quantized version of the one that was separated and hashes differently.
    static juce::String makeKey (const juce::AudioBuffer<float>& buffer,
                                 double sampleRate,
                                 const juce::String& modelId);

    // Identity of the model file at 'modelPath', folded into every key. Covers
    // the file's size and modification time, so swapping in a new htdemucs
    // build orphans the old entries instead of serving stale stems.
    static juce::String makeModelId (const juce::String& modelPath);

    // <userApplicationDataDirectory>/CueSampler/stem-cache, created on demand -
    // ~/Library/CueSampler/stem-cache on macOS, alongside the update-check cache
    // the processor already keeps there. Override with CUE_STEM_CACHE_DIR (an
    // absolute path) to put it somewhere roomier than the system drive: it holds
    // whole songs' worth of audio. Same env-var convention as the separator's
    // CUE_STEM_OVERLAP / CUE_STEM_SEGMENT tuning knobs.
    static juce::File getCacheDirectory();

    // True if 'key' names a readable entry. Cheap — does not decode the audio.
    static bool contains (const juce::String& key);

    // Decodes the entry for 'key'. Returns false on a miss, a truncated or
    // malformed container, or a version this build doesn't understand.
    static bool load (const juce::String& key, Entry& result);

    // Encodes and writes the three stems under 'key', replacing any existing
    // entry. Touches the entry's modification time so prune() treats a re-store
    // as a use. Returns false if encoding or the write failed.
    static bool store (const juce::String& key, const Entry& entry);

    // Trims the cache to 'maxBytes' by deleting least-recently-used entries.
    // Called after every successful store().
    static void prune (std::int64_t maxBytes = defaultBudgetBytes);

    // Deletes every entry. Exposed for a future "clear cache" affordance.
    static void clear();

    // Roughly a dozen full-length songs' worth of stems.
    static constexpr std::int64_t defaultBudgetBytes = 4ll * 1024ll * 1024ll * 1024ll;

private:
    StemCache() = delete;
};

} // namespace cuesampler
