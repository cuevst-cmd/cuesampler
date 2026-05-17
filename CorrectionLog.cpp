#include "CorrectionLog.h"

#include <juce_cryptography/juce_cryptography.h>

juce::File getCorrectionsLogFile()
{
    auto correctionsDirectory = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                    .getChildFile ("CueSampler")
                                    .getChildFile ("corrections");
    correctionsDirectory.createDirectory();

    return correctionsDirectory.getChildFile ("corrections.jsonl");
}

juce::String computeAudioHash (const juce::AudioBuffer<float>& buffer)
{
    juce::MemoryBlock block;

    if (buffer.getNumChannels() > 0 && buffer.getNumSamples() > 0)
        block = juce::MemoryBlock (buffer.getReadPointer (0),
                                   (size_t) buffer.getNumSamples() * sizeof (float));

    juce::SHA256 hash (block);
    return hash.toHexString();
}

int countSavedCorrections()
{
    const auto correctionsFile = getCorrectionsLogFile();
    if (! correctionsFile.existsAsFile())
        return 0;

    juce::StringArray lines;
    lines.addLines (correctionsFile.loadFileAsString());

    int count = 0;
    for (const auto& line : lines)
    {
        if (line.trim().isNotEmpty())
            ++count;
    }

    return count;
}

void saveTempoCorrection (const juce::File& audioFile,
                          const juce::AudioBuffer<float>& audioBuffer,
                          double detectedBpm,
                          double correctedBpm,
                          double sampleRate)
{
    juce::var record (new juce::DynamicObject());
    auto* object = record.getDynamicObject();
    jassert (object != nullptr);

    const auto durationSeconds = sampleRate > 0.0
                               ? (double) audioBuffer.getNumSamples() / sampleRate
                               : 0.0;

    object->setProperty ("timestamp", juce::Time::getCurrentTime().toISO8601 (true));
    object->setProperty ("audio_path", audioFile.getFullPathName());
    object->setProperty ("audio_hash", computeAudioHash (audioBuffer));
    object->setProperty ("detected_bpm", detectedBpm);
    object->setProperty ("corrected_bpm", correctedBpm);
    object->setProperty ("sample_rate", sampleRate);
    object->setProperty ("duration_seconds", durationSeconds);

    auto jsonLine = juce::JSON::toString (record, true).removeCharacters ("\r\n");
    getCorrectionsLogFile().appendText (jsonLine + "\n", false, false, nullptr);
}
