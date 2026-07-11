#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // ========================================================================
    // ModViz — animated visual shared by the modulation trio.
    //   CHORUS : two drifting sine voices separating/overlapping (RATE =
    //            drift speed, DEPTH = voice separation, MIX = intensity)
    //   FLANGER: a sweeping comb — teeth sharpen with FEEDBACK, slide with
    //            RATE, sweep width follows DEPTH
    //   FLANGUS: three phase-spread voices fanning apart with SPREAD
    // ========================================================================
    class ModViz final : public juce::Component,
                         private juce::Timer
    {
    public:
        enum class Kind { chorus, flanger, flangus };

        ModViz (juce::AudioProcessorValueTreeState& s, Kind k, juce::Colour c)
            : apvts (s), kind (k), colour (c)
        {
            setInterceptsMouseClicks (false, false);
            startTimerHz (30);
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat().reduced (10.0f, 8.0f);
            if (b.getWidth() < 70.0f || b.getHeight() < 30.0f)
                return;

            const auto cy = b.getCentreY();

            // faint zero line
            g.setColour (colours::creamDim.withAlpha (0.25f));
            g.drawLine (b.getX(), cy, b.getRight(), cy, 1.0f);

            const auto k = juce::MathConstants<float>::twoPi * 2.5f / b.getWidth();

            if (kind == Kind::chorus)
            {
                const auto depth = get (pid::chDepth) * 0.01f;
                const auto mix   = get (pid::chMix) * 0.01f;
                const auto amp   = b.getHeight() * 0.32f;
                const auto off   = (0.6f + 2.0f * depth) * std::sin (sweep * juce::MathConstants<float>::twoPi);

                drawWave (g, b, cy, amp, k, phase, 0.0f, colour.withAlpha (0.35f + 0.6f * mix));
                drawWave (g, b, cy, amp, k, phase, off, colour.withAlpha (0.25f + 0.45f * mix));
            }
            else if (kind == Kind::flanger)
            {
                const auto fb    = std::abs (get (pid::flFeedback)) / 95.0f;
                const auto depth = get (pid::flDepth) * 0.01f;
                const auto mix   = get (pid::flMix) * 0.01f;
                const auto amp   = b.getHeight() * 0.38f;
                const auto sharp = 1.5f + 6.0f * fb;
                const auto slide = 3.2f * depth * std::sin (sweep * juce::MathConstants<float>::twoPi);
                const auto kTeeth = juce::MathConstants<float>::twoPi * 4.0f / b.getWidth();

                juce::Path p;
                for (int i = 0; i <= 96; ++i)
                {
                    const auto x = b.getX() + b.getWidth() * (float) i / 96.0f;
                    const auto u = kTeeth * (x - b.getX()) + slide + phase * 1.3f;
                    const auto tooth = std::pow (std::abs (std::sin (u)), sharp);
                    const auto y = cy + amp * 0.9f - amp * 1.8f * tooth;
                    i == 0 ? p.startNewSubPath (x, y) : p.lineTo (x, y);
                }
                g.setColour (colour.withAlpha (0.35f + 0.6f * mix));
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
            }
            else   // flangus
            {
                const auto depth  = get (pid::fgDepth) * 0.01f;
                const auto spread = get (pid::fgSpread) * 0.01f;
                const auto mix    = get (pid::fgMix) * 0.01f;
                const auto amp    = b.getHeight() * 0.17f * (0.4f + 0.6f * depth);

                for (int v = 0; v < 3; ++v)
                {
                    const auto yc = cy + ((float) v - 1.0f) * spread * b.getHeight() * 0.27f;
                    const auto ph = phase + (float) v * 2.094f;
                    const auto a  = (0.9f - 0.22f * (float) v) * (0.35f + 0.65f * mix);
                    drawWave (g, b, yc, amp, k, ph, 0.0f, colour.withAlpha (a));
                }
            }
        }

    private:
        void drawWave (juce::Graphics& g, juce::Rectangle<float> b, float yc, float amp,
                       float k, float ph, float extraPhase, juce::Colour c) const
        {
            juce::Path p;
            for (int i = 0; i <= 80; ++i)
            {
                const auto x = b.getX() + b.getWidth() * (float) i / 80.0f;
                const auto y = yc + amp * std::sin (k * (x - b.getX())
                                                    + ph * juce::MathConstants<float>::twoPi
                                                    + extraPhase);
                i == 0 ? p.startNewSubPath (x, y) : p.lineTo (x, y);
            }
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
        }

        void timerCallback() override
        {
            const auto rate = get (kind == Kind::chorus ? pid::chRate
                                 : kind == Kind::flanger ? pid::flRate
                                                         : pid::fgRate);
            phase += (0.10f + rate * 0.10f);
            sweep += rate * (1.0f / 30.0f);
            if (phase > 1000.0f) phase -= 1000.0f;
            if (sweep > 1000.0f) sweep -= 1000.0f;
            repaint();
        }

        float get (const char* paramID) const
        {
            if (auto* raw = apvts.getRawParameterValue (paramID))
                return raw->load();
            return 0.0f;
        }

        juce::AudioProcessorValueTreeState& apvts;
        Kind kind;
        juce::Colour colour;
        float phase = 0.0f, sweep = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModViz)
    };

    //==========================================================================
    class ChorusPanel final : public ModulePanel
    {
    public:
        explicit ChorusPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "CHORUS", pid::chOn, colours::cueBlush)
        {
            addAndMakeVisible (viz);
        }

    private:
        juce::Slider& rate  { addKnob (pid::chRate, "RATE", " HZ", 2) };
        juce::Slider& depth { addKnob (pid::chDepth, "DEPTH", " %", 0) };
        juce::Slider& del   { addKnob (pid::chDelay, "DELAY", " MS", 1) };
        juce::Slider& fb    { addKnob (pid::chFeedback, "FEEDBACK", " %", 0) };
        juce::Slider& mix   { addKnob (pid::chMix, "MIX", " %", 0) };

        ModViz viz { apvts, ModViz::Kind::chorus, colours::cueBlush };

        void layoutContent (juce::Rectangle<int> area) override
        {
            viz.setBounds (area.removeFromTop (juce::jmax (34, area.getHeight() - 200)));
            auto row1 = area.removeFromTop (area.getHeight() / 2);
            const int c3 = row1.getWidth() / 3;
            placeKnob (rate,  row1.removeFromLeft (c3), 50);
            placeKnob (depth, row1.removeFromLeft (c3), 50);
            placeKnob (del,   row1, 50);
            const int c2 = area.getWidth() / 2;
            placeKnob (fb,  area.removeFromLeft (c2), 50);
            placeKnob (mix, area, 50);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChorusPanel)
    };

    //==========================================================================
    class FlangerPanel final : public ModulePanel
    {
    public:
        explicit FlangerPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "FLANGER", pid::flOn, colours::cuePeach)
        {
            addAndMakeVisible (viz);
        }

    private:
        juce::Slider& rate  { addKnob (pid::flRate, "RATE", " HZ", 2) };
        juce::Slider& depth { addKnob (pid::flDepth, "DEPTH", " %", 0) };
        juce::Slider& fb    { addKnob (pid::flFeedback, "FEEDBACK", " %", 0) };
        juce::Slider& mix   { addKnob (pid::flMix, "MIX", " %", 0) };

        ModViz viz { apvts, ModViz::Kind::flanger, colours::cuePeach };

        void layoutContent (juce::Rectangle<int> area) override
        {
            viz.setBounds (area.removeFromTop (juce::jmax (34, area.getHeight() - 200)));
            auto row1 = area.removeFromTop (area.getHeight() / 2);
            const int c2 = row1.getWidth() / 2;
            placeKnob (rate,  row1.removeFromLeft (c2), 52);
            placeKnob (depth, row1, 52);
            const int c2b = area.getWidth() / 2;
            placeKnob (fb,  area.removeFromLeft (c2b), 52);
            placeKnob (mix, area, 52);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlangerPanel)
    };

    //==========================================================================
    class FlangusPanel final : public ModulePanel
    {
    public:
        explicit FlangusPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "FLANGUS", pid::fgOn, colours::cuePale)
        {
            addAndMakeVisible (viz);
        }

    private:
        juce::Slider& rate   { addKnob (pid::fgRate, "RATE", " HZ", 2) };
        juce::Slider& depth  { addKnob (pid::fgDepth, "DEPTH", " %", 0) };
        juce::Slider& spread { addKnob (pid::fgSpread, "SPREAD", " %", 0) };
        juce::Slider& mix    { addKnob (pid::fgMix, "MIX", " %", 0) };

        ModViz viz { apvts, ModViz::Kind::flangus, colours::cuePale };

        void layoutContent (juce::Rectangle<int> area) override
        {
            viz.setBounds (area.removeFromTop (juce::jmax (34, area.getHeight() - 200)));
            auto row1 = area.removeFromTop (area.getHeight() / 2);
            const int c2 = row1.getWidth() / 2;
            placeKnob (rate,  row1.removeFromLeft (c2), 52);
            placeKnob (depth, row1, 52);
            const int c2b = area.getWidth() / 2;
            placeKnob (spread, area.removeFromLeft (c2b), 52);
            placeKnob (mix,    area, 52);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlangusPanel)
    };
}
