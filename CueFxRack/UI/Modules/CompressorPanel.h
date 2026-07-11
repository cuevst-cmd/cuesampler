#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // API 2500-inspired: stepped ratio/attack, knee, Thrust, NEW/OLD topology.
    // Live gain-reduction LED meter fed by the processor.
    class CompressorPanel final : public ModulePanel,
                                  private juce::Timer
    {
    public:
        explicit CompressorPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "COMPRESSOR", pid::compOn, colours::cueMid) {}

        void setGRSource (std::function<float()> sourceDb)   // negative dB from processor
        {
            grSource = std::move (sourceDb);
            startTimerHz (30);
        }

    private:
        juce::Slider& thresh  { addKnob (pid::compThresh,  "THRESH", " DB") };
        juce::Slider& ratio   { addChoiceKnob (pid::compRatio,  "RATIO",  compRatios) };
        juce::Slider& attack  { addChoiceKnob (pid::compAttack, "ATTACK", compAttacks, " MS") };
        juce::Slider& release { addKnob (pid::compRelease, "RELEASE", " S", 2) };
        juce::Slider& makeup  { addKnob (pid::compMakeup,  "MAKEUP", " DB") };
        juce::Slider& mix     { addKnob (pid::compMix,     "MIX", " %", 0) };
        juce::Slider& knee    { addChoiceKnob (pid::compKnee,   "KNEE",   compKnees) };
        juce::Slider& thrust  { addChoiceKnob (pid::compThrust, "THRUST", compThrusts) };
        juce::ToggleButton& oldMode { addToggle (pid::compType, "OLD (FB)") };

        juce::Rectangle<int> meterRect;
        std::function<float()> grSource;
        float dispGr = 0.0f;   // displayed gain reduction, positive dB

        void timerCallback() override
        {
            const auto target = grSource != nullptr ? juce::jmax (0.0f, -grSource()) : 0.0f;
            const auto next   = juce::jmax (target, dispGr - 0.9f);   // fast rise, ~27 dB/s fall
            if (! juce::approximatelyEqual (next, dispGr))
            {
                dispGr = next;
                repaint (meterRect);
            }
        }

        void layoutContent (juce::Rectangle<int> area) override
        {
            if (area.isEmpty())
                return;

            auto row1    = area.removeFromTop (72);
            auto row2    = area.removeFromTop (72);
            auto row3    = area.removeFromTop (64);
            meterRect    = area;

            const int colW = row1.getWidth() / 3;
            placeKnob (thresh,  row1.removeFromLeft (colW), 56);
            placeKnob (ratio,   row1.removeFromLeft (colW), 56);
            placeKnob (attack,  row1, 56);
            placeKnob (release, row2.removeFromLeft (colW), 56);
            placeKnob (makeup,  row2.removeFromLeft (colW), 56);
            placeKnob (mix,     row2, 56);

            placeKnob (knee,   row3.removeFromLeft (colW), 42);
            placeKnob (thrust, row3.removeFromLeft (colW), 42);
            oldMode.setBounds (row3.reduced (6, 0).withSizeKeepingCentre (colW - 12, 18));
        }

        void paintContent (juce::Graphics& g, juce::Rectangle<int>) override
        {
            if (meterRect.getWidth() < 230 || meterRect.getHeight() < 20)
                return;

            auto r = meterRect.toFloat().reduced (8.0f, 6.0f);

            g.setColour (colours::creamDim);
            g.setFont (monoFont (9.5f));
            g.drawText ("GR", r.removeFromLeft (20.0f), juce::Justification::centredLeft, false);

            // Live gain-reduction LEDs
            static const float ledDb[8] { 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 9.0f, 12.0f };
            auto ledArea = r.removeFromLeft (170.0f);
            for (int i = 0; i < 8; ++i)
            {
                const auto cx  = ledArea.getX() + ((float) i + 0.5f) / 8.0f * ledArea.getWidth();
                const auto cy  = ledArea.getCentreY();
                const auto lit = dispGr >= ledDb[i];

                g.setColour (colours::slot);
                g.fillEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
                if (lit)
                {
                    g.setColour (getAccent());
                    g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
                }
                g.setColour (lit ? getAccent() : colours::creamDim.withAlpha (0.5f));
                g.drawEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 1.0f);
            }

            // Small gear (nod to the reference panel)
            const auto gc = juce::Point<float> (r.getRight() - 24.0f, r.getCentreY());
            g.setColour (colours::creamDim.withAlpha (0.8f));
            const float gr = 13.0f;
            g.drawEllipse (gc.x - gr, gc.y - gr, gr * 2.0f, gr * 2.0f, 1.4f);
            g.drawEllipse (gc.x - gr * 0.45f, gc.y - gr * 0.45f, gr * 0.9f, gr * 0.9f, 1.2f);
            for (int i = 0; i < 8; ++i)
            {
                const auto a  = (float) i * juce::MathConstants<float>::twoPi / 8.0f;
                const auto p1 = gc.getPointOnCircumference (gr, a);
                const auto p2 = gc.getPointOnCircumference (gr + 3.5f, a);
                g.drawLine ({ p1, p2 }, 2.2f);
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressorPanel)
    };
}
