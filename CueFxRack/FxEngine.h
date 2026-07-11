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
            if (pv (eqOn) > 0.5f)
            {
                const int   freqIdx[4] { (int) pv (eqB1Freq), (int) pv (eqB2Freq),
                                         (int) pv (eqB3Freq), (int) pv (eqB4Freq) };
                const float gains[4]   { pv (eqB1Gain), pv (eqB2Gain), pv (eqB3Gain), pv (eqB4Gain) };
                eq.setParams (freqIdx, gains, pv (eqLfShelf) > 0.5f, pv (eqHfShelf) > 0.5f);
                eq.process (buffer);
            }

            // ---- Compressor (2500-style) -----------------------------------
            if (pv (compOn) > 0.5f)
            {
                comp.setParams (pv (compThresh), (int) pv (compRatio), (int) pv (compAttack),
                                pv (compRelease), (int) pv (compKnee), (int) pv (compThrust),
                                pv (compType) > 0.5f, pv (compMakeup), pv (compMix) * 0.01f);
                comp.process (buffer);
            }

            // ---- Bit crusher -----------------------------------------------
            if (pv (crushOn) > 0.5f)
            {
                crusher.setParams (pv (crushBits), (int) pv (crushRate), pv (crushDrive),
                                   pv (crushMix) * 0.01f);
                crusher.process (buffer);
            }

            // ---- Guitar amp ------------------------------------------------
            if (pv (ampOn) > 0.5f)
            {
                amp.setParams ((int) pv (ampPreset), pv (ampDrive), pv (ampBass), pv (ampMid),
                               pv (ampTreble), pv (ampLevel), pv (ampMix) * 0.01f);
                amp.process (buffer);
            }

            // ---- Modulation trio -------------------------------------------
            if (pv (chOn) > 0.5f)
            {
                chorusFx.setParams (pv (chRate), pv (chDepth) * 0.01f, pv (chDelay),
                                    pv (chFeedback) * 0.01f, pv (chMix) * 0.01f);
                chorusFx.process (buffer);
            }

            if (pv (flOn) > 0.5f)
            {
                flangerFx.setParams (pv (flRate), pv (flDepth) * 0.01f,
                                     pv (flFeedback) * 0.01f, pv (flMix) * 0.01f);
                flangerFx.process (buffer);
            }

            if (pv (fgOn) > 0.5f)
            {
                flangusFx.setParams (pv (fgRate), pv (fgDepth) * 0.01f,
                                     pv (fgSpread) * 0.01f, pv (fgMix) * 0.01f);
                flangusFx.process (buffer);
            }

            // ---- Tape delay ------------------------------------------------
            if (pv (dlyOn) > 0.5f)
            {
                delay.setParams (pv (dlyTime), pv (dlySync) > 0.5f, (int) pv (dlyDiv), bpm,
                                 pv (dlyFeedback) * 0.01f, pv (dlyTone),
                                 pv (dlyPingPong) > 0.5f, pv (dlyMix) * 0.01f);
                delay.process (buffer);
            }

            // ---- Reverb ----------------------------------------------------
            if (pv (revOn) > 0.5f)
            {
                reverbFx.setParams (pv (revSize) * 0.01f, pv (revDecay), pv (revDamp) * 0.01f,
                                    pv (revPredelay), pv (revWidth) * 0.01f, pv (revMix) * 0.01f);
                reverbFx.process (buffer);
            }

            // ---- Stereo imager ---------------------------------------------
            if (pv (imgOn) > 0.5f)
            {
                imager.setParams (pv (imgWidth) * 0.01f, pv (imgBassMono) > 0.5f, pv (imgXover));
                imager.process (buffer);
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

        std::atomic<float> midLevel { 0.0f }, sideLevel { 0.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxEngine)
    };
}
