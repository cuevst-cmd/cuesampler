#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace cue
{
    // ========================================================================
    // FxRackStrip — the mini CUE RACK living at the bottom of the CUESAMPLER.
    //
    // A single-row port of CUERACK's dynamic rack editor:
    //  - panels lay out left-to-right in a weighted row
    //  - drag any panel by its header to reorder (live animated preview)
    //  - remove panels with the header "x" (module powers off); unused panels
    //    live in the toolbar below as animated chips — click to restore
    //  - panel order + hidden set persist inside the plugin state (APVTS)
    //
    // The strip carries CUERACK's own LookAndFeel for its subtree, so the
    // modules render exactly as they do in the rack. Implementation lives in
    // FxRackStrip.cpp to keep CUERACK's UI headers out of PluginEditor.cpp.
    // ========================================================================
    struct FxRackMeterHooks
    {
        std::function<float()> compGainReductionDb;
        std::function<int()> limiterNumBands;
        std::function<float (int)> limiterBandGRDb;
        std::function<juce::Point<float>()> imagerMidSide;   // x = mid, y = side
    };

    class FxRackStrip final : public juce::Component
    {
    public:
        FxRackStrip (juce::AudioProcessorValueTreeState& state, const FxRackMeterHooks& hooks);
        ~FxRackStrip() override;

        void paint (juce::Graphics&) override;
        void resized() override;

        // Bridge to CUERACK's theme system (cue::colours foundation used by
        // the rack subtree), then re-skin the panels/toolbar.
        static void applyRackTheme (bool light, bool warpActive = false, bool halftimeActive = false);
        void refreshColours();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxRackStrip)
    };
}
