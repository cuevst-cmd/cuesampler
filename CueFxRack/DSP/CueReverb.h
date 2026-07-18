#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <vector>

namespace cue::dsp
{
    // ========================================================================
    // CueReverb — Dattorro plate reverb (Jon Dattorro, "Effect Design Part 1:
    // Reverberator and Other Filters", JAES 1997), replacing the old
    // juce::Reverb (Freeverb) wrapper.
    //
    // Topology: pre-delay -> input bandwidth LP -> 4 input diffusers ->
    // figure-8 tank (two cross-coupled halves, each: modulated allpass ->
    // delay -> damping LP -> decay -> allpass -> delay). Stereo output is
    // drawn from 7 taps per side across the tank, per the paper's tap table,
    // so L/R are naturally decorrelated (WIDTH then scales the side signal).
    //
    // All delay lengths are the paper's values (given at 29,761 Hz) scaled to
    // the actual sample rate; SIZE scales the tank continuously (fractional
    // reads everywhere, slewed ~250 ms, so resizing bends tape-style instead
    // of clicking). DECAY maps to a loop gain that realises the requested
    // RT60 for the current tank size. The two tank allpasses are LFO-
    // modulated (0.71 / 0.83 Hz) which keeps long tails alive and chorus-free.
    // ========================================================================
    class CueReverb
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sr    = spec.sampleRate;
            kBase = sr / 29761.0;

            const auto kMax = kBase * 1.02;   // size factor tops out at 1.0 (+ mod margin)

            preDelayLine.setMaximumDelayInSamples ((int) std::ceil (sr * 0.25) + 4);
            preDelayLine.prepare (spec);

            for (int i = 0; i < 4; ++i)
                inAp[i].alloc ((int) std::ceil (inApLen[i] * kMax) + 8);

            apL1.alloc ((int) std::ceil (672.0  * kMax) + (int) std::ceil (14.0 * kMax) + 8);
            delL1.alloc ((int) std::ceil (4453.0 * kMax) + 8);
            apL2.alloc ((int) std::ceil (1800.0 * kMax) + 8);
            delL2.alloc ((int) std::ceil (3720.0 * kMax) + 8);

            apR1.alloc ((int) std::ceil (908.0  * kMax) + (int) std::ceil (14.0 * kMax) + 8);
            delR1.alloc ((int) std::ceil (4217.0 * kMax) + 8);
            apR2.alloc ((int) std::ceil (2656.0 * kMax) + 8);
            delR2.alloc ((int) std::ceil (3163.0 * kMax) + 8);

            sizeSmooth.reset (sr, 0.25);
            sizeSmooth.setCurrentAndTargetValue (1.0f);
            preDelaySmooth.reset (sr, 0.05);
            preDelaySmooth.setCurrentAndTargetValue (0.0f);
            widthSmooth.reset (sr, 0.05);
            widthSmooth.setCurrentAndTargetValue (1.0f);
            mixSmooth.reset (sr, 0.02);
            mixSmooth.setCurrentAndTargetValue (0.25f);

            reset();
        }

        void reset()
        {
            preDelayLine.reset();
            for (auto& a : inAp) a.clear();
            apL1.clear(); delL1.clear(); apL2.clear(); delL2.clear();
            apR1.clear(); delR1.clear(); apR2.clear(); delR2.clear();
            bwState = dampL = dampR = 0.0f;
            feedL = feedR = 0.0f;
            lfoPhase1 = 0.0f;
            lfoPhase2 = juce::MathConstants<float>::pi * 0.5f;
        }

        void setParams (float size01, float decaySec, float damp01,
                        float preDelayMs, float width01, float mix01)
        {
            sizeSmooth.setTargetValue (0.55f + 0.45f * juce::jlimit (0.0f, 1.0f, size01));
            rt60 = juce::jlimit (0.1f, 15.0f, decaySec);
            dampCoef = 1.0f - juce::jlimit (0.0005f, 0.95f, 0.05f + 0.90f * juce::jlimit (0.0f, 1.0f, damp01));
            preDelaySmooth.setTargetValue ((float) (juce::jlimit (0.0f, 200.0f, preDelayMs) * 0.001 * sr));
            widthSmooth.setTargetValue (juce::jlimit (0.0f, 1.0f, width01));
            mixSmooth.setTargetValue (juce::jlimit (0.0f, 1.0f, mix01));
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());

            auto* l = buffer.getWritePointer (0);
            auto* r = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            const auto lfoInc1 = juce::MathConstants<float>::twoPi * 0.71f / (float) sr;
            const auto lfoInc2 = juce::MathConstants<float>::twoPi * 0.83f / (float) sr;

            for (int i = 0; i < n; ++i)
            {
                const auto k = (float) kBase * sizeSmooth.getNextValue();

                // Loop gain for the requested RT60 at the current tank size.
                // Decay applies twice per full figure-8 cycle (once per half).
                const auto loopSec = 21589.0f * k / (float) sr;
                const auto decay   = juce::jmin (0.99995f,
                                                 std::pow (10.0f, -3.0f * loopSec / (2.0f * rt60)));

                // ---- input: mono sum -> pre-delay -> bandwidth -> diffusers
                const auto inL = l[i];
                const auto inR = r != nullptr ? r[i] : inL;

                preDelayLine.pushSample (0, 0.5f * (inL + inR));
                auto x = preDelayLine.popSample (0, preDelaySmooth.getNextValue());

                bwState += 0.9995f * (x - bwState);      // input bandwidth LP
                x = bwState;

                for (int a = 0; a < 4; ++a)
                    x = inAp[a].allpass (x, inApLen[a] * k, inApG[a]);

                // ---- tank LFOs (keep the tail alive without chorusing)
                lfoPhase1 += lfoInc1;
                lfoPhase2 += lfoInc2;
                if (lfoPhase1 > juce::MathConstants<float>::twoPi) lfoPhase1 -= juce::MathConstants<float>::twoPi;
                if (lfoPhase2 > juce::MathConstants<float>::twoPi) lfoPhase2 -= juce::MathConstants<float>::twoPi;
                const auto exc1 = 12.0f * k * std::sin (lfoPhase1);
                const auto exc2 =  9.0f * k * std::sin (lfoPhase2);

                // ---- left half (fed by the right half's tail)
                {
                    auto t = x + feedR * decay;
                    t = apL1.allpass (t, 672.0f * k + exc1, 0.70f);
                    const auto d1 = delL1.step (t, 4453.0f * k);
                    dampL += dampCoef * (d1 - dampL);
                    auto u = dampL * decay;
                    u = apL2.allpass (u, 1800.0f * k, 0.50f);
                    feedL = delL2.step (u, 3720.0f * k);
                }

                // ---- right half (fed by the left half's tail)
                {
                    auto t = x + feedL * decay;
                    t = apR1.allpass (t, 908.0f * k + exc2, 0.70f);
                    const auto d1 = delR1.step (t, 4217.0f * k);
                    dampR += dampCoef * (d1 - dampR);
                    auto u = dampR * decay;
                    u = apR2.allpass (u, 2656.0f * k, 0.50f);
                    feedR = delR2.step (u, 3163.0f * k);
                }

                // ---- output taps (paper's table, x0.6)
                const auto yL = 0.6f * (  delR1.read (266.0f  * k)
                                        + delR1.read (2974.0f * k)
                                        - apR2.read  (1913.0f * k)
                                        + delR2.read (1996.0f * k)
                                        - delL1.read (1990.0f * k)
                                        - apL2.read  (187.0f  * k)
                                        - delL2.read (1066.0f * k));

                const auto yR = 0.6f * (  delL1.read (353.0f  * k)
                                        + delL1.read (3627.0f * k)
                                        - apL2.read  (1228.0f * k)
                                        + delL2.read (2673.0f * k)
                                        - delR1.read (2111.0f * k)
                                        - apR2.read  (335.0f  * k)
                                        - delR2.read (121.0f  * k));

                // ---- width (mid/side) + mix
                const auto width = widthSmooth.getNextValue();
                const auto mid   = 0.5f * (yL + yR);
                const auto side  = 0.5f * (yL - yR) * width;
                const auto wetL  = mid + side;
                const auto wetR  = mid - side;

                const auto mix = mixSmooth.getNextValue();
                l[i] = inL * (1.0f - mix) + wetL * mix;
                if (r != nullptr)
                    r[i] = inR * (1.0f - mix) + wetR * mix;
            }
        }

    private:
        // Fractional circular buffer. push() then read(L) returns x[n-L];
        // step()/allpass() combine the pre-push read with the push so the
        // effective length is exactly L (fractional, linear interp).
        struct DLine
        {
            void alloc (int maxLen)
            {
                int po2 = 1;
                while (po2 < maxLen + 4)
                    po2 <<= 1;
                buf.assign ((size_t) po2, 0.0f);
                mask = po2 - 1;
                w = 0;
            }

            void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }

            void push (float x) noexcept
            {
                w = (w + 1) & mask;
                buf[(size_t) w] = x;
            }

            // Value the line will hold at delay L *after* this sample's push.
            float readPrePush (float L) const noexcept
            {
                return interp ((float) w + 1.0f - L);
            }

            // Post-push read at delay L (used for the output taps).
            float read (float L) const noexcept
            {
                return interp ((float) w - L);
            }

            float step (float x, float L) noexcept
            {
                const auto out = readPrePush (L);
                push (x);
                return out;
            }

            float allpass (float x, float L, float g) noexcept
            {
                const auto delayed = readPrePush (L);
                const auto v = x - g * delayed;
                push (v);
                return delayed + g * v;
            }

            float interp (float pos) const noexcept
            {
                const auto ip = (int) std::floor (pos);
                const auto frac = pos - (float) ip;
                const auto a = buf[(size_t) (ip & mask)];
                const auto b = buf[(size_t) ((ip + 1) & mask)];
                return a + (b - a) * frac;
            }

            std::vector<float> buf;
            int mask = 0, w = 0;
        };

        static constexpr float inApLen[4] { 142.0f, 107.0f, 379.0f, 277.0f };
        static constexpr float inApG[4]   { 0.750f, 0.750f, 0.625f, 0.625f };

        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> preDelayLine { 48000 };
        std::array<DLine, 4> inAp;
        DLine apL1, delL1, apL2, delL2;
        DLine apR1, delR1, apR2, delR2;

        juce::SmoothedValue<float> sizeSmooth, preDelaySmooth, widthSmooth, mixSmooth;

        double sr = 44100.0, kBase = 44100.0 / 29761.0;
        float rt60 = 2.0f, dampCoef = 0.5f;
        float bwState = 0.0f, dampL = 0.0f, dampR = 0.0f;
        float feedL = 0.0f, feedR = 0.0f;
        float lfoPhase1 = 0.0f, lfoPhase2 = 0.0f;
    };
}
