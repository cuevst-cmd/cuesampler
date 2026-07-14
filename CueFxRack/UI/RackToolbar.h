#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "CueLookAndFeel.h"

namespace cue
{
    // ========================================================================
    // RackToolbar — the strip along the bottom of the rack.
    //
    // Every unused (removed) module as a chip with a small animated icon;
    // clicking a chip restores that panel to the rack. (CUESAMPLER port:
    // the rack's saved-racks section is not included here.)
    // ========================================================================
    class RackToolbar final : public juce::Component,
                              private juce::Timer
    {
    public:
        struct ModuleEntry
        {
            juce::String id;
            juce::Colour colour;
        };

        RackToolbar();
        ~RackToolbar() override;   // defined in the .cpp (chip types complete there)

        std::function<void (const juce::String&)> onRestorePanel;

        void setUnusedPanels (const std::vector<ModuleEntry>& entries);

        void paint (juce::Graphics&) override;
        void resized() override;

        static void drawModuleIcon (juce::Graphics&, const juce::String& id,
                                    juce::Rectangle<float> area, float phase,
                                    juce::Colour colour);

    private:
        void timerCallback() override;

        std::unique_ptr<juce::Drawable> cueWordmark;
        juce::Colour wordmarkColour = colours::cream;

        //==================================================================
        class ModuleChip;

        juce::OwnedArray<ModuleChip> moduleChips;

        float phase = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackToolbar)
    };
}
