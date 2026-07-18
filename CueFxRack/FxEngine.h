#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"
#include "DSP/ApiEQ.h"
#include "DSP/ApiCompressor.h"
#include "DSP/ApiLimiter.h"
#include "DSP/CueDelay.h"
#include "DSP/CueReverb.h"
#include "DSP/CueCrusher.h"
#include "DSP/CueImager.h"
#include "DSP/CueModulation.h"
#include "DSP/CueAmp.h"

namespace cue
{
    // ========================================================================
    // FxEngine — CUERACK's master chain, embedded in the CUESAMPLER.
    //
    // Identical processing order and module DSP as the rack's PluginProcessor:
    //   EQ > COMP > CRUSH > AMP > CHORUS > FLANGER > FLANGUS
    //      > TAPE DELAY > REVERB > IMAGER > LIMITER
    // (No Halftime module — the sampler's transport HALF TIME owns that job —
    //  and no master trims: the sampler's own gain staging applies.)
    //
    // Soft bypass: every module (except the limiter, which has its own
    // latency-preserving bypass path) crossfades in/out over ~10 ms on its
    // ON toggle instead of hard-switching, and is reset when re-enabled so
    // stale delay/reverb tails from minutes ago never leak back in.
    //
    // The limiter's lookahead delay keeps running while bypassed, so host
    // latency stays constant — report getLatencySamples() once at prepare.
    // ========================================================================
    class FxEngine
    {
    public:
        explicit FxEngine (juce::AudioProcessorValueTreeState& state) : apvts (state) {}

        void prepare (double sampleRate, int samplesPerBlock, int numChannels)
        {
            const auto numCh = juce::jmax (1, numChannels);
            const juce::dsp::ProcessSpec spec { sampleRate,
                                                (juce::uint32) samplesPerBlock,
                                                (juce::uint32) numCh };

            eq.prepare        (sampleRate, numCh);
            comp.prepare      (spec);
            crusher.prepare   (spec);
            amp.prepare       (spec);
            chorusFx.prepare  (spec);
            flangerFx.prepare (spec);
            flangusFx.prepare (spec);
            delay.prepare     (spec);
            reverbFx.prepare  (spec);
            imager.prepare    (sampleRate, numCh);
            limiter.prepare   (spec);

            dryScratch.setSize (numCh, juce::jmax (1, samplesPerBlock));
            fadeStep = (float) (1.0 / (0.010 * juce::jmax (1.0, sampleRate)));   // ~10 ms
            for (auto* t : { &eqT, &compT, &crushT, &ampT, &chT, &flT, &fgT, &dlyT, &revT, &imgT })
                *t = {};
        }

        int getLatencySamples() const { return limiter.getLatencySamples(); }

        // Same block body as CueRackAudioProcessor::processBlock (bpm comes
        // from the sampler's host-transport poll).
        void process (juce::AudioBuffer<float>& buffer, double bpm)
        {
            const auto n = buffer.getNumSamples();
            if (n == 0)
                return;

            using namespace cue::pid;

            // ---- EQ (550B-style, proportional Q) ---------------------------
            if (const bool on = pv (eqOn) > 0.5f; gate (eqT, on, [this] { eq.reset(); }))
            {
                const int   freqIdx[4] { (int) pv (eqB1Freq), (int) pv (eqB2Freq),
                                         (int) pv (eqB3Freq), (int) pv (eqB4Freq) };
                const float gains[4]   { pv (eqB1Gain), pv (eqB2Gain), pv (eqB3Gain), pv (eqB4Gain) };
                eq.setParams (freqIdx, gains, pv (eqLfShelf) > 0.5f, pv (eqHfShelf) > 0.5f);
                runSoft (eqT, on, buffer, [this] (auto& b) { eq.process (b); });
            }

            // ---- Compressor (2500-style) -----------------------------------
            if (const bool on = pv (compOn) > 0.5f; gate (compT, on, [this] { comp.reset(); }))
            {
                comp.setParams (pv (compThresh), (int) pv (compRatio), (int) pv (compAttack),
                                pv (compRelease), (int) pv (compKnee), (int) pv (compThrust),
                                pv (compType) > 0.5f, pv (compMakeup), pv (compMix) * 0.01f);
                runSoft (compT, on, buffer, [this] (auto& b) { comp.process (b); });
            }

            // ---- Bit crusher -----------------------------------------------
            if (const bool on = pv (crushOn) > 0.5f; gate (crushT, on, [this] { crusher.reset(); }))
            {
                crusher.setParams (pv (crushBits), (int) pv (crushRate), pv (crushDrive),
                                   pv (crushMix) * 0.01f);
                runSoft (crushT, on, buffer, [this] (auto& b) { crusher.process (b); });
            }

            // ---- Guitar amp ------------------------------------------------
            if (const bool on = pv (ampOn) > 0.5f; gate (ampT, on, [this] { amp.reset(); }))
            {
                amp.setParams ((int) pv (ampPreset), pv (ampDrive), pv (ampBass), pv (ampMid),
                               pv (ampTreble), pv (ampLevel), pv (ampMix) * 0.01f);
                runSoft (ampT, on, buffer, [this] (auto& b) { amp.process (b); });
            }

            // ---- Modulation trio -------------------------------------------
            if (const bool on = pv (chOn) > 0.5f; gate (chT, on, [this] { chorusFx.reset(); }))
            {
                chorusFx.setParams (pv (chRate), pv (chDepth) * 0.01f, pv (chDelay),
                                    pv (chFeedback) * 0.01f, pv (chMix) * 0.01f);
                runSoft (chT, on, buffer, [this] (auto& b) { chorusFx.process (b); });
            }

            if (const bool on = pv (flOn) > 0.5f; gate (flT, on, [this] { flangerFx.reset(); }))
            {
                flangerFx.setParams (pv (flRate), pv (flDepth) * 0.01f,
                                     pv (flFeedback) * 0.01f, pv (flMix) * 0.01f);
                runSoft (flT, on, buffer, [this] (auto& b) { flangerFx.process (b); });
            }

            if (const bool on = pv (fgOn) > 0.5f; gate (fgT, on, [this] { flangusFx.reset(); }))
            {
                flangusFx.setParams (pv (fgRate), pv (fgDepth) * 0.01f,
                                     pv (fgSpread) * 0.01f, pv (fgMix) * 0.01f);
                runSoft (fgT, on, buffer, [this] (auto& b) { flangusFx.process (b); });
            }

            // ---- Tape delay ------------------------------------------------
            if (const bool on = pv (dlyOn) > 0.5f; gate (dlyT, on, [this] { delay.reset(); }))
            {
                delay.setParams (pv (dlyTime), pv (dlySync) > 0.5f, (int) pv (dlyDiv), bpm,
                                 pv (dlyFeedback) * 0.01f, pv (dlyTone),
                                 pv (dlyPingPong) > 0.5f, pv (dlyMix) * 0.01f);
                runSoft (dlyT, on, buffer, [this] (auto& b) { delay.process (b); });
            }

            // ---- Reverb ----------------------------------------------------
            if (const bool on = pv (revOn) > 0.5f; gate (revT, on, [this] { reverbFx.reset(); }))
            {
                reverbFx.setParams (pv (revSize) * 0.01f, pv (revDecay), pv (revDamp) * 0.01f,
                                    pv (revPredelay), pv (revWidth) * 0.01f, pv (revMix) * 0.01f);
                runSoft (revT, on, buffer, [this] (auto& b) { reverbFx.process (b); });
            }

            // ---- Stereo imager ---------------------------------------------
            if (const bool on = pv (imgOn) > 0.5f; gate (imgT, on, [this] { imager.reset(); }))
            {
                imager.setParams (pv (imgWidth) * 0.01f, pv (imgBassMono) > 0.5f, pv (imgXover));
                runSoft (imgT, on, buffer, [this] (auto& b) { imager.process (b); });
            }

            // ---- Limiter (delay always runs -> constant host latency) ------
            limiter.setParams (pv (limGain), pv (limCeiling), pv (limRelease),
                               pv (limAutoRel) > 0.5f, pv (limTruePeak) > 0.5f);
            if (pv (limOn) > 0.5f)
                limiter.process (buffer);
            else
                limiter.processBypassed (buffer);

            // ---- imager scope tap (mid/side peaks, from the rack) ----------
            if (buffer.getNumChannels() > 1)
            {
                auto* l = buffer.getReadPointer (0);
                auto* r = buffer.getReadPointer (1);
                float m = 0.0f, s = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    m = juce::jmax (m, std::abs (l[i] + r[i]) * 0.5f);
                    s = juce::jmax (s, std::abs (l[i] - r[i]) * 0.5f);
                }
                midLevel.store (m);
                sideLevel.store (s);
            }
            else
            {
                midLevel.store (buffer.getMagnitude (0, n));
                sideLevel.store (0.0f);
            }
        }

        // ---- UI hooks (same shapes as the rack processor's) ----------------
        float getCompGainReductionDb() const   { return comp.getGainReductionDb(); }
        int   getLimiterNumBands() const       { return limiter.getNumBands(); }
        float getLimiterBandGRDb (int b) const { return limiter.getBandGainReductionDb (b); }
        float getMidLevel() const              { return midLevel.load(); }
        float getSideLevel() const             { return sideLevel.load(); }

    private:
        float pv (const char* paramID) const
        {
            if (auto* raw = apvts.getRawParameterValue (paramID))
                return raw->load();
            return 0.0f;
        }

        // ---- soft-bypass plumbing ------------------------------------------
        struct SoftToggle
        {
            float fade  = 0.0f;   // 0 = fully bypassed, 1 = fully engaged
            bool  wasOn = false;
        };

        // True when the module must run this block (on, or still fading out).
        // On a rising edge the module is reset first, so a re-enabled delay or
        // reverb starts clean instead of replaying a stale tail.
        template <typename ResetFn>
        bool gate (SoftToggle& t, bool on, ResetFn&& resetModule)
        {
            if (on && ! t.wasOn && t.fade <= 0.0001f)
                resetModule();
            t.wasOn = on;
            return on || t.fade > 0.0001f;
        }

        template <typename RunFn>
        void runSoft (SoftToggle& t, bool on, juce::AudioBuffer<float>& buffer, RunFn&& run)
        {
            // Steady state: fully engaged — no copy, no blend.
            if (on && t.fade >= 1.0f)
            {
                run (buffer);
                return;
            }

            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (dryScratch.getNumChannels(), buffer.getNumChannels());

            for (int ch = 0; ch < nCh; ++ch)
                dryScratch.copyFrom (ch, 0, buffer, ch, 0, n);

            run (buffer);

            auto endFade = t.fade;
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* wet = buffer.getWritePointer (ch);
                const auto* dry = dryScratch.getReadPointer (ch);
                auto f = t.fade;                        // identical ramp per channel
                for (int i = 0; i < n; ++i)
                {
                    f = juce::jlimit (0.0f, 1.0f, f + (on ? fadeStep : -fadeStep));
                    wet[i] = dry[i] * (1.0f - f) + wet[i] * f;
                }
                endFade = f;
            }
            t.fade = endFade;
        }

        juce::AudioProcessorValueTreeState& apvts;

        cue::dsp::ApiEQ         eq;
        cue::dsp::ApiCompressor comp;
        cue::dsp::CueCrusher    crusher;
        cue::dsp::CueAmp        amp;
        cue::dsp::CueChorus     chorusFx;
        cue::dsp::CueFlanger    flangerFx;
        cue::dsp::CueFlangus    flangusFx;
        cue::dsp::CueDelay      delay;
        cue::dsp::CueReverb     reverbFx;
        cue::dsp::CueImager     imager;
        cue::dsp::ApiLimiter    limiter;

        juce::AudioBuffer<float> dryScratch;
        float fadeStep = 0.005f;
        SoftToggle eqT, compT, crushT, ampT, chT, flT, fgT, dlyT, revT, imgT;

        std::atomic<float> midLevel { 0.0f }, sideLevel { 0.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxEngine)
    };
}
