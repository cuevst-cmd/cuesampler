#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // ========================================================================
    // TapeReels — two spinning reels with tape running between them.
    // Reel speed follows delay time (short delay = fast tape); the tape
    // path wobbles gently (wow), and everything stops when the module is off.
    // ========================================================================
    class TapeReels final : public juce::Component,
                            private juce::Timer
    {
    public:
        explicit TapeReels (juce::AudioProcessorValueTreeState& s) : apvts (s)
        {
            setInterceptsMouseClicks (false, false);
            startTimerHz (30);
        }
        ~TapeReels() override { stopTimer(); }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat().reduced (8.0f, 4.0f);
            if (b.getWidth() < 90.0f || b.getHeight() < 34.0f)
                return;

            const auto reelR = juce::jmin (b.getHeight() * 0.42f, b.getWidth() * 0.18f);
            const auto cy    = b.getCentreY();
            const auto cxL   = b.getX() + b.getWidth() * 0.26f;
            const auto cxR   = b.getX() + b.getWidth() * 0.74f;

            const auto accent = colours::cueViolet;
            const auto wob    = 1.4f * std::sin (phase * 2.1f);

            // tape path between reels (sags slightly, wobbles with wow)
            g.setColour (colours::creamDim.withAlpha (0.75f));
            juce::Path tape;
            tape.startNewSubPath (cxL, cy - reelR);
            tape.cubicTo (cxL + (cxR - cxL) * 0.33f, cy - reelR + 2.5f + wob,
                          cxL + (cxR - cxL) * 0.66f, cy - reelR + 2.5f - wob,
                          cxR, cy - reelR);
            tape.startNewSubPath (cxL, cy + reelR);
            tape.cubicTo (cxL + (cxR - cxL) * 0.33f, cy + reelR - 2.0f - wob,
                          cxL + (cxR - cxL) * 0.66f, cy + reelR - 2.0f + wob,
                          cxR, cy + reelR);
            g.strokePath (tape, juce::PathStrokeType (1.2f));

            drawReel (g, { cxL, cy }, reelR, angle, accent);
            drawReel (g, { cxR, cy }, reelR * 0.92f, -angle * 1.09f, accent);
        }

    private:
        void drawReel (juce::Graphics& g, juce::Point<float> c, float r,
                       float a, juce::Colour accent) const
        {
            // platter + rim
            g.setColour (colours::slot);
            g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);
            g.setColour (colours::creamDim);
            g.drawEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.3f);

            // tape pack
            g.setColour (accent.withAlpha (0.30f));
            g.drawEllipse (c.x - r * 0.78f, c.y - r * 0.78f, r * 1.56f, r * 1.56f, r * 0.28f);

            // three rotating spokes + hub
            g.setColour (colours::cream);
            for (int i = 0; i < 3; ++i)
            {
                const auto sa = a + (float) i * (juce::MathConstants<float>::twoPi / 3.0f);
                const auto p1 = c.getPointOnCircumference (r * 0.16f, sa);
                const auto p2 = c.getPointOnCircumference (r * 0.55f, sa);
                g.drawLine ({ p1, p2 }, 2.2f);
            }
            g.fillEllipse (c.x - r * 0.12f, c.y - r * 0.12f, r * 0.24f, r * 0.24f);
        }

        void timerCallback() override
        {
            const auto on   = get (pid::dlyOn) > 0.5f;
            const auto time = juce::jmax (30.0f, get (pid::dlyTime));

            // tape speed ~ inverse of delay time; motor eases in/out
            const auto targetSpeed = on ? juce::jlimit (0.8f, 7.0f, 900.0f / time) : 0.0f;
            speed += (targetSpeed - speed) * 0.06f;

            angle += speed * (1.0f / 30.0f);
            phase += 1.0f / 30.0f;

            if (speed > 0.005f || std::abs (targetSpeed - speed) > 0.005f)
                repaint();
        }

        float get (const char* paramID) const
        {
            if (auto* raw = apvts.getRawParameterValue (paramID))
                return raw->load();
            return 0.0f;
        }

        juce::AudioProcessorValueTreeState& apvts;
        float angle = 0.0f, speed = 0.0f, phase = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeReels)
    };

    //==========================================================================
    class DelayPanel final : public ModulePanel
    {
    public:
        explicit DelayPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "TAPE DELAY", pid::dlyOn, colours::cueViolet)
        {
            addAndMakeVisible (reels);
        }

    private:
        juce::Slider& time     { addKnob (pid::dlyTime, "TIME", " MS", 0) };
        juce::Slider& division { addChoiceKnob (pid::dlyDiv, "DIV", delayDivs) };
        juce::Slider& feedback { addKnob (pid::dlyFeedback, "FEEDBACK", " %", 0) };
        juce::Slider& tone     { addKnob (pid::dlyTone, "TONE", " HZ", 0) };
        juce::Slider& mix      { addKnob (pid::dlyMix, "MIX", " %", 0) };
        juce::ToggleButton& sync     { addToggle (pid::dlySync, "SYNC") };
        juce::ToggleButton& pingPong { addToggle (pid::dlyPingPong, "PING-PONG") };

        TapeReels reels { apvts };

        void layoutContent (juce::Rectangle<int> area) override
        {
            reels.setBounds (area.removeFromTop (juce::jmax (56, area.getHeight() - 250)));

            auto row1 = area.removeFromTop (juce::jmin (140, area.getHeight() - 110));
            auto timeCell = row1.removeFromLeft (row1.getWidth() / 2);
            placeKnob (time, timeCell, 72);

            auto side = row1.reduced (8, 4);
            sync.setBounds (side.removeFromTop (20));
            pingPong.setBounds (side.removeFromBottom (20));
            placeKnob (division, side, 40);

            const int colW = area.getWidth() / 3;
            placeKnob (feedback, area.removeFromLeft (colW), 50);
            placeKnob (tone,     area.removeFromLeft (colW), 50);
            placeKnob (mix,      area, 50);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayPanel)
    };
}
