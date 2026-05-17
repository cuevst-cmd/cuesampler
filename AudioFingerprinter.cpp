#include "AudioFingerprinter.h"

#include <chromaprint.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
// Encodes one field value for application/x-www-form-urlencoded requests.
juce::String formEncode (const juce::String& value)
{
    return juce::URL::addEscapeChars (value, true);
}

// Reads a URL response as text with a bounded blocking timeout.
juce::String readUrlText (const juce::URL& url,
                          const juce::String& httpCommand,
                          const juce::String& extraHeaders,
                          int timeoutMs)
{
    int statusCode = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withHttpRequestCmd (httpCommand)
                                             .withExtraHeaders (extraHeaders)
                                             .withConnectionTimeoutMs (timeoutMs)
                                             .withStatusCode (&statusCode));

    if (stream == nullptr)
        return {};

    const auto responseText = stream->readEntireStreamAsString();
    if (statusCode != 0 && (statusCode < 200 || statusCode >= 300))
        return {};

    return responseText;
}

// Extracts the tonal object from the AcousticBrainz bulk low-level response.
juce::DynamicObject* findTonalObjectForMbid (juce::DynamicObject& rootObject, const juce::String& mbid)
{
    auto recordingValue = rootObject.getProperty (mbid);

    if (recordingValue.isVoid())
        recordingValue = rootObject.getProperty (mbid.toLowerCase());

    auto* recordingObject = recordingValue.getDynamicObject();
    if (recordingObject == nullptr)
        return nullptr;

    auto* offsetObject = recordingObject->getProperty ("0").getDynamicObject();
    if (offsetObject == nullptr)
        return nullptr;

    return offsetObject->getProperty ("tonal").getDynamicObject();
}
}

// Generates a Chromaprint fingerprint from mono-summed int16 PCM audio.
std::string AudioFingerprinter::generateFingerprint (const juce::AudioBuffer<float>& buffer,
                                                     double sampleRate,
                                                     int& durationOut)
{
    durationOut = 0;

    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0)
        return {};

    durationOut = static_cast<int> (static_cast<double> (numSamples) / sampleRate);

    std::vector<int16_t> pcm;
    pcm.resize (static_cast<size_t> (numSamples));

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        double monoSample = 0.0;

        for (int channel = 0; channel < numChannels; ++channel)
            monoSample += static_cast<double> (buffer.getSample (channel, sampleIndex));

        monoSample /= static_cast<double> (numChannels);

        const auto scaledSample = monoSample * 32767.0;
        const auto clampedSample = std::clamp (scaledSample, -32768.0, 32767.0);
        pcm[static_cast<size_t> (sampleIndex)] = static_cast<int16_t> (clampedSample);
    }

    auto* ctx = chromaprint_new (CHROMAPRINT_ALGORITHM_DEFAULT);
    if (ctx == nullptr)
        return {};

    std::string fingerprint;

    if (chromaprint_start (ctx, static_cast<int> (sampleRate), 1) == 1
        && chromaprint_feed (ctx, pcm.data(), static_cast<int> (pcm.size())) == 1
        && chromaprint_finish (ctx) == 1)
    {
        char* fpStr = nullptr;
        if (chromaprint_get_fingerprint (ctx, &fpStr) == 1 && fpStr != nullptr)
        {
            fingerprint = fpStr;
            chromaprint_dealloc (fpStr);
        }
    }

    chromaprint_free (ctx);
    return fingerprint;
}

// Looks up the first MusicBrainz recording ID for a Chromaprint fingerprint.
std::string AudioFingerprinter::lookupMbid (const std::string& fingerprint, int duration)
{
    if (fingerprint.empty() || duration <= 0)
        return {};

    juce::String postData;
    postData << "client=" << formEncode (kAcoustIdApiKey)
             << "&duration=" << duration
             << "&fingerprint=" << formEncode (juce::String (fingerprint))
             << "&meta=recordings";

    const auto responseText = readUrlText (juce::URL ("https://api.acoustid.org/v2/lookup").withPOSTData (postData),
                                           "POST",
                                           "Content-Type: application/x-www-form-urlencoded\r\n",
                                           5000);
    if (responseText.isEmpty())
        return {};

    const auto parsedJson = juce::JSON::parse (responseText);
    auto* rootObject = parsedJson.getDynamicObject();
    if (rootObject == nullptr)
        return {};

    auto* resultsArray = rootObject->getProperty ("results").getArray();
    if (resultsArray == nullptr || resultsArray->isEmpty())
        return {};

    for (const auto& resultValue : *resultsArray)
    {
        auto* resultObject = resultValue.getDynamicObject();
        if (resultObject == nullptr)
            continue;

        auto* recordingsArray = resultObject->getProperty ("recordings").getArray();
        if (recordingsArray == nullptr || recordingsArray->isEmpty())
            continue;

        auto* recordingObject = recordingsArray->getFirst().getDynamicObject();
        if (recordingObject == nullptr)
            continue;

        const auto mbid = recordingObject->getProperty ("id").toString().trim();
        if (mbid.isNotEmpty())
            return mbid.toStdString();
    }

    return {};
}

// Looks up musical key data from AcousticBrainz for a MusicBrainz recording ID.
AudioFingerprinter::OnlineResult AudioFingerprinter::lookupKey (const std::string& mbid)
{
    OnlineResult result;

    if (mbid.empty())
        return result;

    juce::String urlString;
    urlString << "https://acousticbrainz.org/api/v1/low-level?recording_ids="
              << juce::URL::addEscapeChars (juce::String (mbid), true)
              << "&features=tonal.key_key;tonal.key_scale";

    const auto responseText = readUrlText (juce::URL (urlString), "GET", {}, 3000);
    if (responseText.isEmpty())
        return result;

    const auto parsedJson = juce::JSON::parse (responseText);
    auto* rootObject = parsedJson.getDynamicObject();
    if (rootObject == nullptr)
        return result;

    auto* tonalObject = findTonalObjectForMbid (*rootObject, juce::String (mbid));
    if (tonalObject == nullptr)
        return result;

    const auto keyKey = tonalObject->getProperty ("key_key").toString().trim();
    const auto keyScale = tonalObject->getProperty ("key_scale").toString().trim().toLowerCase();

    if (keyKey.isEmpty() || keyScale.isEmpty())
        return result;

    result.found = true;
    result.mbid = mbid;
    result.rootNote = keyKey.toStdString();
    result.isMajor = keyScale == "major";
    result.key = (keyKey + (result.isMajor ? juce::String() : "m")).toStdString();

    return result;
}

// Performs the full blocking fingerprint, AcoustID, and AcousticBrainz lookup chain.
AudioFingerprinter::OnlineResult AudioFingerprinter::lookup (const juce::AudioBuffer<float>& buffer,
                                                             double sampleRate)
{
    int duration = 0;
    const auto fingerprint = generateFingerprint (buffer, sampleRate, duration);
    const auto mbid = lookupMbid (fingerprint, duration);
    return lookupKey (mbid);
}
