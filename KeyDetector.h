#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <string>

class KeyDetector
{
public:
    struct Result
    {
        bool valid = false;
        std::string key;
        std::string rootNote;
        bool isMajor = true;
        int rootIndex = 0;
        float confidence = 0.0f;
        std::string camelot;
    };

    // Detects the most likely musical key from an audio buffer using FFT chroma correlation.
    Result detect (const juce::AudioBuffer<float>& buffer, double sampleRate);

private:
    static const std::array<double, 12> majorProfile;
    static const std::array<double, 12> minorProfile;

    // Builds a normalized 12-bin chromagram from mono-summed FFT magnitudes.
    std::array<double, 12> buildChromagram (const juce::AudioBuffer<float>& buffer, double sampleRate);

    // Returns the normalized correlation between the chromagram and a rotated key profile.
    double correlate (const std::array<double, 12>& chroma, const std::array<double, 12>& profile, int shift);
};
