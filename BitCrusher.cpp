#include "BitCrusher.h"

#include <cmath>

namespace cuesampler
{

void BitCrusher::prepare (double sampleRate, int /*maxBlockSize*/, int /*numChannels*/)
{
    juce::ignoreUnused (sampleRate);
    reset();
}

void BitCrusher::reset()
{
    heldSample.fill (0.0f);
    phase = 1.0; // ensure the first sample after reset triggers a fresh capture
}

void BitCrusher::setParametersImmediately (bool shouldEnable, float bitsIn, float crushPercentIn) noexcept
{
    setEnabled (shouldEnable);
    setBits    (bitsIn);
    setCrush   (crushPercentIn);
}

void BitCrusher::process (juce::AudioBuffer<float>& buffer)
{
    if (! enabled.load (std::memory_order_acquire))
        return;

    const float bits     = bitsParam .load (std::memory_order_acquire);
    const float crushPct = crushParam.load (std::memory_order_acquire);

    const int numChannels = juce::jmin (buffer.getNumChannels(), (int) heldSample.size());
    const int numSamples  = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return;

    const bool quantize = bits     < 15.99f;
    const bool resample = crushPct < 99.99f;

    if (! quantize && ! resample)
        return;

    const float steps    = std::pow (2.0f, bits - 1.0f);
    const float invSteps = 1.0f / steps;

    const double phaseStep = juce::jlimit (1.0e-4, 1.0, (double) (crushPct * 0.01f));

    for (int i = 0; i < numSamples; ++i)
    {
        bool tick = true;
        if (resample)
        {
            phase += phaseStep;
            tick = phase >= 1.0;
            if (tick)
                phase -= 1.0;
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            float x    = data[i];

            if (resample)
            {
                if (tick)
                    heldSample[(size_t) ch] = x;
                x = heldSample[(size_t) ch];
            }

            if (quantize)
                x = std::round (x * steps) * invSteps;

            data[i] = x;
        }
    }
}

} // namespace cuesampler
