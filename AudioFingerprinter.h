#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <string>

#if __has_include("APIKeys.h")
    #include "APIKeys.h"
#else
    namespace APIKeys { static constexpr const char* AcoustIdApiKey = ""; }
#endif

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

    static constexpr const char* kAcoustIdApiKey = APIKeys::AcoustIdApiKey;

    // Performs a blocking online lookup from audio fingerprint to musical key.
    OnlineResult lookup (const juce::AudioBuffer<float>& buffer, double sampleRate);

    // Online lookup from a precomputed Chromaprint fingerprint (avoids
    // recomputing it when the caller already has one).
    OnlineResult lookup (const std::string& fingerprint, int duration);

    // Computes the Chromaprint fingerprint locally (no network). Returns the
    // fingerprint string and writes the integer duration in seconds to
    // durationOut. Empty on failure.
    std::string localFingerprint (const juce::AudioBuffer<float>& buffer, double sampleRate, int& durationOut);

private:
    // Generates a Chromaprint fingerprint and returns the integer duration in seconds.
    std::string generateFingerprint (const juce::AudioBuffer<float>& buffer, double sampleRate, int& durationOut);

    // Looks up the best MusicBrainz recording ID for an AcoustID fingerprint.
    std::string lookupMbid (const std::string& fingerprint, int duration);

    // Looks up musical-key information for a MusicBrainz recording ID.
    OnlineResult lookupKey (const std::string& mbid);
};
