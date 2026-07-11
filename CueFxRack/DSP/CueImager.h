#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace cue::dsp
{
    // ========================================================================
    // CueImager — mid/side stereo width with optional bass-mono:
    // below the crossover the side signal is removed (high-passed), keeping
    // the low end centred and tight.
    // ========================================================================
    class CueImager
    {
    public:
        void prepare (double sampleRate, int numChannels)
        {
            sr  = sampleRate;
            nCh = numChannels;
            widthSmooth.reset (sr, 0.05);
            widthSmooth.setCurrentAndTargetValue (1.0f);
            sideHP.reset();
            curXover = -1.0f;
        }

        void setParams (float width01to2, bool bassMono_, float xoverHz)
        {
            widthSmooth.setTargetValue (juce::jlimit (0.0f, 2.0f, width01to2));
            bassMono = bassMono_;

            if (! juce::approximatelyEqual (curXover, xoverHz))
            {
                curXover = xoverHz;
                sideHP.setCoefficients (juce::IIRCoefficients::makeHighPass (
                    sr, juce::jlimit (40.0, 600.0, (double) xoverHz), 0.707));
            }
        }

        void process (juce::AudioBuffer<float>& buffer)
        {
            if (buffer.getNumChannels() < 2)
                return;                                  // nothing to widen in mono

            const auto n = buffer.getNumSamples();
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);

            for (int i = 0; i < n; ++i)
            {
                const auto width = widthSmooth.getNextValue();

                auto mid  = 0.5f * (l[i] + r[i]);
                auto side = 0.5f * (l[i] - r[i]);

                if (bassMono)
                    side = sideHP.processSingleSampleRaw (side);

                side *= width;

                l[i] = mid + side;
                r[i] = mid - side;
            }
        }

    private:
        juce::SingleThreadedIIRFilter sideHP;
        juce::SmoothedValue<float> widthSmooth;
        double sr = 44100.0;
        int nCh = 2;
        float curXover = -1.0f;
        bool bassMono = false;
    };
}
