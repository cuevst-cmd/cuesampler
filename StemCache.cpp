#include "StemCache.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace cuesampler
{

namespace
{

// Container magic + version. A version bump orphans every existing entry
// instead of risking a misread of an older layout.
constexpr char kMagic[4]   = { 'C', 'S', 'T', 'M' };
constexpr juce::uint32 kContainerVersion = 2;

constexpr const char* kEntryExtension = ".cuestems";

// Integer audio clips floating-point overshoots and changes subtraction on
// recall. Version 2 preserves every float, including peaks beyond full scale.
constexpr int kStemBitsPerSample = 32;

// Serializing a cache write against a concurrent prune keeps a store from
// being deleted by the trim it just triggered.
std::mutex& cacheMutex()
{
    static std::mutex m;
    return m;
}

// Presents an AudioBuffer's raw sample data as one contiguous stream so SHA256
// can consume it without first copying it into a second (100 MB+) block.
// Channels appear back to back, which is enough for hashing — the format only
// has to be stable, not meaningful.
class BufferSampleStream final : public juce::InputStream
{
public:
    explicit BufferSampleStream (const juce::AudioBuffer<float>& bufferIn)
        : buffer (bufferIn),
          bytesPerChannel ((juce::int64) juce::jmax (0, bufferIn.getNumSamples())
                               * (juce::int64) sizeof (float)),
          totalBytes (bytesPerChannel * (juce::int64) juce::jmax (0, bufferIn.getNumChannels()))
    {
    }

    juce::int64 getTotalLength() override { return totalBytes; }
    juce::int64 getPosition() override    { return position; }
    bool isExhausted() override           { return position >= totalBytes; }

    bool setPosition (juce::int64 newPosition) override
    {
        position = juce::jlimit ((juce::int64) 0, totalBytes, newPosition);
        return true;
    }

    int read (void* destBuffer, int maxBytesToRead) override
    {
        if (destBuffer == nullptr || maxBytesToRead <= 0 || bytesPerChannel <= 0)
            return 0;

        auto* dest = static_cast<char*> (destBuffer);
        int written = 0;

        while (written < maxBytesToRead && position < totalBytes)
        {
            const auto channel         = (int) (position / bytesPerChannel);
            const auto offsetInChannel = position % bytesPerChannel;
            const auto chunk = (int) juce::jmin ((juce::int64) (maxBytesToRead - written),
                                                 bytesPerChannel - offsetInChannel);

            const auto* source = reinterpret_cast<const char*> (buffer.getReadPointer (channel));
            std::memcpy (dest + written, source + offsetInChannel, (size_t) chunk);

            written  += chunk;
            position += chunk;
        }

        return written;
    }

private:
    const juce::AudioBuffer<float>& buffer;
    juce::int64 bytesPerChannel = 0;
    juce::int64 totalBytes = 0;
    juce::int64 position = 0;
};

bool encodeStem (const juce::AudioBuffer<float>& buffer, double sampleRate, juce::MemoryBlock& out)
{
    out.reset();

    const auto numChannels = buffer.getNumChannels();
    const auto numSamples  = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (out, false);
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate (sampleRate)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (kStemBitsPerSample)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = wav.createWriterFor (stream, options);
    if (writer == nullptr)
        return false;

    if (! writer->writeFromAudioSampleBuffer (buffer, 0, numSamples))
        return false;

    writer.reset(); // flushes and closes the stream before we inspect 'out'
    return out.getSize() > 0;
}

// Decodes one stem, which must describe exactly the shape the container header
// promised. Checking before allocating means a corrupt FLAC header cannot talk
// us into an enormous buffer.
bool decodeStem (const juce::MemoryBlock& data,
                 int expectedChannels,
                 int expectedSamples, double expectedRate, bool legacy,
                 juce::AudioBuffer<float>& out)
{
    if (data.getSize() == 0 || expectedChannels <= 0 || expectedSamples <= 0)
        return false;

    juce::FlacAudioFormat flac;
    juce::WavAudioFormat wav;
    juce::AudioFormat& format = legacy ? static_cast<juce::AudioFormat&> (flac)
                                      : static_cast<juce::AudioFormat&> (wav);
    // A float WAV must contain at least this many PCM bytes. Reject forged
    // dimensions before allocation, even when its header claims to match.
    if (! legacy && (juce::uint64) expectedChannels * (juce::uint64) expectedSamples * 4 > data.getSize())
        return false;
    auto reader = std::unique_ptr<juce::AudioFormatReader> (
        format.createReaderFor (new juce::MemoryInputStream (data, false), true));

    if (reader == nullptr
        || (int) reader->numChannels != expectedChannels
        || reader->lengthInSamples != (juce::int64) expectedSamples
        || reader->sampleRate != expectedRate)
        return false;

    out.setSize (expectedChannels, expectedSamples, false, true, false);
    return reader->read (&out, 0, expectedSamples, 0, true, true);
}

juce::File entryFileForKey (const juce::String& key)
{
    if (key.isEmpty())
        return {};

    // Keys are our own hex digests, but a corrupted project could carry
    // anything — refuse to build a path out of a key that isn't one.
    if (! key.containsOnly ("0123456789abcdefABCDEF"))
        return {};

    return StemCache::getCacheDirectory().getChildFile (key + kEntryExtension);
}

} // namespace

juce::File StemCache::getCacheDirectory()
{
    const auto resolve = [] () -> juce::File
    {
        if (const char* env = std::getenv ("CUE_STEM_CACHE_DIR"))
        {
            const juce::String path = juce::String::fromUTF8 (env);
            // An absolute path only: a relative one would resolve against
            // whatever working directory the host happens to have.
            if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
                return juce::File (path);
        }

        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("CueSampler")
                   .getChildFile ("stem-cache");
    };

    const auto dir = resolve();
    dir.createDirectory();
    return dir;
}

juce::String StemCache::makeModelId (const juce::String& modelPath)
{
    const juce::File model (modelPath);
    if (modelPath.isEmpty() || ! model.existsAsFile())
        return {};

    const auto descriptor = model.getFileName()
                          + "|" + juce::String (model.getSize())
                          + "|" + juce::String (model.getLastModificationTime().toMilliseconds());

    return juce::SHA256 (descriptor.toRawUTF8(),
                         descriptor.getNumBytesAsUTF8()).toHexString().substring (0, 16);
}

juce::String StemCache::makeKey (const juce::AudioBuffer<float>& buffer,
                                 double sampleRate,
                                 const juce::String& modelId)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0
        || sampleRate <= 0.0 || modelId.isEmpty())
        return {};

    BufferSampleStream stream (buffer);
    const auto audioHash = juce::SHA256 (stream, stream.getTotalLength()).toHexString();

    // Fold the buffer's shape in alongside the PCM hash: the same bytes read
    // as a different channel count or rate are a different separation.
    // New separations must not reuse stems produced by the delayed resampler.
    // Existing embedded/cache-key project states still load their saved sound.
    const auto descriptor = "aligned-float-v3|" + audioHash
                          + "|" + juce::String (buffer.getNumChannels())
                          + "|" + juce::String (buffer.getNumSamples())
                          + "|" + juce::String (sampleRate, 6)
                          + "|" + modelId;

    return juce::SHA256 (descriptor.toRawUTF8(),
                         descriptor.getNumBytesAsUTF8()).toHexString();
}

bool StemCache::contains (const juce::String& key)
{
    const auto file = entryFileForKey (key);
    return file != juce::File() && file.existsAsFile() && file.getSize() > 0;
}

bool StemCache::load (const juce::String& key, Entry& result)
{
    const auto file = entryFileForKey (key);
    if (file == juce::File() || file.getSize() <= 0
        || file.getSize() > std::numeric_limits<int>::max())
        return false;
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || ! decode (data, result))
        return false;
    file.setLastModificationTime (juce::Time::getCurrentTime());
    return true;
}

bool StemCache::decode (const juce::MemoryBlock& data, Entry& result)
{
    juce::MemoryInputStream input (data, false);
    char magic[4] = {};
    if (input.read (magic, 4) != 4 || std::memcmp (magic, kMagic, 4) != 0)
        return false;
    const auto version = (juce::uint32) input.readInt();
    if (version != 1 && version != kContainerVersion)
        return false;
    const auto sampleRate = input.readDouble();
    const auto numChannels = input.readInt();
    const auto numSamples = input.readInt();
    if (! std::isfinite (sampleRate) || sampleRate < 8000.0 || sampleRate > 384000.0
        || numChannels < 1 || numChannels > 2 || numSamples <= 0
        || (double) numSamples > sampleRate * 900.0)
        return false;

    Entry decoded;
    for (auto* target : { &decoded.drums, &decoded.bass, &decoded.vocals })
    {
        if (input.getNumBytesRemaining() < 8)
            return false;
        const auto byteLength = input.readInt64();
        if (byteLength <= 0 || byteLength > input.getNumBytesRemaining()
            || byteLength > std::numeric_limits<int>::max())
            return false;
        juce::MemoryBlock encoded ((size_t) byteLength);
        if (input.read (encoded.getData(), (int) byteLength) != byteLength
            || ! decodeStem (encoded, numChannels, numSamples, sampleRate, version == 1, *target))
            return false;
    }
    if (! input.isExhausted())
        return false;
    decoded.sampleRate = sampleRate;
    result = std::move (decoded);
    return true;
}

bool StemCache::encode (const Entry& entry, juce::MemoryBlock& result)
{
    result.reset();
    const auto channels = entry.drums.getNumChannels();
    const auto samples = entry.drums.getNumSamples();
    if (! std::isfinite (entry.sampleRate) || entry.sampleRate < 8000.0 || entry.sampleRate > 384000.0
        || channels < 1 || channels > 2 || samples <= 0 || samples > entry.sampleRate * 900.0)
        return false;
    for (const auto* stem : { &entry.bass, &entry.vocals })
        if (stem->getNumChannels() != channels || stem->getNumSamples() != samples)
            return false;

    juce::MemoryOutputStream output (result, false);
    output.write (kMagic, 4);
    output.writeInt ((int) kContainerVersion);
    output.writeDouble (entry.sampleRate);
    output.writeInt (channels);
    output.writeInt (samples);
    for (const auto* stem : { &entry.drums, &entry.bass, &entry.vocals })
    {
        juce::MemoryBlock encoded;
        if (! encodeStem (*stem, entry.sampleRate, encoded))
            return false;
        output.writeInt64 ((juce::int64) encoded.getSize());
        output.write (encoded.getData(), encoded.getSize());
    }
    output.flush();
    return true;
}

bool StemCache::store (const juce::String& key, const Entry& entry)
{
    juce::MemoryBlock encoded;
    return encode (entry, encoded) && storeEncoded (key, encoded);
}

bool StemCache::storeEncoded (const juce::String& key, const juce::MemoryBlock& data)
{
    const auto file = entryFileForKey (key);
    if (file == juce::File() || data.isEmpty())
        return false;
    const std::lock_guard<std::mutex> lock (cacheMutex());
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream output (temp.getFile());
        if (! output.openedOk() || ! output.write (data.getData(), data.getSize()))
            return false;
        output.flush();
        if (output.getStatus().failed())
            return false;
    }
    return temp.overwriteTargetFileWithTemporary();
}

void StemCache::prune (std::int64_t maxBytes)
{
    if (maxBytes <= 0)
        return;

    const std::lock_guard<std::mutex> lock (cacheMutex());

    auto entries = getCacheDirectory().findChildFiles (juce::File::findFiles, false,
                                                       juce::String ("*") + kEntryExtension);

    std::int64_t total = 0;
    for (const auto& file : entries)
        total += file.getSize();

    if (total <= maxBytes)
        return;

    // Oldest use first.
    std::sort (entries.begin(), entries.end(),
               [] (const juce::File& a, const juce::File& b)
               {
                   return a.getLastModificationTime() < b.getLastModificationTime();
               });

    for (const auto& file : entries)
    {
        if (total <= maxBytes)
            break;

        const auto size = file.getSize();
        if (file.deleteFile())
            total -= size;
    }
}

void StemCache::clear()
{
    const std::lock_guard<std::mutex> lock (cacheMutex());

    for (const auto& file : getCacheDirectory().findChildFiles (juce::File::findFiles, false,
                                                                juce::String ("*") + kEntryExtension))
        file.deleteFile();
}

} // namespace cuesampler
