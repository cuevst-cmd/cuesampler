#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace cue::dsp
{
    // ========================================================================
    // CueDelay — TAPE delay. Stereo, host sync, ping-pong, and a tape-voiced
    // repeat path modelled on classic tape echoes (Echoplex / Space Echo
    // behaviour): wow (slow) + flutter (fast) modulation of the head position,
    // soft tape saturation in the feedback loop, a low-cut against mud
    // build-up, and the one-pole "tone" low-pass. Delay-time changes are
    // slewed like a motor speeding up/down, giving the classic pitch bend.
    // ========================================================================
    class CueDelay
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sr = spec.sampleRate;
            const auto maxSamples = (int) std::ceil (sr * 4.2);
            line.setMaximumDelayInSamples (maxSamples);
            line.prepare (spec);
            delaySmooth.reset (sr, 0.08);
            delaySmooth.setCurrentAndTargetValue ((float) (0.35 * sr));
            reset();
        }

        void reset()
        {
            line.reset();
            toneLp[0] = toneLp[1] = 0.0f;
            headBumpHp[0] = headBumpHp[1] = 0.0f;
            wowPhase = 0.37f;
            flutterPhase = 0.0f;
        }

        void setParams (float timeMs, bool sync, int divIdx, double bpm,
                        float feedback01, float toneHz, bool pingPong_, float mix01)
        {
            auto ms = timeMs;
            if (sync)
            {
                static const double beats[9] { 4.0, 2.0, 1.0, 1.5, 2.0 / 3.0, 0.5, 0.75, 1.0 / 3.0, 0.25 };
                const auto safeBpm = bpm > 20.0 && bpm < 999.0 ? bpm : 120.0;
                ms = (float) (beats[juce::jlimit (0, 8, divIdx)] * 60000.0 / safeBpm);
            }
            ms = juce::jlimit (1.0f, 4000.0f, ms);
            delaySmooth.setTargetValue ((float) (ms * 0.001 * sr));

            fb       = juce::jlimit (0.0f, 1.05f, feedback01 * 1.08f);   // >1 possible: tape self-oscillation (loop tanh keeps it bounded)
            kTone    = 1.0f - std::exp (-juce::MathConstants<float>::twoPi
                                        * juce::jlimit (100.0f, 18000.0f, toneHz) / (float) sr);
            kHeadHp  = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 70.0f / (float) sr);
            pingPong = pingPong_;
            mix      = juce::jlimit (0.0f, 1.0f, mix01);
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());

            auto* l = buffer.getWritePointer (0);
            auto* r = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            const auto wowInc     = juce::MathConstants<float>::twoPi * 0.62f / (float) sr;
            const auto flutterInc = juce::MathConstants<float>::twoPi * 6.10f / (float) sr;

            for (int i = 0; i < n; ++i)
            {
                // tape transport: wow + flutter wobble the head position
                wowPhase     += wowInc;
                flutterPhase += flutterInc;
                if (wowPhase     > juce::MathConstants<float>::twoPi) wowPhase     -= juce::MathConstants<float>::twoPi;
                if (flutterPhase > juce::MathConstants<float>::twoPi) flutterPhase -= juce::MathConstants<float>::twoPi;

                const auto wobble = 1.0f + 0.0016f * std::sin (wowPhase)
                                         + 0.0004f * std::sin (flutterPhase + 0.8f * std::sin (wowPhase * 2.0f));
                const auto d = juce::jmax (1.0f, delaySmooth.getNextValue() * wobble);

                const auto inL = l[i];
                const auto inR = r != nullptr ? r[i] : inL;

                const auto outL = line.popSample (0, d);
                const auto outR = nCh > 1 ? line.popSample (1, d) : outL;

                if (pingPong)
                {
                    // input feeds L only; repeats bounce L -> R -> L
                    const auto inMono = 0.5f * (inL + inR);
                    line.pushSample (0, inMono + tone (0, outR) * fb);
                    if (nCh > 1)
                        line.pushSample (1, tone (1, outL) * fb);
                }
                else
                {
                    line.pushSample (0, inL + tone (0, outL) * fb);
                    if (nCh > 1)
                        line.pushSample (1, inR + tone (1, outR) * fb);
                }

                l[i] = inL * (1.0f - mix) + outL * mix;
                if (r != nullptr)
                    r[i] = inR * (1.0f - mix) + outR * mix;
            }
        }

    private:
        // Tape repeat path: low-cut (no mud build-up) -> tone LP -> soft
        // saturation. Each generation of the echo degrades like tape.
        float tone (int ch, float x)
        {
            auto& hp = headBumpHp[ch];
            hp += kHeadHp * (x - hp);
            x -= hp;                                   // ~70 Hz low-cut in the loop

            auto& lp = toneLp[ch];
            lp += kTone * (x - lp);

            return std::tanh (lp * 1.15f) * 0.92f;     // tape squash, keeps fb stable
        }

        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line { 48000 * 5 };
        juce::SmoothedValue<float> delaySmooth;
        double sr = 44100.0;
        float fb = 0.35f, kTone = 0.5f, mix = 0.25f;
        float toneLp[2] { 0.0f, 0.0f };
        float headBumpHp[2] { 0.0f, 0.0f };
        float kHeadHp = 0.01f;
        float wowPhase = 0.0f, flutterPhase = 0.0f;
        bool pingPong = false;
    };
}
