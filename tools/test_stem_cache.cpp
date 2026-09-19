// Offline test for the on-disk StemCache.
//
// Covers the properties the project-restore path depends on: keys are stable
// for identical audio and distinct for anything else, a stored entry decodes
// back sample-accurately, a damaged or foreign entry is refused rather than
// half-decoded, and prune() honours its budget oldest-first.
//
//   cmake -S . -B build -DCUE_BUILD_STEM_CACHE_TEST=ON
//   cmake --build build --target test_stem_cache
//   ./build/test_stem_cache_artefacts/test_stem_cache
//
// Runs against a scratch directory via CUE_STEM_CACHE_DIR so it never touches
// the real user cache.

#include "../StemCache.h"

#include <juce_core/juce_core.h>

#include <cstdlib>
#include <iostream>

namespace
{

int failures = 0;

void check (bool condition, const juce::String& what)
{
    std::cout << (condition ? "  ok   " : "  FAIL ") << what << std::endl;
    if (! condition)
        ++failures;
}

// Deterministic pseudo-audio: a different 'seed' gives materially different
// samples, so two calls can stand in for two unrelated stems.
juce::AudioBuffer<float> makeBuffer (int numChannels, int numSamples, int seed)
{
    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    juce::Random random (seed);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            data[i] = (random.nextFloat() * 2.0f - 1.0f) * 0.5f;
    }

    return buffer;
}

float maxDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return 1.0f;

    float worst = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (a.getReadPointer (ch)[i] - b.getReadPointer (ch)[i]));

    return worst;
}

} // namespace

int main (int argc, char** argv)
{
    // Isolate from the real cache before anything touches getCacheDirectory().
    const auto base = argc > 1 ? juce::File (juce::String (argv[1]))
                              : juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto scratch = base.getChildFile ("cue-stem-cache-test-"
                                            + juce::String (juce::Time::currentTimeMillis()));
    if (scratch.createDirectory().failed())
        return 2;
   #if JUCE_WINDOWS
    ::_putenv_s ("CUE_STEM_CACHE_DIR", scratch.getFullPathName().toRawUTF8());
   #else
    ::setenv ("CUE_STEM_CACHE_DIR", scratch.getFullPathName().toRawUTF8(), 1);
   #endif

    check (cuesampler::StemCache::getCacheDirectory() == scratch,
           "CUE_STEM_CACHE_DIR redirects the cache directory");

    constexpr double sampleRate = 44100.0;
    constexpr int    numSamples = 44100; // 1 s
    const auto mix = makeBuffer (2, numSamples, 1);

    // ---- keys ---------------------------------------------------------------
    const auto key = cuesampler::StemCache::makeKey (mix, sampleRate, "model-a");

    check (key.isNotEmpty(), "makeKey returns a key for valid audio");
    check (key == cuesampler::StemCache::makeKey (mix, sampleRate, "model-a"),
           "makeKey is stable for identical audio");
    check (key != cuesampler::StemCache::makeKey (makeBuffer (2, numSamples, 2), sampleRate, "model-a"),
           "different audio yields a different key");
    check (key != cuesampler::StemCache::makeKey (mix, 48000.0, "model-a"),
           "a different sample rate yields a different key");
    check (key != cuesampler::StemCache::makeKey (mix, sampleRate, "model-b"),
           "a different model yields a different key");
    check (cuesampler::StemCache::makeKey (mix, sampleRate, {}).isEmpty(),
           "no model id yields no key");
    check (cuesampler::StemCache::makeKey ({}, sampleRate, "model-a").isEmpty(),
           "an empty buffer yields no key");

    // ---- round trip ---------------------------------------------------------
    cuesampler::StemCache::Entry written;
    written.sampleRate = sampleRate;
    written.drums  = makeBuffer (2, numSamples, 10);
    written.bass   = makeBuffer (2, numSamples, 11);
    written.vocals = makeBuffer (2, numSamples, 12);

    check (! cuesampler::StemCache::contains (key), "a fresh key is not in the cache");
    check (cuesampler::StemCache::store (key, written), "store writes an entry");
    check (cuesampler::StemCache::contains (key), "the stored key is now in the cache");

    cuesampler::StemCache::Entry read;
    check (cuesampler::StemCache::load (key, read), "load finds the stored entry");
    check (read.sampleRate == sampleRate, "the sample rate round-trips");

    // 24-bit FLAC: one LSB is ~1.2e-7, so anything at 1e-6 is a real corruption.
    check (maxDifference (written.drums,  read.drums)  < 1.0e-6f, "drums round-trip sample-accurately");
    check (maxDifference (written.bass,   read.bass)   < 1.0e-6f, "bass round-trips sample-accurately");
    check (maxDifference (written.vocals, read.vocals) < 1.0e-6f, "vocals round-trip sample-accurately");

    // Stems must not be swapped in the container.
    check (maxDifference (written.drums, read.bass) > 0.01f, "stems keep their identity");

    // Floating-point overshoots and quiet detail must survive exactly.
    written.drums.setSample (0, 0, 1.25f);
    written.bass.setSample (0, 0, -1.5f);
    written.vocals.setSample (1, 1, 1.0e-12f);
    check (cuesampler::StemCache::store (key, written), "store float peaks");
    check (cuesampler::StemCache::load (key, read), "load float peaks");
    check (maxDifference (written.drums, read.drums) == 0.0f, "positive overshoots survive bit-exactly");
    check (maxDifference (written.bass, read.bass) == 0.0f, "negative overshoots survive bit-exactly");
    check (maxDifference (written.vocals, read.vocals) == 0.0f, "quiet detail survives bit-exactly");

    juce::MemoryBlock portable;
    check (cuesampler::StemCache::encode (written, portable), "encode portable project stems");
    cuesampler::StemCache::clear();
    check (cuesampler::StemCache::decode (portable, read), "decode stems without the disk cache");
    check (maxDifference (written.drums, read.drums) == 0.0f, "portable stems are exact");

    // Version-1 projects must still read their original FLAC cache entries.
    juce::MemoryBlock legacy;
    {
        juce::MemoryOutputStream container (legacy, false);
        container.write ("CSTM", 4);
        container.writeInt (1);
        container.writeDouble (sampleRate);
        container.writeInt (2);
        container.writeInt (numSamples);
        for (const auto* stem : { &written.drums, &written.bass, &written.vocals })
        {
            juce::MemoryBlock audio;
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (audio, false);
            juce::FlacAudioFormat flac;
            auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions()
                .withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24));
            writer->writeFromAudioSampleBuffer (*stem, 0, numSamples);
            writer.reset();
            container.writeInt64 ((juce::int64) audio.getSize());
            container.write (audio.getData(), audio.getSize());
        }
    }
    check (cuesampler::StemCache::decode (legacy, read), "legacy v1 FLAC entries remain readable");
    check (cuesampler::StemCache::store (key, written), "restore disk fixture");

    // ---- misses and malformed entries --------------------------------------
    cuesampler::StemCache::Entry ignored;
    check (! cuesampler::StemCache::load (juce::String ("ab12cd34").paddedRight ('f', 64), ignored),
           "an unknown key misses");
    check (! cuesampler::StemCache::load ("../../etc/passwd", ignored),
           "a key that is not a hex digest is refused");
    check (! cuesampler::StemCache::load ({}, ignored), "an empty key misses");

    // A truncated container must read as a miss, not as a partial entry.
    const auto entryFile = scratch.getChildFile (key + ".cuestems");
    const auto fullSize  = entryFile.getSize();
    {
        juce::MemoryBlock whole;
        entryFile.loadFileAsData (whole);
        whole.setSize ((size_t) (fullSize / 2));
        entryFile.replaceWithData (whole.getData(), whole.getSize());
    }
    check (! cuesampler::StemCache::load (key, ignored), "a truncated entry is refused");

    // Garbage in place of the container is refused on the magic.
    entryFile.replaceWithText ("not a stem container");
    check (! cuesampler::StemCache::load (key, ignored), "an entry with a bad header is refused");

    // ---- mismatched stems are rejected at store time ------------------------
    cuesampler::StemCache::Entry ragged;
    ragged.sampleRate = sampleRate;
    ragged.drums  = makeBuffer (2, numSamples, 20);
    ragged.bass   = makeBuffer (2, numSamples / 2, 21); // shorter than the others
    ragged.vocals = makeBuffer (2, numSamples, 22);
    check (! cuesampler::StemCache::store (key, ragged), "stems of unequal length are refused");

    // ---- prune --------------------------------------------------------------
    cuesampler::StemCache::clear();

    juce::StringArray storedKeys;
    for (int i = 0; i < 4; ++i)
    {
        const auto k = cuesampler::StemCache::makeKey (makeBuffer (2, numSamples, 100 + i),
                                                       sampleRate, "model-a");
        cuesampler::StemCache::Entry e;
        e.sampleRate = sampleRate;
        e.drums  = makeBuffer (2, numSamples, 200 + i);
        e.bass   = makeBuffer (2, numSamples, 300 + i);
        e.vocals = makeBuffer (2, numSamples, 400 + i);
        cuesampler::StemCache::store (k, e);

        // Stagger modification times so the LRU order is unambiguous.
        scratch.getChildFile (k + ".cuestems")
               .setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::minutes (10 - i));
        storedKeys.add (k);
    }

    auto totalBytes = [&scratch]
    {
        juce::int64 total = 0;
        for (const auto& f : scratch.findChildFiles (juce::File::findFiles, false, "*.cuestems"))
            total += f.getSize();
        return total;
    };

    const auto before = totalBytes();
    check (before > 0, "the prune fixture wrote entries");

    cuesampler::StemCache::prune (before);
    check (totalBytes() == before, "prune keeps everything when already under budget");

    cuesampler::StemCache::prune (before / 2);
    check (totalBytes() <= before / 2, "prune trims down to the budget");
    check (! cuesampler::StemCache::contains (storedKeys[0]), "prune evicts the oldest entry first");
    check (cuesampler::StemCache::contains (storedKeys[3]), "prune keeps the newest entry");

    cuesampler::StemCache::clear();
    check (totalBytes() == 0, "clear empties the cache");

    scratch.deleteRecursively();

    std::cout << std::endl
              << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED").toStdString()
              << std::endl;
    return failures == 0 ? 0 : 1;
}
