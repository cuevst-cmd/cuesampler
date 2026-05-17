#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <string>

class AudioFingerprinter
{
public:
    struct OnlineResult
    {
        bool found = false;
        std::string key;
        std::string rootNote;
        bool isMajor = true;
        std::string mbid;
        std::string title;
        std::string artist;
    };

    static constexpr const char* kAcoustIdApiKey = "eJk2XKFZNi";

    // Performs a blocking online lookup from audio fingerprint to musical key.
    OnlineResult lookup (const juce::AudioBuffer<float>& buffer, double sampleRate);

private:
    // Generates a Chromaprint fingerprint and returns the integer duration in seconds.
    std::string generateFingerprint (const juce::AudioBuffer<float>& buffer, double sampleRate, int& durationOut);

    // Looks up the best MusicBrainz recording ID for an AcoustID fingerprint.
    std::string lookupMbid (const std::string& fingerprint, int duration);

    // Looks up musical-key information for a MusicBrainz recording ID.
    OnlineResult lookupKey (const std::string& mbid);
};
