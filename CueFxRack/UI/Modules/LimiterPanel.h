#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"
#include <array>
#include <functional>

namespace cue
{
    // Multiband (L3-style) limiter: GAIN / CEILING / RELEASE, AUTO (ARC) and
    // TP (true-peak) switches, plus a live per-band gain-reduction display.
    class LimiterPanel final : public ModulePanel,
                               private juce::Timer
    {
    public:
        explicit LimiterPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "LIMITER", pid::limOn, colours::cueEmber)
        {
            setTagline ("multiband maximizer");
        }
        ~LimiterPanel() override { stopTimer(); }

        // negative dB per band, from the processor
        void setBandGRSource (std::function<float (int)> src, int numBands)
        {
            bandGRSource = std::move (src);
            bands = juce::jlimit (1, (int) dispGr.size(), numBands);
            startTimerHz (30);
        }

    private:
        juce::Slider& gain    { addKnob (pid::limGain,    "GAIN", " DB") };
        juce::Slider& ceiling { addKnob (pid::limCeiling, "CEILING", " DB") };
        juce::Slider& release { addKnob (pid::limRelease, "RELEASE", " MS", 0) };
        juce::ToggleButton& autoRel  { addToggle (pid::limAutoRel,  "AUTO") };
        juce::ToggleButton& truePeak { addToggle (pid::limTruePeak, "TP") };

        juce::Rectangle<int> barsRect;
        std::function<float (int)> bandGRSource;
        std::array<float, 5> dispGr { {} };
        int bands = 4;

        static constexpr float maxGrDb = 12.0f;   // full-scale of the display

        void timerCallback() override
        {
            if (bandGRSource == nullptr)
                return;

            bool changed = false;
            for (int b = 0; b < bands; ++b)
            {
                const auto target = juce::jmax (0.0f, -bandGRSource (b));
                const auto next   = juce::jmax (target, dispGr[(size_t) b] - 0.9f);
                if (! juce::approximatelyEqual (next, dispGr[(size_t) b]))
                {
                    dispGr[(size_t) b] = next;
                    changed = true;
                }
            }
            if (changed)
                repaint (barsRect);
        }

        void layoutContent (juce::Rectangle<int> area) override
        {
            barsRect = area.removeFromRight (juce::jmax (40, area.getWidth() / 4));

            auto toggleRow = area.removeFromBottom (22);
            autoRel.setBounds  (toggleRow.removeFromLeft (toggleRow.getWidth() / 2).reduced (2, 0));
            truePeak.setBounds (toggleRow.reduced (2, 0));

            const int cellH = juce::jmax (40, area.getHeight() / 3);
            placeKnob (gain,    area.removeFromTop (cellH), 52);
            placeKnob (ceiling, area.removeFromTop (cellH), 52);
            placeKnob (release, area, 52);
        }

        void paintContent (juce::Graphics& g, juce::Rectangle<int>) override
        {
            if (barsRect.getWidth() < 24 || barsRect.getHeight() < 40)
                return;

            auto r = barsRect.reduced (4, 14);
            g.setColour (colours::creamDim);
            g.setFont (monoFont (7.5f));
            g.drawText ("GR", barsRect.getX(), barsRect.getY(), barsRect.getWidth(), 12,
                        juce::Justification::centred, false);

            const auto colW = (float) r.getWidth() / (float) bands;
            static const char* names[] { "LO", "LM", "HM", "HI", "AIR" };

            for (int b = 0; b < bands; ++b)
            {
                juce::Rectangle<float> col (r.getX() + (float) b * colW, (float) r.getY(),
                                            colW - 2.0f, (float) r.getHeight() - 10.0f);
                g.setColour (colours::slot);
                g.fillRect (col);

                const auto frac = juce::jlimit (0.0f, 1.0f, dispGr[(size_t) b] / maxGrDb);
                auto fill = col.withTop (col.getY()).removeFromTop (col.getHeight() * frac);
                g.setColour (getAccent().withAlpha (0.9f));
                g.fillRect (fill);

                g.setColour (colours::line);
                g.drawRect (col, 1.0f);

                g.setColour (colours::creamDim);
                g.setFont (monoFont (6.5f));
                g.drawText (names[juce::jmin (b, 4)],
                            (int) col.getX(), r.getBottom() - 9, (int) col.getWidth() + 2, 9,
                            juce::Justification::centred, false);
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterPanel)
    };
}
