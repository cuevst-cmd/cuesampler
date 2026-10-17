#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <functional>

namespace cuesampler
{

// Offline MP3 encoding used for project persistence, stem caching and exports.
// LAME allocates internally, so every call must stay on an existing worker
// thread and must never be made from processBlock().
class Mp3AudioCodec
{
public:
    struct EncodedInfo
    {
        double sampleRate = 0.0;
        int channels = 0;
        juce::int64 samples = 0;
    };

    static bool encode (const juce::AudioBuffer<float>& audio,
                        double inputSampleRate,
                        juce::MemoryBlock& mp3Data,
                        EncodedInfo* encodedInfo = nullptr,
                        int bitrateKbps = 320,
                        const std::function<bool (float)>& progress = {});

    static bool decode (const juce::MemoryBlock& mp3Data,
                        double sampleRate,
                        int channels,
                        int samples,
                        juce::AudioBuffer<float>& audio);

private:
    Mp3AudioCodec() = delete;
};

} // namespace cuesampler
