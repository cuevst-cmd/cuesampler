#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "CueLookAndFeel.h"

namespace cue
{
    //==========================================================================
    // Small round power switch, drawn in the panel header.
    class PowerButton : public juce::ToggleButton
    {
    public:
        PowerButton() { setClickingTogglesState (true); }
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

        juce::Colour onColour { colours::cream };
    };

    //==========================================================================
    // Small round remove ("x") switch, drawn in the panel header.
    class RemoveButton : public juce::Button
    {
    public:
        RemoveButton() : juce::Button ({}) {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    //==========================================================================
    // Base class for every rack module: plate, screws, title, power switch,
    // remove button, header drag handle, and factory helpers that create
    // controls already attached to the APVTS.
    class ModulePanel : public juce::Component
    {
    public:
        ModulePanel (juce::AudioProcessorValueTreeState& state,
                     juce::String titleText,
                     const char* powerParamID = nullptr,
                     juce::Colour accentColour = colours::cream);

        void paint (juce::Graphics&) override;
        void paintOverChildren (juce::Graphics&) override;   // dims the panel when off
        void resized() override;
        void parentHierarchyChanged() override;

        bool isPowered() const
        {
            return powerAttachment == nullptr || power.getToggleState();
        }

        // Rack interaction (wired up by the editor)
        std::function<void()> onRemoveRequested;
        std::function<void()> onDragStarted;
        std::function<void (juce::Point<int> desiredTopLeft)> onDragMoved;
        std::function<void()> onDragFinished;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp   (const juce::MouseEvent&) override;

        juce::Colour getAccent() const            { return accent; }

        // Re-apply theme-dependent colours cached by this panel's widgets after
        // cue::applyTheme(). Base handles owned labels; overrides call through.
        virtual void refreshColours();

    protected:
        virtual void layoutContent (juce::Rectangle<int> area)               = 0;
        virtual void paintContent (juce::Graphics&, juce::Rectangle<int>)    {}

        juce::Rectangle<int> contentArea() const;

        // Control factories (created attached; caller keeps the reference for layout)
        juce::Slider& addKnob       (const char* paramID, const juce::String& label,
                                     const juce::String& suffix = {}, int decimals = 1);
        juce::Slider& addChoiceKnob (const char* paramID, const juce::String& label,
                                     const juce::StringArray& names, const juce::String& suffix = {});
        juce::Slider& addFader      (const char* paramID, const juce::String& label,
                                     const juce::String& suffix = {});
        juce::Slider& addChoiceFader (const char* paramID, const juce::String& label,
                                      const juce::StringArray& names, const juce::String& suffix = {});
        juce::ToggleButton& addToggle (const char* paramID, const juce::String& label);
        juce::Label& addStatic      (const juce::String& text, float fontHeight = 9.0f, bool bright = false);

        // Layout helpers
        void placeKnob   (juce::Slider&, juce::Rectangle<int> cell, int knobSize);
        void placeFader  (juce::Slider&, juce::Rectangle<int> cell);
        void placeFaderH (juce::Slider&, juce::Rectangle<int> cell, int labelWidth = 34);

        void setTagline (const juce::String& t)   { tagline = t; }

        // Decorations
        static void drawDotGrid (juce::Graphics&, juce::Rectangle<float>,
                                 int cols, int rows, float dotRadius, juce::Colour);
        static void drawJackRow (juce::Graphics&, juce::Rectangle<float>,
                                 int count, const juce::String& caption);
        static void drawScrew   (juce::Graphics&, float cx, float cy);

        juce::AudioProcessorValueTreeState& apvts;

    private:
        juce::Slider& makeSlider (const char* paramID, const juce::String& label,
                                  juce::Slider::SliderStyle);

        juce::String title, tagline;
        juce::Colour accent;
        PowerButton power;
        RemoveButton removeButton;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAttachment;

        bool draggingHeader = false;
        juce::Point<int> dragGrabOffset;

        std::vector<std::unique_ptr<juce::Slider>> sliders;
        std::vector<std::unique_ptr<juce::Label>> ownedLabels;
        // Labels whose text colour follows the theme; bool = bright (cream) vs dim.
        std::vector<std::pair<juce::Label*, bool>> themedLabels;
        std::map<juce::Slider*, juce::Label*> labelFor;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments;
        std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAttachments;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulePanel)
    };
}
