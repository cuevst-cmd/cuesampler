#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace cue::dsp
{
    // ========================================================================
    // ApiEQ — 550B-inspired 4-band EQ.
    //
    //  - Proportional Q: bandwidth narrows as |gain| grows
    //    (broad/musical at +-2 dB, surgical at +-12 dB)
    //  - Reciprocal curves: RBJ peaking with matched Q, cut mirrors boost
    //  - Stepped frequency choices matching the hardware
    //  - LF/HF bands switchable to shelves
    //  - Per-band gain smoothing (~40 ms): knob sweeps re-derive the
    //    coefficients block-by-block along a ramped gain, so there is no
    //    zipper noise and no click when a band engages/disengages.
    // ========================================================================
    class ApiEQ
    {
    public:
        void prepare (double sampleRate, int numChannels)
        {
            sr  = sampleRate;
            nCh = juce::jmin (2, numChannels);
            for (int b = 0; b < 4; ++b)
            {
                gainSmooth[b].reset (sr, 0.04);
                gainSmooth[b].setCurrentAndTargetValue (0.0f);
                smoothedGain[b] = 0.0f;
            }
            reset();
            dirty = true;
        }

        void reset()
        {
            for (auto& bandFilters : filters)
                for (auto& f : bandFilters)
                    f.reset();
        }

        void setParams (const int freqIdx[4], const float gainDb[4], bool lfShelf, bool hfShelf)
        {
            for (int b = 0; b < 4; ++b)
            {
                if (curFreqIdx[b] != freqIdx[b])
                    dirty = true;
                curFreqIdx[b] = freqIdx[b];
                gainSmooth[b].setTargetValue (gainDb[b]);
            }
            if (curLfShelf != lfShelf || curHfShelf != hfShelf)
                dirty = true;
            curLfShelf = lfShelf;
            curHfShelf = hfShelf;
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            const auto n = buffer.getNumSamples();

            // Advance the gain ramps by one block and rebuild coefficients
            // whenever a ramp is still moving (cheap: four RBJ recomputes).
            for (int b = 0; b < 4; ++b)
            {
                gainSmooth[b].skip (n);
                const auto g = gainSmooth[b].getCurrentValue();
                if (std::abs (g - smoothedGain[b]) > 0.005f)
                {
                    smoothedGain[b] = g;
                    dirty = true;
                }
            }

            if (dirty)
                updateCoefficients();

            for (int b = 0; b < 4; ++b)
            {
                if (std::abs (smoothedGain[b]) < 0.05f)
                    continue;
                for (int ch = 0; ch < juce::jmin (nCh, buffer.getNumChannels()); ++ch)
                    filters[(size_t) b][(size_t) ch].processSamples (buffer.getWritePointer (ch), n);
            }
        }

    private:
        void updateCoefficients()
        {
            static const double freqTables[4][7] = {
                {   30.0,   40.0,   50.0,   100.0,   200.0,   300.0,   400.0 },   // LF  (MUD)
                {   75.0,  150.0,  180.0,   240.0,   500.0,   700.0,  1000.0 },   // LMF (BOXY)
                {  800.0, 1500.0, 2000.0,  3000.0,  4000.0,  5000.0,  6500.0 },   // HMF (BITE)
                { 2500.0, 5000.0, 7000.0, 10000.0, 12500.0, 15000.0, 20000.0 } }; // HF  (AIR)

            for (int b = 0; b < 4; ++b)
            {
                const auto f    = freqTables[b][juce::jlimit (0, 6, curFreqIdx[b])];
                const auto gLin = juce::Decibels::decibelsToGain (smoothedGain[b]);

                // Proportional Q: 0.45 at 0 dB -> ~2.45 at +-12 dB
                const auto q = 0.45 + 2.0 * (std::abs ((double) smoothedGain[b]) / 12.0);

                juce::IIRCoefficients coeffs;
                if (b == 0 && curLfShelf)
                    coeffs = juce::IIRCoefficients::makeLowShelf (sr, f, 0.707, gLin);
                else if (b == 3 && curHfShelf)
                    coeffs = juce::IIRCoefficients::makeHighShelf (sr, juce::jmin (f, sr * 0.45), 0.707, gLin);
                else
                    coeffs = juce::IIRCoefficients::makePeakFilter (sr, juce::jmin (f, sr * 0.45), q, gLin);

                for (int ch = 0; ch < 2; ++ch)
                    filters[(size_t) b][(size_t) ch].setCoefficients (coeffs);
            }
            dirty = false;
        }

        juce::SingleThreadedIIRFilter filters[4][2];
        juce::SmoothedValue<float> gainSmooth[4];
        double sr = 44100.0;
        int nCh = 2;
        int curFreqIdx[4] { -1, -1, -1, -1 };
        float smoothedGain[4] { 0.0f, 0.0f, 0.0f, 0.0f };
        bool curLfShelf = false, curHfShelf = false, dirty = true;
    };
}
