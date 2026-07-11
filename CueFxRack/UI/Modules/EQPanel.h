#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // API 550B-inspired bands, API 560-style vertical sliders.
    // Each band is its own soft-hued zone, warm lows -> icy highs.
    class EQPanel final : public ModulePanel
    {
    public:
        explicit EQPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "EQ", pid::eqOn, colours::cueSand)
        {
            for (int i = 0; i < 4; ++i)
            {
                freqSliders[i]->setColour (juce::Slider::thumbColourId, bandHues[i]);
                gainSliders[i]->setColour (juce::Slider::thumbColourId, bandHues[i]);
                bandLabels[i]->setColour (juce::Label::textColourId, bandHues[i]);
            }
        }

    private:
        // CUE ramp within the EQ: hot lows -> warm-white highs
        inline static const juce::Colour bandHues[4] { colours::cueEmber,
                                                       colours::cueMid,
                                                       colours::cueSand,
                                                       colours::cueWhite };

        juce::Slider* freqSliders[4] { &addChoiceFader (pid::eqB1Freq, "FREQ", eqB1Freqs, " HZ"),
                                       &addChoiceFader (pid::eqB2Freq, "FREQ", eqB2Freqs, " HZ"),
                                       &addChoiceFader (pid::eqB3Freq, "FREQ", eqB3Freqs, " HZ"),
                                       &addChoiceFader (pid::eqB4Freq, "FREQ", eqB4Freqs, " HZ") };

        juce::Slider* gainSliders[4] { &addFader (pid::eqB1Gain, "GAIN", " DB"),
                                       &addFader (pid::eqB2Gain, "GAIN", " DB"),
                                       &addFader (pid::eqB3Gain, "GAIN", " DB"),
                                       &addFader (pid::eqB4Gain, "GAIN", " DB") };

        // Band names double as orb-language: each describes the frequency
        // range AND the alteration it makes to the CUE orb
        juce::Label* bandLabels[4] { &addStatic ("SWELL", 12.5f, true),
                                     &addStatic ("BODY", 12.5f, true),
                                     &addStatic ("EDGE", 12.5f, true),
                                     &addStatic ("SHIMMER", 12.5f, true) };

        juce::Label* rangeLabels[4] { &addStatic ("LF 30-400", 8.5f),
                                      &addStatic ("LMF 75-1K", 8.5f),
                                      &addStatic ("HMF 800-6K5", 8.5f),
                                      &addStatic ("HF 2K5-20K", 8.5f) };

        juce::ToggleButton& lfShelf { addToggle (pid::eqLfShelf, "LF SHELF") };
        juce::ToggleButton& hfShelf { addToggle (pid::eqHfShelf, "HF SHELF") };

        juce::Rectangle<int> bandRects[4];

        void layoutContent (juce::Rectangle<int> area) override
        {
            auto toggleRow = area.removeFromBottom (22);
            area.removeFromBottom (4);

            lfShelf.setBounds (toggleRow.removeFromLeft (toggleRow.getWidth() / 2).reduced (14, 0));
            hfShelf.setBounds (toggleRow.reduced (14, 0));

            const int colW = area.getWidth() / 4;
            for (int i = 0; i < 4; ++i)
            {
                auto col     = area.removeFromLeft (colW);
                bandRects[i] = col.reduced (3, 0);

                col.reduce (6, 4);
                bandLabels[i]->setBounds (col.removeFromTop (16));
                rangeLabels[i]->setBounds (col.removeFromTop (11));
                col.removeFromTop (2);

                auto left = col.removeFromLeft (col.getWidth() / 2);
                placeFader (*freqSliders[i], left);
                placeFader (*gainSliders[i], col);
            }
        }

        void paintContent (juce::Graphics& g, juce::Rectangle<int>) override
        {
            // One soft zone per band, sharp-edged, sitting under its controls
            for (int i = 0; i < 4; ++i)
            {
                if (bandRects[i].getWidth() < 30 || bandRects[i].getHeight() < 30)
                    continue;

                const auto z = bandRects[i].toFloat();
                g.setColour (bandHues[i].withAlpha (0.06f));
                g.fillRect (z);
                g.setColour (bandHues[i].withAlpha (0.30f));
                g.drawRect (z.reduced (0.5f), 1.0f);

                // Hue chip at the top edge of each zone
                g.setColour (bandHues[i]);
                g.fillRect (z.getCentreX() - 12.0f, z.getY() + 1.5f, 24.0f, 2.5f);
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQPanel)
    };
}
