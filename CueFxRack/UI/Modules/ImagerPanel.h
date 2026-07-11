#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // ========================================================================
    // ImagerScope — the radar as a real goniometer. A point cloud driven by
    // the processor's mid/side levels: mono = tall thin cloud on the vertical,
    // wide = round cloud. Fed by the editor via setLevelSource().
    // ========================================================================
    class ImagerScope final : public juce::Component,
                              private juce::Timer
    {
    public:
        ImagerScope()
        {
            setInterceptsMouseClicks (false, false);
            startTimerHz (30);
        }

        std::function<juce::Point<float>()> levelSource;   // returns (mid, side)

        void paint (juce::Graphics& g) override
        {
            const auto b      = getLocalBounds().toFloat();
            const auto centre = b.getCentre();
            const auto radius = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 10.0f;

            if (radius < 8.0f)                                   // mid-animation / tiny panel
                return;

            // frame: ring + crosshair + diagonals
            g.setColour (colours::creamDim.withAlpha (0.8f));
            g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.2f);
            g.setColour (colours::creamDim.withAlpha (0.4f));
            g.drawLine (centre.x - radius, centre.y, centre.x + radius, centre.y, 1.0f);
            g.drawLine (centre.x, centre.y - radius, centre.x, centre.y + radius, 1.0f);
            for (auto a : { juce::MathConstants<float>::pi * 0.25f, juce::MathConstants<float>::pi * 0.75f })
            {
                const auto p1 = centre.getPointOnCircumference (radius, a);
                const auto p2 = centre.getPointOnCircumference (radius, a + juce::MathConstants<float>::pi);
                g.drawLine ({ p1, p2 }, 1.0f);
            }

            g.setFont (monoFont (9.5f));
            g.setColour (colours::creamDim);
            g.drawText ("L", (int) (centre.x - radius) - 2, (int) centre.y - 16, 10, 10,
                        juce::Justification::centred, false);
            g.drawText ("R", (int) (centre.x + radius) - 8, (int) centre.y - 16, 10, 10,
                        juce::Justification::centred, false);

            // goniometer cloud: x = side, y = mid
            const auto sx = juce::jmin (radius, dispSide * radius * 2.4f);
            const auto sy = juce::jmin (radius, dispMid  * radius * 1.6f);

            g.setColour (colours::cueSky.withAlpha (0.8f));
            juce::Random rng (seed);
            for (int i = 0; i < 56; ++i)
            {
                // triangular distribution ~ gaussian-ish
                const auto rx = (rng.nextFloat() + rng.nextFloat() - 1.0f) * sx;
                const auto ry = (rng.nextFloat() + rng.nextFloat() - 1.0f) * sy;
                if (rx * rx + ry * ry > radius * radius)
                    continue;
                g.fillEllipse (centre.x + rx - 1.2f, centre.y + ry - 1.2f, 2.4f, 2.4f);
            }

            // faint envelope ellipse
            if (sx > 2.0f || sy > 2.0f)
            {
                g.setColour (colours::cueSky.withAlpha (0.4f));
                g.drawEllipse (centre.x - juce::jmax (2.0f, sx), centre.y - juce::jmax (2.0f, sy),
                               juce::jmax (2.0f, sx) * 2.0f, juce::jmax (2.0f, sy) * 2.0f, 1.0f);
            }
        }

    private:
        void timerCallback() override
        {
            const auto lv = levelSource != nullptr ? levelSource() : juce::Point<float>();
            dispMid  += (juce::jlimit (0.0f, 1.0f, lv.x) - dispMid)  * 0.35f;
            dispSide += (juce::jlimit (0.0f, 1.0f, lv.y) - dispSide) * 0.35f;
            if (++frame % 2 == 0)
                seed = (juce::int64) frame * 7919;   // resample the cloud
            repaint();
        }

        float dispMid = 0.0f, dispSide = 0.0f;
        int frame = 0;
        juce::int64 seed = 1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImagerScope)
    };

    //==========================================================================
    class ImagerPanel final : public ModulePanel
    {
    public:
        explicit ImagerPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "STEREO IMAGER", pid::imgOn, colours::cueSky)
        {
            addAndMakeVisible (scope);
        }

        void setLevelSource (std::function<juce::Point<float>()> source)
        {
            scope.levelSource = std::move (source);
        }

    private:
        juce::Slider& width { addKnob (pid::imgWidth, "WIDTH", " %", 0) };
        juce::Slider& xover { addKnob (pid::imgXover, "XOVER", " HZ", 0) };
        juce::ToggleButton& bassMono { addToggle (pid::imgBassMono, "BASS MONO") };

        ImagerScope scope;

        void layoutContent (juce::Rectangle<int> area) override
        {
            scope.setBounds (area.removeFromTop (area.getHeight() - 166).reduced (2));
            auto toggleRow = area.removeFromBottom (22);
            bassMono.setBounds (toggleRow.withSizeKeepingCentre (110, 20));

            placeKnob (width, area.removeFromLeft (area.getWidth() / 2), 68);
            placeKnob (xover, area, 52);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImagerPanel)
    };
}
