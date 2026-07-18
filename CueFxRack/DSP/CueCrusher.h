#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "IronStage.h"

namespace cue::dsp
{
    // ========================================================================
    // CueCrusher — bit-depth quantiser + sample-rate reducer, with the drive
    // control pushing into an IronStage so "DRIVE" carries the 2520 flavour.
    // ========================================================================
    class CueCrusher
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            iron.prepare (spec);
            dryBuffer.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);   // no audio-thread allocs
            driveSmooth.reset (spec.sampleRate, 0.02);
            driveSmooth.setCurrentAndTargetValue (1.0f);
            mixSmooth.reset (spec.sampleRate, 0.02);
            mixSmooth.setCurrentAndTargetValue (1.0f);
            reset();
        }

        void reset()
        {
            holdCount[0] = holdCount[1] = 0;
            holdSample[0] = holdSample[1] = 0.0f;
            iron.reset();
        }

        void setParams (float bits_, int rateDivide_, float driveDb, float mix01)
        {
            bits       = juce::jlimit (1.0f, 16.0f, bits_);
            rateDivide = juce::jmax (1, rateDivide_);
            driveSmooth.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (0.0f, 24.0f, driveDb)));
            iron.setAmount (juce::jlimit (0.0f, 1.0f, 0.15f + driveDb / 24.0f * 0.65f));
            mixSmooth.setTargetValue (juce::jlimit (0.0f, 1.0f, mix01));
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());

            dryBuffer.makeCopyOf (buffer, true);

            // drive into the iron (ramped so DRIVE rides don't zipper)
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                auto dr = driveSmooth;                 // identical ramp per channel
                for (int i = 0; i < n; ++i)
                    d[i] *= dr.getNextValue();
                if (ch == nCh - 1)
                    driveSmooth = dr;                  // keep the real state advanced
            }
            iron.process (buffer);

            const auto steps = std::pow (2.0f, bits - 1.0f);

            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d   = buffer.getWritePointer (ch);
                auto& cnt = holdCount[ch];
                auto& smp = holdSample[ch];

                for (int i = 0; i < n; ++i)
                {
                    // sample-rate reduction: hold every Nth sample
                    if (cnt <= 0)
                    {
                        smp = d[i];
                        cnt = rateDivide;
                    }
                    --cnt;

                    // bit-depth quantise
                    d[i] = std::floor (smp * steps + 0.5f) / steps;
                }
            }

            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* wet = buffer.getWritePointer (ch);
                auto* dry = dryBuffer.getReadPointer (ch);
                auto mx = mixSmooth;                   // identical ramp per channel
                for (int i = 0; i < n; ++i)
                {
                    const auto m = mx.getNextValue();
                    wet[i] = dry[i] * (1.0f - m) + wet[i] * m;
                }
                if (ch == nCh - 1)
                    mixSmooth = mx;                    // keep the real state advanced
            }
        }

    private:
        IronStage iron;
        juce::AudioBuffer<float> dryBuffer;
        juce::SmoothedValue<float> driveSmooth, mixSmooth;
        float bits = 16.0f;
        int rateDivide = 1;
        int holdCount[2] { 0, 0 };
        float holdSample[2] { 0.0f, 0.0f };
    };
}
