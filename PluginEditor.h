#pragma once

#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

namespace cue
{
class CueSamplerLookAndFeel;
class HelpOverlayComponent;
class HeaderComponent;
class VisualizerOrb;
class WaveformDisplayComponent;
class TransportSectionComponent;
class UtilityStripComponent;
class FxRackStrip;
class StemRackComponent;
class StartKnobComponent;
class UpdateBannerComponent;
class GlassKeyboard;
}

//==============================================================================
class AudioPluginAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                              public juce::ChangeListener,
                                              private juce::Timer
{
public:
    explicit AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor&);
    ~AudioPluginAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    void loadSampleFromFile();

    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

private:
    float getUiScale() const noexcept;
    int getUiFluidWidth() const noexcept;   // design-space width the layout fills

    // Reveals the "new version available" banner from the update checker's
    // result. Idempotent: only builds/shows the banner once per session.
    void showUpdateBannerIfNeeded();

    // Light/dark theme swap (CUERACK-style). The choice persists in a
    // per-user settings file; plugin state is untouched.
    void toggleTheme();
    void applyThemeToUi();

    AudioPluginAudioProcessor& processorRef;

    juce::Component contentComponent;

    std::unique_ptr<cue::CueSamplerLookAndFeel> lookAndFeel;
    std::unique_ptr<cue::HeaderComponent> headerComponent;
    std::unique_ptr<cue::WaveformDisplayComponent> waveformDisplayComponent;
    std::unique_ptr<cue::TransportSectionComponent> transportSectionComponent;
    std::unique_ptr<cue::UtilityStripComponent> utilityStripComponent;
    std::unique_ptr<cue::FxRackStrip> fxRackStrip;
    std::unique_ptr<cue::StemRackComponent> stemRackComponent;

    std::unique_ptr<cue::HelpOverlayComponent> helpOverlayComponent;
    std::unique_ptr<cue::UpdateBannerComponent> updateBannerComponent;
    std::unique_ptr<cue::GlassKeyboard> midiKeyboardComponent;
    std::unique_ptr<cue::VisualizerOrb> cueOrbComponent;
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::TooltipWindow tooltipWindow { this, 500 };
    juce::DropShadow defaultShadow { juce::Colours::black.withAlpha (0.6f), 12, { 0, 5 } };
    juce::DropShadowEffect panelShadowEffect;

    // Measures the real display refresh rate (60/120) so animations run at the
    // display's native frame rate. See cue::observeVBlankInterval.
    juce::VBlankAttachment vblankRateMeter;
    double lastVBlankSeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessorEditor)
};
