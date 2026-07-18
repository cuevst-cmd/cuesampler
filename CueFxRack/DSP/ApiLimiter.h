#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include "IronStage.h"

namespace cue::dsp
{
    // ========================================================================
    // ApiLimiter — multiband maximizer in the spirit of the Waves L3.
    //
    // The L3 is defined by multiband peak limiting: it splits the spectrum
    // (five linear-phase bands + a "PLMixer" that decides per-band attenuation
    // psychoacoustically) so a loud low end can't pump the highs, then keeps a
    // single master ceiling with brickwall look-ahead limiting and an Automatic
    // Release Control (ARC) that reacts to peaks and RMS differently for more
    // loudness with less audible distortion.
    //
    // This emulation:
    //   - a subtle 2520/transformer drive (IronStage) preserves the CUE "API"
    //     colour on the way in (the GAIN knob pushes it)
    //   - a 4-band Linkwitz-Riley split (minimum-phase, allpass-compensated so
    //     the bands sum flat) — the true L3 uses linear-phase crossovers; this
    //     trades a little phase for zero reported filter latency
    //   - each band is limited to the ceiling by its own look-ahead brickwall
    //     with dual-stage / ARC release, so bands don't cross-pump
    //   - the summed output passes a final look-ahead brickwall at the ceiling,
    //     true-peak-aware (extra headroom so inter-sample peaks stay under)
    //
    // Reported latency = bandLookahead + finalLookahead (constant); the module
    // routes through processBypassed() when off so host latency never changes.
    // ========================================================================
    class ApiLimiter
    {
    public:
        static constexpr int kNumBands = 4;

        //==================================================================
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sr        = spec.sampleRate;
            maxBlock  = (int) spec.maximumBlockSize;
            numCh     = juce::jmax (1, (int) spec.numChannels);

            iron.prepare (spec);
            iron.setAmount (0.22f);

            bandLook  = juce::jmax (16, (int) std::round (sr * 0.002));    // 2 ms
            finalLook = juce::jmax (16, (int) std::round (sr * 0.0015));   // 1.5 ms
            arcDecay  = std::exp (-1.0f / (float) (sr * 0.05));            // ~50 ms

            for (auto& b : bands)
            {
                b.limiter.prepare (sr, bandLook, numCh, arcDecay);
                b.buffer.setSize (2, juce::jmax (1, maxBlock));
            }
            finalStage.prepare (sr, finalLook, numCh, arcDecay);

            // 3 Linkwitz-Riley crossovers -> 4 bands, + allpass compensation so
            // bands separated before a later crossover stay phase-aligned.
            const std::array<float, 3> fx { 110.0f, 750.0f, 4500.0f };
            for (int i = 0; i < 3; ++i)
            {
                xover[i].prepare (spec);
                xover[i].setType (juce::dsp::LinkwitzRileyFilterType::allpass); // (magnitude func only; we use dual output)
                xover[i].setCutoffFrequency (fx[(size_t) i]);
            }
            for (auto* ap : { &apf2_b0, &apf3_b0, &apf3_b1 })
                ap->prepare (spec);
            apf2_b0.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
            apf3_b0.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
            apf3_b1.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
            apf2_b0.setCutoffFrequency (fx[1]);
            apf3_b0.setCutoffFrequency (fx[2]);
            apf3_b1.setCutoffFrequency (fx[2]);

            bypassLine[0].assign ((size_t) (bandLook + finalLook), 0.0f);
            bypassLine[1].assign ((size_t) (bandLook + finalLook), 0.0f);

            reset();
        }

        void reset()
        {
            for (auto& b : bands) { b.limiter.reset(); b.buffer.clear(); }
            finalStage.reset();
            iron.reset();
            for (auto& x : xover) x.reset();
            apf2_b0.reset(); apf3_b0.reset(); apf3_b1.reset();
            for (auto& d : bypassLine) std::fill (d.begin(), d.end(), 0.0f);
            bypassIdx = 0;
            grDbAtomic.store (0.0f);
        }

        int getLatencySamples() const noexcept   { return bandLook + finalLook; }

        //==================================================================
        void setParams (float gainDb, float ceilingDb, float releaseMs,
                        bool autoRelease = true, bool truePeak = true)
        {
            inGain  = juce::Decibels::decibelsToGain (gainDb);
            ceiling = juce::Decibels::decibelsToGain (ceilingDb);

            const auto relSec = juce::jmax (1.0f, releaseMs) * 0.001f;
            for (auto& b : bands)
            {
                b.limiter.setCeiling (ceiling);
                b.limiter.setRelease (relSec, autoRelease);
            }
            // True-peak: the final stage detects inter-sample peaks (half-sample
            // interpolated estimates) instead of eating a fixed 0.6 dB of
            // headroom, so TP mode no longer costs loudness on normal material.
            finalStage.setCeiling (ceiling);
            finalStage.setRelease (relSec, autoRelease);
            finalStage.setInterSamplePeakDetection (truePeak);
        }

        //==================================================================
        void processBypassed (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());
            const auto len = (int) bypassLine[0].size();

            for (int i = 0; i < n; ++i)
            {
                for (int ch = 0; ch < nCh; ++ch)
                {
                    auto* d = buffer.getWritePointer (ch);
                    const auto delayed = bypassLine[(size_t) ch][(size_t) bypassIdx];
                    bypassLine[(size_t) ch][(size_t) bypassIdx] = d[i];
                    d[i] = delayed;
                }
                bypassIdx = (bypassIdx + 1) % len;
            }
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (2, buffer.getNumChannels());

            // ---- drive + API colour (wideband, pre-split) --------------------
            buffer.applyGain (0, n, inGain);
            iron.process (buffer);

            // ---- split into bands (allpass-compensated LR4) ------------------
            for (int ch = 0; ch < nCh; ++ch)
            {
                const auto* in = buffer.getReadPointer (ch);
                auto* b0 = bands[0].buffer.getWritePointer (ch);
                auto* b1 = bands[1].buffer.getWritePointer (ch);
                auto* b2 = bands[2].buffer.getWritePointer (ch);
                auto* b3 = bands[3].buffer.getWritePointer (ch);

                for (int i = 0; i < n; ++i)
                {
                    float lo0, hi0, lo1, hi1, lo2, hi2;
                    xover[0].processSample (ch, in[i], lo0, hi0);
                    xover[1].processSample (ch, hi0,   lo1, hi1);
                    xover[2].processSample (ch, hi1,   lo2, hi2);

                    // phase-align lower bands to the later crossovers
                    float band0 = apf2_b0.processSample (ch, lo0);
                    band0       = apf3_b0.processSample (ch, band0);
                    const float band1 = apf3_b1.processSample (ch, lo1);

                    b0[i] = band0;
                    b1[i] = band1;
                    b2[i] = lo2;
                    b3[i] = hi2;
                }
            }

            // ---- per-band look-ahead limiting --------------------------------
            // Wrap the first n samples of each band buffer in a non-owning view
            // (capacity was allocated to maxBlock in prepare — no realloc here).
            float grMag = 0.0f;
            for (auto& b : bands)
            {
                juce::AudioBuffer<float> bandView (b.buffer.getArrayOfWritePointers(), nCh, n);
                b.limiter.process (bandView);
                grMag = juce::jmax (grMag, -b.limiter.getGainReductionDb());
            }

            // ---- sum bands back into the main buffer -------------------------
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* out = buffer.getWritePointer (ch);
                const auto* b0 = bands[0].buffer.getReadPointer (ch);
                const auto* b1 = bands[1].buffer.getReadPointer (ch);
                const auto* b2 = bands[2].buffer.getReadPointer (ch);
                const auto* b3 = bands[3].buffer.getReadPointer (ch);
                for (int i = 0; i < n; ++i)
                    out[i] = b0[i] + b1[i] + b2[i] + b3[i];
            }

            // ---- final wideband brickwall (true-peak-aware) ------------------
            finalStage.process (buffer);
            grMag = juce::jmax (grMag, -finalStage.getGainReductionDb());
            grDbAtomic.store (-grMag);
        }

        float getGainReductionDb() const noexcept          { return grDbAtomic.load(); }
        int   getNumBands() const noexcept                 { return kNumBands; }
        float getBandGainReductionDb (int b) const noexcept
        {
            return (b >= 0 && b < kNumBands) ? bands[(size_t) b].limiter.getGainReductionDb() : 0.0f;
        }

    private:
        //==================================================================
        // Monotonic queue: O(1) amortised sliding maximum over `horizon`.
        struct SlidingMax
        {
            void resize (int h)
            {
                horizon = juce::jmax (1, h);
                cap = horizon + 1;
                val.assign ((size_t) cap, 0.0f);
                idx.assign ((size_t) cap, 0);
                reset();
            }
            void reset() { head = tail = 0; count = 0; }

            void push (float v) noexcept
            {
                while (tail != head && val[(size_t) ((tail + cap - 1) % cap)] <= v)
                    tail = (tail + cap - 1) % cap;
                val[(size_t) tail] = v;
                idx[(size_t) tail] = count;
                tail = (tail + 1) % cap;
                if (idx[(size_t) head] <= count - horizon)
                    head = (head + 1) % cap;
                ++count;
            }
            float currentMax() const noexcept { return val[(size_t) head]; }

            std::vector<float> val;
            std::vector<juce::int64> idx;
            int horizon = 1, cap = 2, head = 0, tail = 0;
            juce::int64 count = 0;
        };

        //==================================================================
        // Look-ahead brickwall (the original CUE limiter core), reused for each
        // band and for the final wideband stage.
        struct Brickwall
        {
            void prepare (double sampleRate, int lookahead, int channels, float arcDecayCoef)
            {
                sr       = sampleRate;
                look     = juce::jmax (16, lookahead);
                nCh      = channels;
                arcDecay = arcDecayCoef;
                for (auto& d : delayLine) d.assign ((size_t) look, 0.0f);
                smax.resize (look);
                aAtk = timeCoef ((float) look / (float) sr / 5.0f);
                reset();
            }
            void reset()
            {
                for (auto& d : delayLine) std::fill (d.begin(), d.end(), 0.0f);
                smax.reset();
                writeIdx = 0;
                envFast = envSmooth = 1.0f;
                arcRamp = 0.0f;
                for (auto& h : ispHist) h.fill (0.0f);
                grDb.store (0.0f);
            }
            void setCeiling (float lin) noexcept
            {
                ceiling    = lin;
                detCeiling = lin * 0.983f;
            }
            void setRelease (float relSec, bool autoRelease) noexcept
            {
                arc      = autoRelease;
                aRelFast = timeCoef (relSec * 0.15f);
                aRelSlow = timeCoef (relSec * 1.4f);
            }
            void setInterSamplePeakDetection (bool shouldDetect) noexcept
            {
                ispDetect = shouldDetect;
            }

            // Half-sample Catmull-Rom estimate between h[1] and h[2] — the
            // standard cheap inter-sample-peak probe (equivalent to one 2x
            // polyphase phase: -1/16, 9/16, 9/16, -1/16).
            static float halfSamplePeak (const std::array<float, 4>& h) noexcept
            {
                return std::abs (-0.0625f * h[0] + 0.5625f * h[1] + 0.5625f * h[2] - 0.0625f * h[3]);
            }

            void process (juce::AudioBuffer<float>& buffer)
            {
                const auto n   = buffer.getNumSamples();
                const auto nc  = juce::jmin (nCh, buffer.getNumChannels());
                auto* l = buffer.getWritePointer (0);
                auto* r = nc > 1 ? buffer.getWritePointer (1) : nullptr;

                float minEnv = 1.0f;
                for (int i = 0; i < n; ++i)
                {
                    auto pk = juce::jmax (std::abs (l[i]), r != nullptr ? std::abs (r[i]) : 0.0f);

                    if (ispDetect)
                    {
                        // Fold in the estimated peak *between* samples. The
                        // detector runs `look` samples ahead of the delayed
                        // output, so the half-sample lag of the estimate is
                        // comfortably inside the look-ahead window.
                        auto& hl = ispHist[0];
                        hl[0] = hl[1]; hl[1] = hl[2]; hl[2] = hl[3]; hl[3] = l[i];
                        pk = juce::jmax (pk, halfSamplePeak (hl));
                        if (r != nullptr)
                        {
                            auto& hr = ispHist[1];
                            hr[0] = hr[1]; hr[1] = hr[2]; hr[2] = hr[3]; hr[3] = r[i];
                            pk = juce::jmax (pk, halfSamplePeak (hr));
                        }
                    }

                    smax.push (pk);

                    const auto winMax = smax.currentMax();
                    const auto target = winMax > detCeiling ? detCeiling / juce::jmax (1.0e-9f, winMax) : 1.0f;

                    if (target < envFast)
                    {
                        envFast += aAtk * (target - envFast);
                        arcRamp  = 1.0f;                                  // just caught a transient
                    }
                    else
                    {
                        // ARC: fast release right after a transient, easing to
                        // slow for sustained material; fixed blend when off.
                        const auto relCoef = arc ? (aRelFast * arcRamp + aRelSlow * (1.0f - arcRamp))
                                                 : (aRelFast * 0.35f + aRelSlow * 0.65f);
                        envFast += relCoef * (target - envFast);
                    }
                    arcRamp *= arcDecay;

                    envSmooth += juce::jmin (1.0f, aAtk * 1.6f) * (envFast - envSmooth);
                    minEnv = juce::jmin (minEnv, envSmooth);

                    const auto dL = delayLine[0][(size_t) writeIdx];
                    delayLine[0][(size_t) writeIdx] = l[i];
                    l[i] = juce::jlimit (-ceiling, ceiling, dL * envSmooth);
                    if (r != nullptr)
                    {
                        const auto dR = delayLine[1][(size_t) writeIdx];
                        delayLine[1][(size_t) writeIdx] = r[i];
                        r[i] = juce::jlimit (-ceiling, ceiling, dR * envSmooth);
                    }
                    writeIdx = (writeIdx + 1) % look;
                }
                grDb.store (juce::Decibels::gainToDecibels (minEnv, -80.0f));
            }

            float getGainReductionDb() const noexcept { return grDb.load(); }

            float timeCoef (float seconds) const noexcept
            {
                return 1.0f - std::exp (-1.0f / ((float) sr * juce::jmax (1.0e-5f, seconds)));
            }

            std::array<std::vector<float>, 2> delayLine;
            std::array<std::array<float, 4>, 2> ispHist {};
            SlidingMax smax;
            double sr = 44100.0;
            int look = 96, writeIdx = 0, nCh = 2;
            float ceiling = 1.0f, detCeiling = 0.983f;
            float aAtk = 0.1f, aRelFast = 0.01f, aRelSlow = 0.002f, arcDecay = 0.999f;
            float envFast = 1.0f, envSmooth = 1.0f, arcRamp = 0.0f;
            bool arc = true, ispDetect = false;
            std::atomic<float> grDb { 0.0f };
        };

        struct BandProc
        {
            Brickwall limiter;
            juce::AudioBuffer<float> buffer;
        };

        //==================================================================
        IronStage iron;
        std::array<BandProc, kNumBands> bands;
        Brickwall finalStage;
        std::array<juce::dsp::LinkwitzRileyFilter<float>, 3> xover;
        juce::dsp::LinkwitzRileyFilter<float> apf2_b0, apf3_b0, apf3_b1;

        std::array<std::vector<float>, 2> bypassLine;

        double sr = 44100.0;
        int maxBlock = 512, numCh = 2, bandLook = 96, finalLook = 72, bypassIdx = 0;
        float arcDecay = 0.999f;
        float inGain = 1.0f, ceiling = 1.0f;

        std::atomic<float> grDbAtomic { 0.0f };
    };
}
