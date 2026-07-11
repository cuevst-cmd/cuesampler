#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include "IronStage.h"

namespace cue::dsp
{
    // ========================================================================
    // ApiCompressor — API 2500-style stereo bus compressor, mastering grade.
    //
    // Architecture follows the hardware as documented by API and modelled by
    // the UAD / Waves emulations (docs/API_SOUND_RESEARCH.md):
    //
    //  - RMS detection per channel (the 2500 always uses per-channel RMS
    //    detectors), combined at 100% link for bus/mastering use
    //  - NEW  = feed-forward RMS detection (modern, precise)
    //    OLD  = feedback, peak detection (API 525 heritage): the detector
    //           reads input x previous gain — the pre-makeup feedback tap
    //  - Thrust(R): patented sidechain filter with a ~10 dB/decade rising
    //    tilt (the inverse of the pink-noise energy curve) ahead of the
    //    detector. NORM = off, MED = half tilt, LOUD = full tilt.
    //    Realised as cascaded one-pole/one-zero tilt sections, gain-
    //    normalised at 1 kHz so the threshold calibration doesn't move.
    //  - Knees: HARD = immediate ratio, MED = slight fade-in, SOFT =
    //    "over-easy" gradual approach (quadratic knee, 0 / 4 / 10 dB wide)
    //  - Ratios 1.5:1 ... 10:1 plus infinity:1 ("LIM"), like the hardware
    //  - Gain smoothing in the log domain: branching attack/release one-pole
    //    on the gain-reduction signal (the standard clean digital topology)
    //  - Parallel MIX blends the untouched dry signal with the compressed,
    //    makeup-driven, IronStage-coloured wet path
    // ========================================================================
    class ApiCompressor
    {
    public:
        //==================================================================
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sr = spec.sampleRate;

            iron.prepare (spec);
            iron.setAmount (0.22f);

            dryBuffer.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);

            for (auto& t : thrustLow)   t.prepare (sr, 40.0, 400.0);      // ~20 dB/dec across 40-400
            for (auto& t : thrustHigh)  t.prepare (sr, 1600.0, 14000.0);  // ~20 dB/dec across 1.6k-14k

            rmsCoef = timeCoef (0.005f);   // ~5 ms RMS integration window

            makeupSmooth.reset (sr, 0.03);
            mixSmooth.reset (sr, 0.03);

            reset();
        }

        void reset()
        {
            for (auto& s : rmsState) s = 0.0f;
            for (auto& t : thrustLow)  t.reset();
            for (auto& t : thrustHigh) t.reset();
            grSmoothDb = 0.0f;
            prevGain   = 1.0f;
            iron.reset();
            grDbAtomic.store (0.0f);
        }

        //==================================================================
        void setParams (float threshDb_, int ratioIdx, int attackIdx, float releaseSec,
                        int kneeIdx, int thrustIdx, bool feedbackMode, float makeupDb, float mix01)
        {
            // hardware ladders (LIM = infinity:1)
            static const float ratioSlopes[7] { 1.0f / 1.5f - 1.0f, 1.0f / 2.0f - 1.0f,
                                                1.0f / 3.0f - 1.0f, 1.0f / 4.0f - 1.0f,
                                                1.0f / 6.0f - 1.0f, 1.0f / 10.0f - 1.0f,
                                                -1.0f };
            static const float attacksMs[7]  { 0.03f, 0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f };
            static const float kneesDb[3]    { 0.0f, 4.0f, 10.0f };   // HARD / MED / SOFT (over-easy)

            threshDb  = threshDb_;
            slope     = ratioSlopes[juce::jlimit (0, 6, ratioIdx)];
            aAtk      = timeCoef (attacksMs[juce::jlimit (0, 6, attackIdx)] * 0.001f);
            aRel      = timeCoef (juce::jmax (0.01f, releaseSec));
            kneeDb    = kneesDb[juce::jlimit (0, 2, kneeIdx)];
            thrustMode = juce::jlimit (0, 2, thrustIdx);
            feedback  = feedbackMode;

            makeupSmooth.setTargetValue (juce::Decibels::decibelsToGain (makeupDb));
            mixSmooth.setTargetValue (juce::jlimit (0.0f, 1.0f, mix01));
        }

        //==================================================================
        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());

            dryBuffer.makeCopyOf (buffer, true);

            auto* l = buffer.getWritePointer (0);
            auto* r = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            float blockMinGrDb = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                // ---- sidechain source: FF reads input, FB reads input x prev gain
                const auto srcL = feedback ? l[i] * prevGain : l[i];
                const auto srcR = r != nullptr ? (feedback ? r[i] * prevGain : r[i]) : srcL;

                // ---- Thrust tilt, then per-channel detection
                const auto scL = thrust (0, srcL);
                const auto scR = thrust (1, srcR);

                float detDbL, detDbR;
                if (feedback)
                {
                    // OLD: peak detection (ballistics come from the GR smoother)
                    detDbL = juce::Decibels::gainToDecibels (std::abs (scL), -96.0f);
                    detDbR = juce::Decibels::gainToDecibels (std::abs (scR), -96.0f);
                }
                else
                {
                    // NEW: per-channel RMS (mean-square one-pole, dB of RMS)
                    rmsState[0] += rmsCoef * (scL * scL - rmsState[0]);
                    rmsState[1] += rmsCoef * (scR * scR - rmsState[1]);
                    detDbL = 10.0f * std::log10 (rmsState[0] + 1.0e-12f);
                    detDbR = 10.0f * std::log10 (rmsState[1] + 1.0e-12f);
                }

                const auto detDb = juce::jmax (detDbL, detDbR);   // 100% link

                // ---- gain computer with quadratic (over-easy) knee
                const auto over = detDb - threshDb;
                float grTargetDb = 0.0f;

                if (kneeDb > 0.0f && over > -kneeDb * 0.5f && over < kneeDb * 0.5f)
                {
                    const auto t = over + kneeDb * 0.5f;
                    grTargetDb = slope * t * t / (2.0f * kneeDb);
                }
                else if (over >= kneeDb * 0.5f)
                {
                    grTargetDb = slope * over;
                }

                // ---- log-domain ballistics: attack toward more reduction
                grSmoothDb += (grTargetDb < grSmoothDb ? aAtk : aRel) * (grTargetDb - grSmoothDb);
                blockMinGrDb = juce::jmin (blockMinGrDb, grSmoothDb);

                const auto gain = juce::Decibels::decibelsToGain (grSmoothDb);
                prevGain = gain;                       // feedback tap (pre-makeup)

                // ---- wet path: gain, then makeup driving the iron
                const auto makeup = makeupSmooth.getNextValue();
                l[i] *= gain * makeup;
                if (r != nullptr)
                    r[i] *= gain * makeup;
            }

            grDbAtomic.store (blockMinGrDb);

            // 2520 + transformer colour on the wet path only
            iron.process (buffer);

            // ---- parallel mix against the untouched dry signal
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* wet = buffer.getWritePointer (ch);
                auto* dry = dryBuffer.getReadPointer (ch);
                auto  mixCopy = mixSmooth;             // identical ramp per channel

                for (int i = 0; i < n; ++i)
                {
                    const auto m = mixCopy.getNextValue();
                    wet[i] = dry[i] * (1.0f - m) + wet[i] * m;
                }

                if (ch == nCh - 1)
                    mixSmooth = mixCopy;               // keep the real state advanced
            }
        }

        float getGainReductionDb() const noexcept   { return grDbAtomic.load(); }

    private:
        //==================================================================
        // One-pole/one-zero tilt section: analog (s + wz) / (s + wp), wp = 10 wz,
        // bilinear-transformed and gain-normalised at 1 kHz. One section gives
        // ~20 dB/decade between its corners; used at half/full strength this
        // approximates the Thrust filter's 10 dB/decade average tilt.
        struct TiltSection
        {
            void prepare (double sampleRate, double fz, double fp)
            {
                const auto wz = juce::MathConstants<double>::twoPi * fz;
                const auto wp = juce::MathConstants<double>::twoPi * fp;
                const auto k  = 2.0 * sampleRate;

                const auto norm1k = std::sqrt ((std::pow (juce::MathConstants<double>::twoPi * 1000.0, 2.0) + wz * wz)
                                             / (std::pow (juce::MathConstants<double>::twoPi * 1000.0, 2.0) + wp * wp));

                const auto d = k + wp;
                b0 = (float) (((k + wz) / d) / norm1k);
                b1 = (float) (((wz - k) / d) / norm1k);
                a1 = (float) ((wp - k) / d);
                reset();
            }

            void reset() noexcept   { x1 = y1 = 0.0f; }

            float process (float x) noexcept
            {
                const auto y = b0 * x + b1 * x1 - a1 * y1;
                x1 = x;
                y1 = y;
                return y;
            }

            float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        };

        float thrust (int ch, float x) noexcept
        {
            if (thrustMode == 0)
                return x;                               // NORM: flat sidechain

            auto y = thrustLow[(size_t) ch].process (x);   // MED: half tilt
            if (thrustMode == 2)
                y = thrustHigh[(size_t) ch].process (y);   // LOUD: full 10 dB/dec
            return y;
        }

        float timeCoef (float seconds) const noexcept
        {
            return 1.0f - std::exp (-1.0f / ((float) sr * juce::jmax (1.0e-5f, seconds)));
        }

        //==================================================================
        IronStage iron;
        juce::AudioBuffer<float> dryBuffer;
        juce::SmoothedValue<float> makeupSmooth, mixSmooth;

        std::array<TiltSection, 2> thrustLow, thrustHigh;
        std::array<float, 2> rmsState {};

        double sr = 44100.0;
        float threshDb = 0.0f, slope = -2.0f / 3.0f;   // 3:1
        float aAtk = 0.01f, aRel = 0.001f, kneeDb = 4.0f, rmsCoef = 0.01f;
        int thrustMode = 0;
        bool feedback = false;

        float grSmoothDb = 0.0f, prevGain = 1.0f;
        std::atomic<float> grDbAtomic { 0.0f };
    };
}
