#pragma once

#include "../ModulePanel.h"
#include "../../Parameters.h"

namespace cue
{
    // ========================================================================
    // ReverbSpace — the grille reimagined as a living, interactive space.
    // Dot rings continuously emanate from the centre: ring reach = SIZE,
    // ring persistence = DECAY, softness = DAMP, brightness = MIX.
    // Drag inside it like an XY pad: horizontal = SIZE, vertical = DECAY.
    // ========================================================================
    class ReverbSpace final : public juce::Component,
                              private juce::Timer
    {
    public:
        explicit ReverbSpace (juce::AudioProcessorValueTreeState& s) : apvts (s)
        {
            sizeParam  = s.getParameter (pid::revSize);
            decayParam = s.getParameter (pid::revDecay);

            if (sizeParam != nullptr)
                sizeAtt = std::make_unique<juce::ParameterAttachment> (*sizeParam, [this] (float) { repaint(); });
            if (decayParam != nullptr)
                decayAtt = std::make_unique<juce::ParameterAttachment> (*decayParam, [this] (float) { repaint(); });

            startTimerHz (30);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (sizeAtt != nullptr)  sizeAtt->beginGesture();
            if (decayAtt != nullptr) decayAtt->beginGesture();
            applyDrag (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override   { applyDrag (e); }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (sizeAtt != nullptr)  sizeAtt->endGesture();
            if (decayAtt != nullptr) decayAtt->endGesture();
        }

        void paint (juce::Graphics& g) override
        {
            const auto b      = getLocalBounds().toFloat();
            const auto centre = b.getCentre();
            const auto maxR   = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 6.0f;

            if (maxR < 8.0f)                                     // mid-animation / tiny panel
                return;

            const auto size01  = norm (pid::revSize)  / 100.0f;
            const auto decay01 = juce::jlimit (0.0f, 1.0f, (norm (pid::revDecay) - 0.1f) / 14.9f);
            const auto damp01  = norm (pid::revDamp)  / 100.0f;
            const auto mix01   = norm (pid::revMix)   / 100.0f;

            const auto reach = maxR * (0.50f + 0.50f * size01);
            const auto glow  = 0.30f + 0.70f * mix01;

            // emanating dot rings
            constexpr int numRings = 6;
            for (int k = 0; k < numRings; ++k)
            {
                auto ring = std::fmod (phase + (float) k / (float) numRings, 1.0f);
                const auto r = 4.0f + ring * reach;

                // decay controls how far a ring survives; damp softens dots
                auto alpha = (1.0f - ring) * (0.20f + 0.65f * std::pow (1.0f - ring, 1.0f + 3.0f * (1.0f - decay01)));
                alpha *= glow;
                if (alpha < 0.02f)
                    continue;

                g.setColour (getParentAccent().withAlpha (alpha));
                const auto dotR  = 1.9f - 0.8f * damp01;
                const int  count = juce::jmax (8, (int) (juce::MathConstants<float>::twoPi * r / 8.0f));
                for (int i = 0; i < count; ++i)
                {
                    const auto a = (float) i / (float) count * juce::MathConstants<float>::twoPi + ring * 0.8f;
                    const auto p = centre.getPointOnCircumference (r, a);
                    g.fillEllipse (p.x - dotR, p.y - dotR, dotR * 2.0f, dotR * 2.0f);
                }
            }

            // centre source
            g.setColour (colours::cream);
            g.fillEllipse (centre.x - 2.0f, centre.y - 2.0f, 4.0f, 4.0f);

            // XY marker: x = size, y = decay (top = long)
            const auto mx = b.getX() + 8.0f + size01 * (b.getWidth() - 16.0f);
            const auto my = b.getY() + 8.0f + (1.0f - decay01) * (b.getHeight() - 16.0f);
            g.setColour (colours::cream.withAlpha (0.85f));
            g.drawLine (mx - 5.0f, my, mx + 5.0f, my, 1.2f);
            g.drawLine (mx, my - 5.0f, mx, my + 5.0f, 1.2f);
        }

    private:
        void timerCallback() override
        {
            const auto decay01 = juce::jlimit (0.0f, 1.0f, (norm (pid::revDecay) - 0.1f) / 14.9f);
            phase = std::fmod (phase + (0.012f + 0.010f * (1.0f - decay01)), 1.0f);
            repaint();
        }

        void applyDrag (const juce::MouseEvent& e)
        {
            const auto b  = getLocalBounds().toFloat().reduced (8.0f);
            const auto nx = juce::jlimit (0.0f, 1.0f, ((float) e.position.x - b.getX()) / b.getWidth());
            const auto ny = juce::jlimit (0.0f, 1.0f, ((float) e.position.y - b.getY()) / b.getHeight());

            if (sizeAtt != nullptr && sizeParam != nullptr)
                sizeAtt->setValueAsPartOfGesture (sizeParam->convertFrom0to1 (nx));
            if (decayAtt != nullptr && decayParam != nullptr)
                decayAtt->setValueAsPartOfGesture (decayParam->convertFrom0to1 (1.0f - ny));
        }

        float norm (const char* paramID) const
        {
            if (auto* raw = apvts.getRawParameterValue (paramID))
                return raw->load();
            return 0.0f;
        }

        juce::Colour getParentAccent() const   { return colours::cueTeal; }

        juce::AudioProcessorValueTreeState& apvts;
        juce::RangedAudioParameter* sizeParam = nullptr;
        juce::RangedAudioParameter* decayParam = nullptr;
        std::unique_ptr<juce::ParameterAttachment> sizeAtt, decayAtt;
        float phase = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbSpace)
    };

    //==========================================================================
    class ReverbPanel final : public ModulePanel
    {
    public:
        explicit ReverbPanel (juce::AudioProcessorValueTreeState& s)
            : ModulePanel (s, "REVERB", pid::revOn, colours::cueTeal)
        {
            addAndMakeVisible (space);
        }

    private:
        juce::Slider& size     { addKnob (pid::revSize, "SIZE", " %", 0) };
        juce::Slider& decay    { addKnob (pid::revDecay, "DECAY", " S") };
        juce::Slider& damp     { addKnob (pid::revDamp, "DAMP", " %", 0) };
        juce::Slider& predelay { addKnob (pid::revPredelay, "PRE-DLY", " MS", 0) };
        juce::Slider& width    { addKnob (pid::revWidth, "WIDTH", " %", 0) };
        juce::Slider& mix      { addKnob (pid::revMix, "MIX", " %", 0) };

        ReverbSpace space { apvts };

        void layoutContent (juce::Rectangle<int> area) override
        {
            // the animated space takes every pixel the knobs don't need
            space.setBounds (area.removeFromTop (area.getHeight() - 152).reduced (2, 0));
            auto row1 = area.removeFromTop (76);
            auto row2 = area;

            const int colW = row1.getWidth() / 3;
            placeKnob (size,  row1.removeFromLeft (colW), 46);
            placeKnob (decay, row1.removeFromLeft (colW), 46);
            placeKnob (damp,  row1, 46);
            placeKnob (predelay, row2.removeFromLeft (colW), 46);
            placeKnob (width,    row2.removeFromLeft (colW), 46);
            placeKnob (mix,      row2, 46);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbPanel)
    };
}
