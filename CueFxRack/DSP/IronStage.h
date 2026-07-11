#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace cue::dsp
{
    // ========================================================================
    // IronStage — the shared "API" output stage.
    //
    // Models the 2520 discrete op-amp + output transformer character
    // (see docs/API_SOUND_RESEARCH.md):
    //   1. low-shelf pre-emphasis so lows hit the nonlinearity first
    //      (transformer core saturates earlier at low frequencies)
    //   2. 2x-oversampled waveshaper: odd harmonics (tanh, slope-normalised)
    //      plus a touch of even-order asymmetry
    //   3. inverse shelf de-emphasis, then a DC blocker (the x^2 term
    //      generates DC)
    // ========================================================================
    class IronStage
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            numChannels = (int) spec.numChannels;
            oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
                              spec.numChannels, 1,
                              juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                              true, false);
            oversampler->initProcessing (spec.maximumBlockSize);

            preEmph.resize ((size_t) numChannels);
            deEmph.resize ((size_t) numChannels);
            dcX.assign ((size_t) numChannels, 0.0f);
            dcY.assign ((size_t) numChannels, 0.0f);

            for (auto& f : preEmph)
                f.setCoefficients (juce::IIRCoefficients::makeLowShelf (
                    spec.sampleRate, 130.0, 0.707, juce::Decibels::decibelsToGain (3.5f)));
            for (auto& f : deEmph)
                f.setCoefficients (juce::IIRCoefficients::makeLowShelf (
                    spec.sampleRate, 130.0, 0.707, juce::Decibels::decibelsToGain (-3.5f)));

            reset();
        }

        void reset()
        {
            if (oversampler != nullptr)
                oversampler->reset();
            for (auto& f : preEmph)  f.reset();
            for (auto& f : deEmph)   f.reset();
            std::fill (dcX.begin(), dcX.end(), 0.0f);
            std::fill (dcY.begin(), dcY.end(), 0.0f);
        }

        void setAmount (float newAmount)   { amount = juce::jlimit (0.0f, 1.0f, newAmount); }

        void process (juce::AudioBuffer<float>& buffer)
        {
            if (amount < 0.001f || oversampler == nullptr)
                return;

            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (numChannels, buffer.getNumChannels());

            for (int ch = 0; ch < nCh; ++ch)
                preEmph[(size_t) ch].processSamples (buffer.getWritePointer (ch), n);

            juce::dsp::AudioBlock<float> block (buffer);
            auto upBlock = oversampler->processSamplesUp (block);

            // Blend shaper: y = x + k*(tanh(x) + even*x^2 - x).
            // Unity small-signal gain (bypass-honest for mastering), harmonic
            // content scales with `amount`, and no hidden hard cap below the
            // downstream limiter ceiling — the saturator colours, the
            // brickwall protects.
            const auto k    = amount;
            const auto even = 0.05f * amount;

            for (size_t ch = 0; ch < upBlock.getNumChannels(); ++ch)
            {
                auto* d = upBlock.getChannelPointer (ch);
                for (size_t i = 0; i < upBlock.getNumSamples(); ++i)
                {
                    const auto x   = d[i];
                    const auto sat = std::tanh (x) + even * x * x;
                    d[i] = x + k * (sat - x);
                }
            }

            oversampler->processSamplesDown (block);

            for (int ch = 0; ch < nCh; ++ch)
            {
                deEmph[(size_t) ch].processSamples (buffer.getWritePointer (ch), n);

                // DC blocker (~10 Hz)
                auto* d  = buffer.getWritePointer (ch);
                auto& x1 = dcX[(size_t) ch];
                auto& y1 = dcY[(size_t) ch];
                for (int i = 0; i < n; ++i)
                {
                    const auto x = d[i];
                    y1   = x - x1 + 0.995f * y1;
                    x1   = x;
                    d[i] = y1;
                }
            }
        }

    private:
        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
        std::vector<juce::SingleThreadedIIRFilter> preEmph, deEmph;
        std::vector<float> dcX, dcY;
        int numChannels = 2;
        float amount = 0.25f;
    };
}
