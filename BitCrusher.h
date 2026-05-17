#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace cuesampler
{

// Stereo bit crusher: bit-depth quantization + sample-rate reduction.
// Both knobs sweep "high = clean, low = more effect": BITS == 16 and
// CRUSH == 100 are bypass-equivalent, so the user can dial in just as
// much grit as they want.
class BitCrusher
{
public:
    BitCrusher() = default;

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void process (juce::AudioBuffer<float>& buffer);
    void setParametersImmediately (bool shouldEnable, float bitsIn, float crushPercentIn) noexcept;

    void setEnabled (bool b)        noexcept { enabled   .store (b,                                              std::memory_order_release); }
    void setBits    (float bits)    noexcept { bitsParam .store (juce::jlimit (1.0f,  16.0f,  bits),             std::memory_order_release); }
    void setCrush   (float percent) noexcept { crushParam.store (juce::jlimit (1.0f,  100.0f, percent),          std::memory_order_release); }

    bool isEnabled() const noexcept { return enabled.load (std::memory_order_acquire); }

private:
    std::atomic<bool>  enabled    { false };
    std::atomic<float> bitsParam  { 16.0f };
    std::atomic<float> crushParam { 100.0f };

    std::array<float, 2> heldSample { { 0.0f, 0.0f } };
    double phase = 1.0;
};

} // namespace cuesampler
