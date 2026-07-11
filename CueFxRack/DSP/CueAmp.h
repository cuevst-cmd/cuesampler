#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

namespace cue::dsp
{
    // ========================================================================
    // CueAmp — guitar amplifier with six voicings after famous amps.
    //
    // Per channel: input voicing high-pass -> 2x-oversampled asymmetric tanh
    // stage (bias = tube asymmetry, even harmonics) -> DC blocker -> tone
    // stack (bass shelf 110 Hz, mid peak 650 Hz, treble shelf 3.2 kHz) ->
    // preset bright shelf -> cab low-pass -> level, parallel MIX.
    //
    // Voicings: JZ CLEAN (pristine, headroom), TWEED 57 (warm sag), BASS 59
    // (big lows), BRIT PLEXI (aggressive mids), BRIT AC (chimey top),
    // RECTO (tight scoop, high gain).
    // ========================================================================
    class CueAmp
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sr = spec.sampleRate;
            numChannels = juce::jmin (2, (int) spec.numChannels);

            oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
                              spec.numChannels, 1,
                              juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                              true, false);
            oversampler->initProcessing (spec.maximumBlockSize);

            dryBuffer.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);
            levelSmooth.reset (sr, 0.03);
            mixSmooth.reset (sr, 0.03);

            curPreset = -1;                        // force voicing rebuild
            curBass = curMid = curTreble = -99.0f;
            reset();
        }

        void reset()
        {
            if (oversampler != nullptr)
                oversampler->reset();
            for (auto& f : preHp)   f.reset();
            for (auto& f : bass)    f.reset();
            for (auto& f : mid)     f.reset();
            for (auto& f : treble)  f.reset();
            for (auto& f : bright)  f.reset();
            for (auto& f : cab)     f.reset();
            dcX.fill (0.0f);
            dcY.fill (0.0f);
        }

        void setParams (int presetIdx, float driveDb, float bassDb, float midDb,
                        float trebleDb, float levelDb, float mix01)
        {
            presetIdx = juce::jlimit (0, 5, presetIdx);
            if (presetIdx != curPreset)
            {
                curPreset = presetIdx;
                applyVoicing();
            }

            if (! juce::approximatelyEqual (bassDb, curBass)
             || ! juce::approximatelyEqual (midDb, curMid)
             || ! juce::approximatelyEqual (trebleDb, curTreble))
            {
                curBass = bassDb; curMid = midDb; curTreble = trebleDb;
                for (auto& f : bass)
                    f.setCoefficients (juce::IIRCoefficients::makeLowShelf (sr, 110.0, 0.707,
                                       juce::Decibels::decibelsToGain (bassDb)));
                for (auto& f : mid)
                    f.setCoefficients (juce::IIRCoefficients::makePeakFilter (sr, 650.0, 0.8,
                                       juce::Decibels::decibelsToGain (midDb)));
                for (auto& f : treble)
                    f.setCoefficients (juce::IIRCoefficients::makeHighShelf (sr, 3200.0, 0.707,
                                       juce::Decibels::decibelsToGain (trebleDb)));
            }

            const auto& v = voicings()[(size_t) curPreset];
            drive = juce::Decibels::decibelsToGain (driveDb) * v.driveScale;
            bias  = v.bias;
            shapeComp = 1.0f / juce::jmax (0.25f, std::tanh (0.35f * juce::jmax (1.0f, drive)));

            levelSmooth.setTargetValue (juce::Decibels::decibelsToGain (levelDb));
            mixSmooth.setTargetValue (juce::jlimit (0.0f, 1.0f, mix01));
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n   = buffer.getNumSamples();
            const auto nCh = juce::jmin (numChannels, buffer.getNumChannels());

            dryBuffer.makeCopyOf (buffer, true);

            for (int ch = 0; ch < nCh; ++ch)
                preHp[(size_t) ch].processSamples (buffer.getWritePointer (ch), n);

            // ---- oversampled tube stage
            juce::dsp::AudioBlock<float> block (buffer);
            auto up = oversampler->processSamplesUp (block);

            const auto tb = std::tanh (bias);
            for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
            {
                auto* d = up.getChannelPointer (ch);
                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    d[i] = (std::tanh (drive * d[i] + bias) - tb) * shapeComp;
            }
            oversampler->processSamplesDown (block);

            // ---- tone stack + cab + DC block
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                const auto c = (size_t) ch;

                bass[c].processSamples (d, n);
                mid[c].processSamples (d, n);
                treble[c].processSamples (d, n);
                bright[c].processSamples (d, n);
                cab[c].processSamples (d, n);

                auto& x1 = dcX[c];
                auto& y1 = dcY[c];
                for (int i = 0; i < n; ++i)
                {
                    y1 = d[i] - x1 + 0.995f * y1;
                    x1 = d[i];
                    d[i] = y1;
                }
            }

            // ---- level + parallel mix
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* wet = buffer.getWritePointer (ch);
                auto* dry = dryBuffer.getReadPointer (ch);
                auto lvl = levelSmooth;
                auto mx  = mixSmooth;

                for (int i = 0; i < n; ++i)
                {
                    const auto m = mx.getNextValue();
                    wet[i] = dry[i] * (1.0f - m) + wet[i] * lvl.getNextValue() * m;
                }

                if (ch == nCh - 1)
                {
                    levelSmooth = lvl;
                    mixSmooth = mx;
                }
            }
        }

    private:
        struct Voicing
        {
            double preHpHz, cabHz, brightHz;
            float brightDb, driveScale, bias;
        };

        static const std::array<Voicing, 6>& voicings()
        {
            //                                  preHP   cab   brightF  brDb  drive  bias
            static const std::array<Voicing, 6> v { { {  40.0, 7000.0, 3000.0,  2.0f, 0.6f, 0.02f },   // JZ CLEAN
                                                      {  70.0, 5000.0, 2800.0,  1.5f, 1.0f, 0.15f },   // TWEED 57
                                                      {  45.0, 4200.0, 2500.0,  0.5f, 0.9f, 0.10f },   // BASS 59
                                                      {  90.0, 4800.0, 3000.0,  3.0f, 1.5f, 0.20f },   // BRIT PLEXI
                                                      {  80.0, 6500.0, 3200.0,  4.0f, 1.2f, 0.28f },   // BRIT AC
                                                      {  60.0, 3800.0, 2400.0, -1.0f, 2.4f, 0.05f } } };// RECTO
            return v;
        }

        void applyVoicing()
        {
            const auto& v = voicings()[(size_t) curPreset];
            for (auto& f : preHp)
                f.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, v.preHpHz, 0.707));
            for (auto& f : bright)
                f.setCoefficients (juce::IIRCoefficients::makeHighShelf (sr, v.brightHz, 0.707,
                                   juce::Decibels::decibelsToGain (v.brightDb)));
            for (auto& f : cab)
                f.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, v.cabHz, 0.85));
        }

        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
        juce::AudioBuffer<float> dryBuffer;
        juce::SmoothedValue<float> levelSmooth, mixSmooth;

        std::array<juce::SingleThreadedIIRFilter, 2> preHp, bass, mid, treble, bright, cab;
        std::array<float, 2> dcX {}, dcY {};

        double sr = 44100.0;
        int numChannels = 2, curPreset = -1;
        float curBass = -99.0f, curMid = -99.0f, curTreble = -99.0f;
        float drive = 4.0f, bias = 0.15f, shapeComp = 1.0f;
    };
}
