#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

namespace cue::dsp
{
    // ========================================================================
    // CueChorus / CueFlanger — thin wrappers over juce::dsp::Chorus with the
    // ranges voiced for each effect (long modulated delay = chorus, short
    // delay + feedback = flanger).
    // ========================================================================
    class CueChorus
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)   { chorus.prepare (spec); }
        void reset()                                        { chorus.reset(); }

        void setParams (float rateHz, float depth01, float delayMs, float feedback01, float mix01)
        {
            chorus.setRate (rateHz);
            chorus.setDepth (juce::jlimit (0.0f, 1.0f, depth01));
            chorus.setCentreDelay (juce::jlimit (5.0f, 30.0f, delayMs));
            chorus.setFeedback (juce::jlimit (-0.95f, 0.95f, feedback01));
            chorus.setMix (juce::jlimit (0.0f, 1.0f, mix01));
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            juce::dsp::AudioBlock<float> block (buffer);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            chorus.process (ctx);
        }

    private:
        juce::dsp::Chorus<float> chorus;
    };

    //==========================================================================
    class CueFlanger
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)   { flanger.prepare (spec); }
        void reset()                                        { flanger.reset(); }

        void setParams (float rateHz, float depth01, float feedback01, float mix01)
        {
            flanger.setRate (rateHz);
            flanger.setDepth (juce::jlimit (0.0f, 1.0f, depth01));
            flanger.setCentreDelay (3.0f);                       // classic flange zone
            flanger.setFeedback (juce::jlimit (-0.95f, 0.95f, feedback01));
            flanger.setMix (juce::jlimit (0.0f, 1.0f, mix01));
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            juce::dsp::AudioBlock<float> block (buffer);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            flanger.process (ctx);
        }

    private:
        juce::dsp::Chorus<float> flanger;
    };

    // ========================================================================
    // CueFlangus — multi-voice ensemble flanger: three modulated taps per
    // channel with LFO phases 120 degrees apart; SPREAD offsets the LFO phase
    // between channels for width. Lush, dimensional, and distinct from the
    // single-voice flanger.
    // ========================================================================
    class CueFlangus
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sr = spec.sampleRate;
            line.setMaximumDelayInSamples ((int) std::ceil (sr * 0.05));
            line.prepare (spec);
            reset();
        }

        void reset()
        {
            line.reset();
            phase = 0.0f;
        }

        void setParams (float rateHz, float depth01, float spread01, float mix01)
        {
            rate   = juce::jlimit (0.05f, 3.0f, rateHz);
            depth  = juce::jlimit (0.0f, 1.0f, depth01);
            spread = juce::jlimit (0.0f, 1.0f, spread01);
            mix    = juce::jlimit (0.0f, 1.0f, mix01);
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());

            const auto inc      = juce::MathConstants<float>::twoPi * rate / (float) sr;
            const auto baseSamp = (float) (0.006 * sr);              // 6 ms centre
            const auto depSamp  = depth * (float) (0.004 * sr);      // +/-4 ms swing

            for (int i = 0; i < n; ++i)
            {
                phase += inc;
                if (phase > juce::MathConstants<float>::twoPi)
                    phase -= juce::MathConstants<float>::twoPi;

                for (int ch = 0; ch < nCh; ++ch)
                {
                    auto* d = buffer.getWritePointer (ch);
                    const auto chPhase = phase + (float) ch * spread * juce::MathConstants<float>::halfPi;

                    line.pushSample (ch, d[i]);

                    auto wet = 0.0f;
                    for (int v = 0; v < 3; ++v)
                    {
                        const auto lfo = std::sin (chPhase + (float) v * (juce::MathConstants<float>::twoPi / 3.0f));
                        const auto tap = juce::jmax (1.0f, baseSamp + depSamp * lfo + (float) v * 1.7f);
                        wet += line.popSample (ch, tap, v == 2);     // advance on last tap
                    }
                    wet *= (1.0f / 3.0f);

                    d[i] = d[i] * (1.0f - mix) + wet * mix;
                }
            }
        }

    private:
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line { 4800 };
        double sr = 44100.0;
        float rate = 0.4f, depth = 0.5f, spread = 0.7f, mix = 0.5f, phase = 0.0f;
    };
}
