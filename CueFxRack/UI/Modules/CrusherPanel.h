#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // ========================================================================
    // CrusherScreen — the LED matrix as a live dot-matrix scope: a scrolling
    // wave drawn exactly as the crusher would render it. BITS sets how many
    // amplitude levels exist, RATE holds columns (sample & hold), MIX sets
    // brightness.
    // ========================================================================
    class CrusherScreen final : public juce::Component,
                                private juce::Timer
    {
    public:
        explicit CrusherScreen (juce::AudioProcessorValueTreeState& s) : apvts (s)
        {
            setInterceptsMouseClicks (false, false);
            startTimerHz (24);
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat().reduced (6.0f, 4.0f);
            if (b.getWidth() < 60.0f || b.getHeight() < 30.0f)   // mid-animation / tiny panel
                return;

            constexpr int cols = 18, rows = 9;
            const auto cw = b.getWidth() / (float) cols;
            const auto chh = b.getHeight() / (float) rows;

            const auto bits   = get (pid::crushBits);
            const auto rate01 = juce::jlimit (0.0f, 1.0f, (get (pid::crushRate) - 1.0f) / 49.0f);
            const auto mix01  = get (pid::crushMix) / 100.0f;

            // amplitude levels available at this bit depth (capped by screen rows)
            const auto levels = juce::jlimit (2, rows, (int) std::round (std::pow (2.0f, juce::jmin (bits, 3.17f))));
            const auto holdN  = 1 + (int) std::round (rate01 * 7.0f);   // columns held per sample
            const auto bright = 0.35f + 0.65f * mix01;

            for (int c = 0; c < cols; ++c)
            {
                // sample & hold the wave in column groups
                const auto idx      = scroll + c;
                const auto heldIdx  = (idx / holdN) * holdN;
                const auto v        = 0.5f + 0.5f * std::sin ((float) heldIdx * 0.55f);

                // quantise to the available levels, map to a row
                const auto q   = std::round (v * (float) (levels - 1)) / (float) (levels - 1);
                const auto row = juce::jlimit (0, rows - 1, (int) std::round ((1.0f - q) * (float) (rows - 1)));

                for (int rIdx = 0; rIdx < rows; ++rIdx)
                {
                    juce::Rectangle<float> cell (b.getX() + (float) c * cw + 1.5f,
                                                 b.getY() + (float) rIdx * chh + 1.5f,
                                                 cw - 3.0f, chh - 3.0f);
                    if (rIdx == row)
                    {
                        g.setColour (colours::cueOrange.withAlpha (bright));
                        g.fillRect (cell);
                    }
                    else
                    {
                        g.setColour (colours::creamDim.withAlpha (0.16f));
                        g.drawRect (cell, 1.0f);
                    }
                }
            }
        }

    private:
        void timerCallback() override
        {
            ++scroll;
            repaint();
        }

        float get (const char* paramID) const
        {
            if (auto* raw = apvts.getRawParameterValue (paramID))
                return raw->load();
            return 0.0f;
        }

        juce::AudioProcessorValueTreeState& apvts;
        int scroll = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrusherScreen)
    };

    //==========================================================================
    class CrusherPanel final : public ModulePanel
    {
    public:
        explicit CrusherPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "BIT CRUSHER", pid::crushOn, colours::cueOrange)
        {
            addAndMakeVisible (screen);
        }

    private:
        juce::Slider& bits  { addKnob (pid::crushBits, "BITS") };
        juce::Slider& rate  { addKnob (pid::crushRate, "RATE", "X", 0) };
        juce::Slider& drive { addKnob (pid::crushDrive, "DRIVE", " DB") };
        juce::Slider& mix   { addKnob (pid::crushMix, "MIX", " %", 0) };

        CrusherScreen screen { apvts };

        void layoutContent (juce::Rectangle<int> area) override
        {
            screen.setBounds (area.removeFromTop (area.getHeight() - 224).reduced (4, 2));
            auto row1 = area.removeFromTop (112);
            auto row2 = area;

            placeKnob (bits,  row1.removeFromLeft (row1.getWidth() / 2), 62);
            placeKnob (rate,  row1, 52);
            placeKnob (drive, row2.removeFromLeft (row2.getWidth() / 2), 52);
            placeKnob (mix,   row2, 52);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrusherPanel)
    };
}
