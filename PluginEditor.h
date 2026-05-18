#pragma once

#include "PluginProcessor.h"

#include <memory>

namespace cue
{
class CueSamplerLookAndFeel;
class HelpOverlayComponent;
class HeaderComponent;
class WaveformDisplayComponent;
class TransportSectionComponent;
class UtilityStripComponent;
class EffectsRackComponent;
class StartKnobComponent;
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

    void paintSideRail (juce::Graphics&, juce::Rectangle<int>, bool isLeftRail) const;
    void paintScrew (juce::Graphics&, juce::Point<float>) const;

    AudioPluginAudioProcessor& processorRef;

    juce::Component contentComponent;

    std::unique_ptr<cue::CueSamplerLookAndFeel> lookAndFeel;
    std::unique_ptr<cue::HeaderComponent> headerComponent;
    std::unique_ptr<cue::WaveformDisplayComponent> waveformDisplayComponent;
    std::unique_ptr<cue::TransportSectionComponent> transportSectionComponent;
    std::unique_ptr<cue::UtilityStripComponent> utilityStripComponent;
    std::unique_ptr<cue::EffectsRackComponent> effectsRackComponent;

    std::unique_ptr<cue::HelpOverlayComponent> helpOverlayComponent;
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::TooltipWindow tooltipWindow { this, 500 };
    juce::DropShadow defaultShadow { juce::Colours::black.withAlpha (0.6f), 12, { 0, 5 } };
    juce::DropShadowEffect panelShadowEffect;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessorEditor)
};
