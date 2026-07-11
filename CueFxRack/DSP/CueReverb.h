#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace cue::dsp
{
    // ========================================================================
    // CueReverb — juce::Reverb wrapped with a wet-path pre-delay and
    // dry/wet mixing. Size + decay both feed room size; damp and width map
    // directly.
    // ========================================================================
    class CueReverb
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sr = spec.sampleRate;
            reverb.setSampleRate (sr);

            preDelayLine.setMaximumDelayInSamples ((int) std::ceil (sr * 0.25));
            preDelayLine.prepare (spec);

            wetBuffer.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);
            reset();
        }

        void reset()
        {
            reverb.reset();
            preDelayLine.reset();
        }

        void setParams (float size01, float decaySec, float damp01,
                        float preDelayMs, float width01, float mix01)
        {
            const auto decay01 = juce::jlimit (0.0f, 1.0f, (decaySec - 0.1f) / 14.9f);

            juce::Reverb::Parameters p;
            p.roomSize   = juce::jlimit (0.0f, 1.0f, 0.20f + 0.55f * decay01 + 0.25f * size01);
            p.damping    = juce::jlimit (0.0f, 1.0f, 0.05f + 0.90f * damp01);
            p.width      = width01;
            p.wetLevel   = 1.0f;
            p.dryLevel   = 0.0f;
            p.freezeMode = 0.0f;
            reverb.setParameters (p);

            preDelaySamples = (float) (juce::jlimit (0.0f, 200.0f, preDelayMs) * 0.001 * sr);
            mix = juce::jlimit (0.0f, 1.0f, mix01);
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());

            // wet path: pre-delayed copy of the input
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* src = buffer.getReadPointer (ch);
                auto* wet = wetBuffer.getWritePointer (ch);
                for (int i = 0; i < n; ++i)
                {
                    preDelayLine.pushSample (ch, src[i]);
                    wet[i] = preDelayLine.popSample (ch, preDelaySamples);
                }
            }

            if (nCh > 1)
                reverb.processStereo (wetBuffer.getWritePointer (0), wetBuffer.getWritePointer (1), n);
            else
                reverb.processMono (wetBuffer.getWritePointer (0), n);

            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* dry = buffer.getWritePointer (ch);
                auto* wet = wetBuffer.getReadPointer (ch);
                for (int i = 0; i < n; ++i)
                    dry[i] = dry[i] * (1.0f - mix) + wet[i] * mix;
            }
        }

    private:
        juce::Reverb reverb;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> preDelayLine { 48000 };
        juce::AudioBuffer<float> wetBuffer;
        double sr = 44100.0;
        float preDelaySamples = 0.0f, mix = 0.25f;
    };
}
