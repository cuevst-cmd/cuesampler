#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

juce::File getCorrectionsLogFile();

juce::String computeAudioHash (const juce::AudioBuffer<float>& buffer);

int countSavedCorrections();

void saveTempoCorrection (const juce::File& audioFile,
                          const juce::AudioBuffer<float>& audioBuffer,
                          double detectedBpm,
                          double correctedBpm,
                          double sampleRate);
