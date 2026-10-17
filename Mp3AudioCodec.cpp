#include "Mp3AudioCodec.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <lame.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace cuesampler
{

namespace
{
struct LameDeleter
{
    void operator() (lame_global_flags* flags) const noexcept
    {
        if (flags != nullptr)
            lame_close (flags);
    }
};

using LameHandle = std::unique_ptr<lame_global_flags, LameDeleter>;

int compatibleMp3Rate (double rate)
{
    static constexpr std::array<int, 9> supported
    {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
    };

    int closest = supported.front();
    for (const auto candidate : supported)
        if (candidate <= rate)
            closest = candidate;
    return closest;
}
} // namespace

bool Mp3AudioCodec::encode (const juce::AudioBuffer<float>& audio,
                            double inputSampleRate,
                            juce::MemoryBlock& mp3Data,
                            EncodedInfo* encodedInfo,
                            int bitrateKbps,
                            const std::function<bool (float)>& progress)
{
    mp3Data.reset();
    if (encodedInfo != nullptr)
        *encodedInfo = {};

    const auto channels = audio.getNumChannels();
    const auto samples = audio.getNumSamples();
    if (channels < 1 || channels > 2 || samples <= 0
        || ! std::isfinite (inputSampleRate) || inputSampleRate < 8000.0
        || inputSampleRate > 384000.0 || bitrateKbps < 8 || bitrateKbps > 320)
        return false;

    LameHandle lame (lame_init());
    if (lame == nullptr)
        return false;

    const auto inputRate = juce::roundToInt (inputSampleRate);
    const auto outputRate = compatibleMp3Rate (inputSampleRate);
    if (lame_set_in_samplerate (lame.get(), inputRate) < 0
        || lame_set_out_samplerate (lame.get(), outputRate) < 0
        || lame_set_num_channels (lame.get(), channels) < 0
        || lame_set_mode (lame.get(), channels == 1 ? MONO : JOINT_STEREO) < 0
        || lame_set_VBR (lame.get(), vbr_off) < 0
        || lame_set_brate (lame.get(), bitrateKbps) < 0
        || lame_set_quality (lame.get(), 2) < 0
        || lame_set_bWriteVbrTag (lame.get(), 1) < 0
        || lame_init_params (lame.get()) < 0)
        return false;

    juce::MemoryOutputStream encoded (mp3Data, false);
    constexpr int blockSize = 8192;
    // LAME's documented upper bound for one encode call is 1.25*n + 7200.
    std::vector<unsigned char> bytes ((size_t) (blockSize * 5 / 4 + 7200));
    const auto* left = audio.getReadPointer (0);
    const auto* right = audio.getReadPointer (channels > 1 ? 1 : 0);

    for (int offset = 0; offset < samples; offset += blockSize)
    {
        if (progress && ! progress ((float) offset / (float) samples))
            return false;
        const auto count = juce::jmin (blockSize, samples - offset);
        const auto written = lame_encode_buffer_ieee_float (
            lame.get(), left + offset, right + offset, count,
            bytes.data(), (int) bytes.size());
        if (written < 0 || (written > 0 && ! encoded.write (bytes.data(), (size_t) written)))
            return false;
    }

    if (progress && ! progress (1.0f))
        return false;
    const auto flushed = lame_encode_flush (lame.get(), bytes.data(), (int) bytes.size());
    if (flushed < 0 || (flushed > 0 && ! encoded.write (bytes.data(), (size_t) flushed)))
        return false;
    encoded.flush();

    // Replace LAME's placeholder first frame with its final Xing/LAME tag.
    // Decoders use this to remove encoder delay and end padding.
    const auto tagBytes = lame_get_lametag_frame (lame.get(), bytes.data(), bytes.size());
    if (tagBytes > bytes.size() || tagBytes > mp3Data.getSize())
        return false;
    if (tagBytes > 0)
        std::memcpy (mp3Data.getData(), bytes.data(), tagBytes);

    if (mp3Data.isEmpty())
        return false;

    if (encodedInfo != nullptr)
    {
        encodedInfo->sampleRate = inputSampleRate;
        encodedInfo->channels = channels;
        encodedInfo->samples = samples;
    }

    return true;
}

bool Mp3AudioCodec::decode (const juce::MemoryBlock& mp3Data,
                            double sampleRate,
                            int channels,
                            int samples,
                            juce::AudioBuffer<float>& audio)
{
    if (mp3Data.isEmpty() || ! std::isfinite (sampleRate)
        || sampleRate < 8000.0 || sampleRate > 384000.0
        || channels < 1 || channels > 2 || samples <= 0)
        return false;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader> (
        formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (mp3Data, false)));

    // JUCE's software reader exposes LAME's 576-sample encoder delay plus the
    // Layer III synthesis filterbank's 529 samples. The LAME tag remains in the
    // stream so players with gapless support can perform the same trim.
    constexpr juce::int64 decoderStart = 1105;
    if (reader == nullptr || reader->numChannels != (unsigned int) channels
        || reader->sampleRate < 8000.0 || reader->sampleRate > 48000.0)
        return false;

    const auto encodedSamples = juce::jmax (1, (int) std::llround ((double) samples * reader->sampleRate / sampleRate));
    if (reader->lengthInSamples < decoderStart + encodedSamples)
        return false;

    juce::AudioBuffer<float> decoded (channels, encodedSamples);
    if (! reader->read (&decoded, 0, encodedSamples, decoderStart, true, true))
        return false;

    if (encodedSamples == samples && juce::approximatelyEqual (reader->sampleRate, sampleRate))
    {
        audio = std::move (decoded);
        return true;
    }

    // MP3 tops out at 48 kHz. Restore the project's original frame clock with
    // zero-delay interpolation so saved chop indices and stem subtraction keep
    // the same duration and alignment at 88.2/96/192 kHz.
    audio.setSize (channels, samples, false, true, false);
    const double ratio = reader->sampleRate / sampleRate;
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* source = decoded.getReadPointer (ch);
        auto* destination = audio.getWritePointer (ch);
        for (int i = 0; i < samples; ++i)
        {
            const auto position = (double) i * ratio;
            const auto left = juce::jlimit (0, encodedSamples - 1, (int) position);
            const auto right = juce::jmin (encodedSamples - 1, left + 1);
            const auto fraction = (float) (position - std::floor (position));
            destination[i] = source[left] + fraction * (source[right] - source[left]);
        }
    }
    return true;
}

} // namespace cuesampler
