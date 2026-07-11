#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // ========================================================================
    // AmpTubes — a row of glowing vacuum tubes on a chassis. Glow intensity
    // follows DRIVE, each tube flickers on its own rhythm, and the whole
    // thing breathes a little brighter on hotter presets.
    // ========================================================================
    class AmpTubes final : public juce::Component,
                           private juce::Timer
    {
    public:
        explicit AmpTubes (juce::AudioProcessorValueTreeState& s) : apvts (s)
        {
            setInterceptsMouseClicks (false, false);
            startTimerHz (24);
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat().reduced (10.0f, 6.0f);
            if (b.getWidth() < 100.0f || b.getHeight() < 44.0f)
                return;

            // chassis
            g.setColour (colours::slot);
            g.fillRoundedRectangle (b, 4.0f);
            g.setColour (colours::line);
            g.drawRoundedRectangle (b, 4.0f, 1.0f);

            const auto drive01  = get (pid::ampDrive) / 36.0f;
            const auto preset   = (int) get (pid::ampPreset);
            const auto hot      = preset == 3 || preset == 5 ? 0.15f : 0.0f;   // PLEXI/RECTO run hotter
            const auto baseGlow = 0.25f + 0.55f * drive01 + hot;

            const auto tubeH = b.getHeight() * 0.72f;
            const auto tubeW = juce::jmin (20.0f, b.getWidth() * 0.08f);
            const auto cy    = b.getCentreY() + 2.0f;

            for (int i = 0; i < 4; ++i)
            {
                const auto cx = b.getX() + b.getWidth() * (0.20f + 0.20f * (float) i);
                const auto flick = 0.5f + 0.5f * std::sin (phase * (2.2f + 0.6f * (float) i) + (float) i * 2.4f);
                const auto glow  = juce::jlimit (0.05f, 1.0f, baseGlow * (0.82f + 0.25f * flick));

                juce::Rectangle<float> tube (cx - tubeW * 0.5f, cy - tubeH * 0.5f, tubeW, tubeH);

                // halo
                g.setColour (colours::cueTube.withAlpha (glow * 0.30f));
                g.fillEllipse (tube.expanded (7.0f + 5.0f * glow, 5.0f + 4.0f * glow));

                // glass envelope
                g.setColour (colours::slot.brighter (0.10f));
                g.fillRoundedRectangle (tube, tubeW * 0.5f);
                g.setColour (colours::creamDim.withAlpha (0.7f));
                g.drawRoundedRectangle (tube, tubeW * 0.5f, 1.0f);

                // filament: hot centre line + coil ticks
                g.setColour (colours::cueTube.withAlpha (0.35f + 0.65f * glow));
                g.drawLine (cx, tube.getY() + 6.0f, cx, tube.getBottom() - 6.0f, 1.8f);
                for (int k = 0; k < 4; ++k)
                {
                    const auto y = tube.getY() + 8.0f + (tubeH - 16.0f) * (float) k / 3.0f;
                    g.drawLine (cx - 3.0f, y, cx + 3.0f, y, 1.2f);
                }

                // base pins
                g.setColour (colours::creamDim.withAlpha (0.5f));
                g.fillRect (cx - tubeW * 0.35f, tube.getBottom(), tubeW * 0.7f, 3.0f);
            }

            // pilot lamp
            const auto on = get (pid::ampOn) > 0.5f;
            g.setColour (on ? colours::cueTube : colours::creamDim.withAlpha (0.4f));
            g.fillEllipse (b.getRight() - 16.0f, b.getY() + 8.0f, 6.0f, 6.0f);
        }

    private:
        void timerCallback() override
        {
            phase += 1.0f / 24.0f;
            if (phase > 4000.0f)
                phase = 0.0f;
            repaint();
        }

        float get (const char* paramID) const
        {
            if (auto* raw = apvts.getRawParameterValue (paramID))
                return raw->load();
            return 0.0f;
        }

        juce::AudioProcessorValueTreeState& apvts;
        float phase = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpTubes)
    };

    //==========================================================================
    class AmpPanel final : public ModulePanel
    {
    public:
        explicit AmpPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "AMP", pid::ampOn, colours::cueTube)
        {
            addAndMakeVisible (tubes);
        }

    private:
        juce::Slider& preset { addChoiceKnob (pid::ampPreset, "PRESET", ampPresets) };
        juce::Slider& drive  { addKnob (pid::ampDrive, "DRIVE", " DB") };
        juce::Slider& bassK  { addKnob (pid::ampBass, "BASS", " DB") };
        juce::Slider& midK   { addKnob (pid::ampMid, "MID", " DB") };
        juce::Slider& trebK  { addKnob (pid::ampTreble, "TREBLE", " DB") };
        juce::Slider& level  { addKnob (pid::ampLevel, "LEVEL", " DB") };
        juce::Slider& mix    { addKnob (pid::ampMix, "MIX", " %", 0) };

        AmpTubes tubes { apvts };

        void layoutContent (juce::Rectangle<int> area) override
        {
            tubes.setBounds (area.removeFromTop (juce::jmax (44, area.getHeight() - 210)));

            auto row1 = area.removeFromTop (area.getHeight() / 2);
            const int c3 = row1.getWidth() / 3;
            placeKnob (preset, row1.removeFromLeft (c3), 54);
            placeKnob (drive,  row1.removeFromLeft (c3), 54);
            placeKnob (level,  row1, 54);

            const int c4 = area.getWidth() / 4;
            placeKnob (bassK, area.removeFromLeft (c4), 46);
            placeKnob (midK,  area.removeFromLeft (c4), 46);
            placeKnob (trebK, area.removeFromLeft (c4), 46);
            placeKnob (mix,   area, 46);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpPanel)
    };
}
