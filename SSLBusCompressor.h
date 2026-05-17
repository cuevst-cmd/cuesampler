#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>

namespace cuesampler
{

// SSL G-Series style master bus compressor.
// Feedforward topology, log-domain envelope, soft knee, dual-envelope auto
// release, stereo-linked detector, and -18 dBFS analog-style calibration.
// Ratio locked at 4:1. Output is not limited or clipped; makeup gain is
// purely gain staging.
class SSLBusCompressor
{
public:
    SSLBusCompressor() = default;

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void process (juce::AudioBuffer<float>& buffer);
    void setParametersImmediately (bool shouldEnable, float thresholdDbIn, float makeupDbIn) noexcept;

    // Setters (any thread). Values are smoothed inside process().
    void setEnabled     (bool b)    noexcept { enabled.store (b, std::memory_order_release); }
    void setThresholdDb (float dB)  noexcept { thresholdDb.store (juce::jlimit (-15.0f, 15.0f, dB), std::memory_order_release); }
    void setMakeupDb    (float dB)  noexcept { makeupDb   .store (juce::jlimit (  0.0f, 20.0f, dB), std::memory_order_release); }

    bool  isEnabled()                  const noexcept { return enabled.load (std::memory_order_acquire); }
    float getCurrentGainReductionDb()  const noexcept { return currentGrDb.load (std::memory_order_acquire); }

private:
    std::atomic<bool>  enabled     { false };
    std::atomic<float> thresholdDb { 0.0f };
    std::atomic<float> makeupDb    { 0.0f };

    double fs = 44100.0;

    juce::dsp::IIR::Filter<float> scHpfL, scHpfR;

    // Envelope state (log domain, dB; positive = gain reduction)
    float grFastDb = 0.0f;
    float grSlowDb = 0.0f;

    std::atomic<float> currentGrDb { 0.0f };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedThreshold { 0.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedMakeupLin { 1.0f };

    float attackCoeff      = 0.0f;
    float attackSlowCoeff  = 0.0f;
    float relFastCoeff     = 0.0f;
    float relSlowCoeff     = 0.0f;

    void updateCoefficients();
};

} // namespace cuesampler
