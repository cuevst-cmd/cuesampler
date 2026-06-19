#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <bungee/Stream.h>

#include <array>

//==============================================================================
// Definition of the per-voice pitch/time engine that PluginProcessor.h
// forward-declares. Keeping it here means the bungee/Eigen template heaps
// don't bleed into PluginEditor.cpp.
struct AudioPluginAudioProcessor::VoicePitchEngine
{
    std::unique_ptr<Bungee::Stretcher<Bungee::Basic>> stretcher;
    std::unique_ptr<Bungee::Stream<Bungee::Basic>>    stream;
    int    channelCount     = 0;
    int    inputSampleRate  = 0;
    int    outputSampleRate = 0;

    // Scratch buffers used while feeding source samples through the stream.
    // Sized once at construction so the audio thread never allocates.
    juce::AudioBuffer<float> scratchInput;   // chunked source frames into bungee
    juce::AudioBuffer<float> discardOutput;  // priming output we throw away
};

struct AudioPluginAudioProcessor::VoicePitchEngineSet
{
    uint64_t generation = 0;
    int inputSampleRate = 0;
    int outputSampleRate = 0;
    int channelCount = 0;
    std::array<std::unique_ptr<VoicePitchEngine>, 2> voices;
};

AudioPluginAudioProcessor::VoiceState::VoiceState()  = default;
AudioPluginAudioProcessor::VoiceState::~VoiceState() = default;

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace
{
// Bit Crusher UI knobs run 0..100 (% intensity). The DSP works in real units
// (bit depth + surviving sample-rate percentage), so map at the boundary.
// Both knobs use a square-root curve so the perceptually-loud end of each
// range is reachable in the upper half of the knob instead of being crammed
// into the last 5 %:
//   BITS  amount 0   → bits 16   (clean)
//         amount 25  → bits 8.5  (clearly grainy)
//         amount 50  → bits 5.4  (lo-fi)
//         amount 100 → bits 1    (1-bit square)
//   CRUSH amount 0   → crush 100 % (full sample rate)
//         amount 25  → crush ~50 % (audibly aliased)
//         amount 50  → crush ~30 % (heavy lo-fi)
//         amount 100 → crush 1 %   (severe sample-rate reduction)
inline float bitsAmountToDspBits (float amount) noexcept
{
    const auto a = juce::jlimit (0.0f, 100.0f, amount);
    return 16.0f - 15.0f * std::sqrt (a * 0.01f);
}

inline float crushAmountToDspPercent (float amount) noexcept
{
    const auto a = juce::jlimit (0.0f, 100.0f, amount);
    return 100.0f - 99.0f * std::sqrt (a * 0.01f);
}

constexpr double tempoAnalysisTargetRate = 200.0;
constexpr double maximumTempoAnalysisSeconds = 120.0;
constexpr int maximumLoadedChannels = 2;
constexpr int maximumLoadedSampleFrames = 44100 * 60 * 10;
constexpr size_t maximumEmbeddedSampleBytes = 128ull * 1024ull * 1024ull;
constexpr int maximumRestoredSequenceItems = 4096;
constexpr int maximumRestoredChops = 256;
constexpr int maximumRestoredWarpMarkersPerChop = 64;
constexpr double representativeTempoWindowSeconds = 24.0;
constexpr double representativeWindowStepSeconds = 4.0;
constexpr double leadingSilenceWindowSeconds = 0.01;
constexpr double leadingSilencePreRollSeconds = 0.005;
constexpr float minimumLeadingPeakThreshold = 1.0e-4f;
constexpr float minimumLeadingMeanThreshold = 3.0e-5f;
constexpr float relativeLeadingPeakThresholdScale = 0.015f;
constexpr float relativeLeadingMeanThresholdScale = 0.005f;
constexpr float strongClassicTempoConfidence = 0.5f;
constexpr float strongNeuralTempoConfidence = 0.55f;
constexpr double neuralAgreementTolerance = 0.04;
constexpr double neuralSelectionScoreMargin = 0.08;
constexpr int representativeConsensusWindowCount = 4;
constexpr int localTempoCandidatesPerWindow = 4;
constexpr double representativeWindowOverlapRatio = 0.5;
constexpr double tempoConsensusTolerance = 0.06;
constexpr double harmonicMappedSupportPenalty = 0.88;
constexpr double halfDoubleScoreMargin = 0.025;
constexpr double minimumTempoBpm = 55.0;
constexpr double maximumTempoBpm = 210.0;
constexpr double preferredTempoLow = 72.0;
constexpr double preferredTempoHigh = 160.0;
constexpr int midiRootNote = 36; // C2 — chop 0
constexpr int defaultSliceStartFadeSamples = 64;
// MIDI chops need a near-instant front edge; keep only a tiny de-click ramp.
constexpr int midiSliceStartFadeSamples = 4;
constexpr int sliceEndFadeSamples = 16;
constexpr int midiVoiceReleaseSamples = 32;
constexpr double autoCueSearchSeconds = 0.20;
constexpr double autoCueWindowSeconds = 0.002;
constexpr double autoCuePreRollSeconds = 0.0005;
constexpr float autoCueRelativePeakThreshold = 0.025f;
constexpr float autoCueMinimumPeakThreshold = 8.0e-5f;
constexpr char cueSamplerStateMagic[] = "CSB2";

constexpr std::array<const char*, 12> metadataMajorCamelot
{
    "8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B"
};

constexpr std::array<const char*, 12> metadataMinorCamelot
{
    "5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A"
};

constexpr std::array<const char*, 12> camelotMinorKeys
{
    "Abm", "Ebm", "Bbm", "Fm", "Cm", "Gm", "Dm", "Am", "Em", "Bm", "F#m", "C#m"
};

constexpr std::array<const char*, 12> camelotMajorKeys
{
    "B", "F#", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E"
};

// Converts a Camelot wheel code into a conventional key string.
juce::String camelotCodeToKeyString (const juce::String& text)
{
    const auto normalized = text.trim().toUpperCase();

    if (normalized.length() < 2 || normalized.length() > 3)
        return {};

    const auto mode = normalized.getLastCharacter();
    if (mode != 'A' && mode != 'B')
        return {};

    const auto number = normalized.dropLastCharacters (1).getIntValue();
    if (number < 1 || number > 12)
        return {};

    return mode == 'A' ? camelotMinorKeys[static_cast<size_t> (number - 1)]
                       : camelotMajorKeys[static_cast<size_t> (number - 1)];
}

// Returns the pitch-class index for a parsed note spelling.
int rootIndexForNoteName (juce::juce_wchar root, juce::juce_wchar accidental) noexcept
{
    switch (root)
    {
        case 'C': return accidental == '#' ? 1 : accidental == 'B' ? 11 : 0;
        case 'D': return accidental == '#' ? 3 : accidental == 'B' ? 1 : 2;
        case 'E': return accidental == '#' ? 5 : accidental == 'B' ? 3 : 4;
        case 'F': return accidental == '#' ? 6 : accidental == 'B' ? 4 : 5;
        case 'G': return accidental == '#' ? 8 : accidental == 'B' ? 6 : 7;
        case 'A': return accidental == '#' ? 10 : accidental == 'B' ? 8 : 9;
        case 'B': return accidental == '#' ? 0 : accidental == 'B' ? 10 : 11;
        default: break;
    }

    return -1;
}

// Removes common separators from a key-mode suffix.
juce::String compactKeyModeSuffix (juce::String suffix)
{
    suffix = suffix.trim();
    suffix = suffix.removeCharacters (" \t\r\n-_()");
    return suffix;
}

// Parses a text key value into a complete key-detection result.
KeyDetector::Result parseMetadataKeyString (const juce::String& rawText)
{
    KeyDetector::Result result;

    auto text = rawText.trim();
    if (text.isEmpty())
        return result;

    if (const auto camelotKey = camelotCodeToKeyString (text); camelotKey.isNotEmpty())
        text = camelotKey;

    const auto upperText = text.toUpperCase();
    const auto root = upperText[0];

    if (root < 'A' || root > 'G')
        return result;

    auto accidental = juce::juce_wchar {};
    auto rootLength = 1;

    if (upperText.length() > 1 && (upperText[1] == '#' || upperText[1] == 'B'))
    {
        accidental = upperText[1];
        rootLength = 2;
    }

    const auto rootIndex = rootIndexForNoteName (root, accidental);
    if (rootIndex < 0)
        return result;

    juce::String rootNote;
    rootNote << juce::String::charToString (root);
    if (accidental == '#')
        rootNote << "#";
    else if (accidental == 'B')
        rootNote << "b";

    const auto modeSuffix = compactKeyModeSuffix (text.substring (rootLength));
    const auto upperModeSuffix = modeSuffix.toUpperCase();

    auto isMajor = true;
    if (modeSuffix == "m" || upperModeSuffix.startsWith ("MIN"))
        isMajor = false;

    result.valid = true;
    result.rootNote = rootNote.toStdString();
    result.isMajor = isMajor;
    result.rootIndex = rootIndex;
    result.key = (rootNote + (isMajor ? juce::String() : "m")).toStdString();
    result.confidence = 1.0f;
    result.camelot = isMajor ? metadataMajorCamelot[static_cast<size_t> (rootIndex)]
                             : metadataMinorCamelot[static_cast<size_t> (rootIndex)];

    return result;
}

double getTempoPreferenceWeight (double bpm) noexcept
{
    if (bpm >= preferredTempoLow && bpm <= preferredTempoHigh)
        return 1.0;

    if (bpm < preferredTempoLow)
        return juce::jmap (bpm, minimumTempoBpm, preferredTempoLow, 0.82, 1.0);

    return juce::jmap (bpm, preferredTempoHigh, maximumTempoBpm, 1.0, 0.86);
}

bool isSupportedReaderShape (const juce::AudioFormatReader& reader) noexcept
{
    return std::isfinite (reader.sampleRate)
        && reader.sampleRate >= 8000.0
        && reader.sampleRate <= 384000.0
        && reader.numChannels > 0
        && reader.numChannels <= (unsigned int) maximumLoadedChannels
        && reader.lengthInSamples > 0
        && reader.lengthInSamples <= (juce::int64) maximumLoadedSampleFrames;
}

std::shared_ptr<AudioPluginAudioProcessor::LoadedSampleData>
readValidatedSampleData (juce::AudioFormatReader& reader)
{
    if (! isSupportedReaderShape (reader))
        return {};

    const auto numSamples = static_cast<int> (reader.lengthInSamples);
    const auto numChannels = static_cast<int> (reader.numChannels);

    auto sampleData = std::make_shared<AudioPluginAudioProcessor::LoadedSampleData>();
    sampleData->buffer.setSize (numChannels, numSamples, false, true, true);

    if (! reader.read (&sampleData->buffer, 0, numSamples, 0, true, true))
        return {};

    sampleData->sampleRate = reader.sampleRate;
    sampleData->leadingContentStartSample = 0;
    return sampleData;
}

const AudioPluginAudioProcessor::ChopDefinition* findSelectedChop (const AudioPluginAudioProcessor::ChopState* state) noexcept
{
    if (state == nullptr || state->selectedChopId < 0)
        return nullptr;

    for (const auto& chop : state->chops)
    {
        if (chop.id == state->selectedChopId)
            return &chop;
    }

    return nullptr;
}

double wrapToLoopRange (double position, double loopStart, double loopLength) noexcept
{
    if (loopLength <= 0.0)
        return loopStart;

    auto wrapped = std::fmod (position - loopStart, loopLength);
    if (wrapped < 0.0)
        wrapped += loopLength;

    return loopStart + wrapped;
}

int reflectSampleIndex (int index, int sourceLength) noexcept
{
    if (sourceLength <= 1)
        return 0;

    const auto period = 2 * (sourceLength - 1);
    auto wrappedIndex = index % period;

    if (wrappedIndex < 0)
        wrappedIndex += period;

    if (wrappedIndex >= sourceLength)
        wrappedIndex = period - wrappedIndex;

    return wrappedIndex;
}

double sinc (double x) noexcept
{
    constexpr double pi = 3.14159265358979323846;

    if (std::abs (x) <= 1.0e-12)
        return 1.0;

    const auto pix = pi * x;
    return std::sin (pix) / pix;
}

float computeSliceBoundaryGain (double sourcePosition,
                                double playbackStartSample,
                                double playbackStopSample,
                                int startFadeLengthSamples,
                                int endFadeLengthSamples) noexcept
{
    if (playbackStopSample <= playbackStartSample)
        return 1.0f;

    const auto sliceLength = playbackStopSample - playbackStartSample;
    float gain = 1.0f;

    if (startFadeLengthSamples > 0)
    {
        const auto availableStartFade = juce::jmin ((double) startFadeLengthSamples,
                                                    juce::jmax (1.0, sliceLength * 0.5));
        if (availableStartFade > 1.0 && sourcePosition < playbackStartSample + availableStartFade)
            gain = juce::jmin (gain, (float) juce::jlimit (0.0, 1.0, (sourcePosition - playbackStartSample) / availableStartFade));
    }

    if (endFadeLengthSamples > 0)
    {
        const auto availableEndFade = juce::jmin ((double) endFadeLengthSamples,
                                                  juce::jmax (1.0, sliceLength * 0.5));
        if (availableEndFade > 1.0 && sourcePosition > playbackStopSample - availableEndFade)
            gain = juce::jmin (gain, (float) juce::jlimit (0.0, 1.0, (playbackStopSample - sourcePosition) / availableEndFade));
    }

    return gain;
}

float interpolateSampleLanczos (const float* sourceData, int sourceLength, double position, int filterRadius = 3) noexcept
{
    if (sourceData == nullptr || sourceLength <= 0)
        return 0.0f;
    if (filterRadius <= 0)
        return sourceData[reflectSampleIndex ((int) std::round (position), sourceLength)];

    const auto centre = (int) std::floor (position);
    double sum = 0.0;
    double weightSum = 0.0;

    for (int tap = centre - filterRadius + 1; tap <= centre + filterRadius; ++tap)
    {
        const auto distance = position - (double) tap;
        const auto normalizedDistance = std::abs (distance) / (double) filterRadius;
        if (normalizedDistance >= 1.0)
            continue;

        const auto weight = sinc (distance) * sinc (distance / (double) filterRadius);
        const auto sampleIndex = reflectSampleIndex (tap, sourceLength);

        sum += (double) sourceData[sampleIndex] * weight;
        weightSum += weight;
    }

    if (std::abs (weightSum) <= 1.0e-9)
        return sourceData[reflectSampleIndex (centre, sourceLength)];

    return (float) juce::jlimit (-1.0, 1.0, sum / weightSum);
}

juce::ValueTree createSequenceTree (const juce::Identifier& parentType,
                                    const juce::Identifier& itemType,
                                    const juce::Identifier& valueProperty,
                                    const std::vector<double>& values)
{
    juce::ValueTree tree (parentType);

    for (const auto value : values)
    {
        juce::ValueTree item (itemType);
        item.setProperty (valueProperty, value, nullptr);
        tree.addChild (item, -1, nullptr);
    }

    return tree;
}

std::vector<double> parseDoubleSequenceTree (const juce::ValueTree& parent,
                                             const juce::Identifier& valueProperty)
{
    std::vector<double> values;
    values.reserve ((size_t) juce::jmin (parent.getNumChildren(), maximumRestoredSequenceItems));

    for (const auto child : parent)
    {
        if ((int) values.size() >= maximumRestoredSequenceItems)
            break;

        const auto value = (double) child.getProperty (valueProperty, 0.0);
        if (std::isfinite (value) && value >= 0.0)
            values.push_back (value);
    }

    return values;
}

const AudioPluginAudioProcessor::ChopDefinition* findChopAtSample (const AudioPluginAudioProcessor::ChopState* state,
                                                                   double samplePosition) noexcept
{
    if (state == nullptr)
        return nullptr;

    for (const auto& chop : state->chops)
    {
        if (samplePosition >= (double) chop.startSample && samplePosition < (double) chop.endSample)
            return &chop;
    }

    return nullptr;
}

double mapChopTimelineToSourceSample (const AudioPluginAudioProcessor::ChopDefinition* chop,
                                      double timelineSample,
                                      double sampleRate) noexcept
{
    if (chop == nullptr || sampleRate <= 0.0 || chop->endSample <= chop->startSample)
        return timelineSample;

    if (chop->warpMarkers.empty())
        return timelineSample;

    const auto localSeconds = juce::jlimit (0.0,
                                            (double) (chop->endSample - chop->startSample) / sampleRate,
                                            (timelineSample - (double) chop->startSample) / sampleRate);

    double previousLocalSeconds = 0.0;
    double previousSourceSample = (double) chop->startSample;

    for (const auto& marker : chop->warpMarkers)
    {
        const auto markerLocalSeconds = juce::jlimit (0.0,
                                                      (double) (chop->endSample - chop->startSample) / sampleRate,
                                                      marker.localTimeSeconds);
        const auto markerSourceSample = juce::jlimit ((double) chop->startSample,
                                                      (double) chop->endSample,
                                                      (double) marker.sourceSample);

        if (localSeconds <= markerLocalSeconds)
        {
            const auto span = markerLocalSeconds - previousLocalSeconds;
            if (span <= 1.0e-9)
                return markerSourceSample;

            const auto alpha = (localSeconds - previousLocalSeconds) / span;
            return previousSourceSample + (markerSourceSample - previousSourceSample) * alpha;
        }

        previousLocalSeconds = markerLocalSeconds;
        previousSourceSample = markerSourceSample;
    }

    const auto endLocalSeconds = (double) (chop->endSample - chop->startSample) / sampleRate;
    const auto endSourceSample = (double) chop->endSample;
    const auto span = endLocalSeconds - previousLocalSeconds;
    if (span <= 1.0e-9)
        return endSourceSample;

    const auto alpha = (localSeconds - previousLocalSeconds) / span;
    return previousSourceSample + (endSourceSample - previousSourceSample) * alpha;
}

struct AnalysisWindow
{
    int startSample = 0;
    int endSample = 0;
};

struct TempoLagCandidate
{
    int lag = 0;
    double bpm = 0.0;
    double score = 0.0;
};

struct TempoWindowCandidateSet
{
    int startFrame = 0;
    int endFrame = 0;
    double bestScore = 0.0;
    std::vector<TempoLagCandidate> candidates;
};

struct TempoConsensusChoice
{
    bool valid = false;
    double bpm = 0.0;
    double score = 0.0;
    double normalizedSupport = 0.0;
    int strongestWindowIndex = 0;
    int supportingWindows = 0;
    int directSupportWindows = 0;
};

struct TempoHypothesisEvaluation
{
    bool valid = false;
    double bpm = 0.0;
    double score = 0.0;
    float confidence = 0.0f;
    bool likelyDrifting = false;
    double beatPeriodSeconds = 0.0;
    double firstBeatSeconds = 0.0;
    int downbeatPhase = 0;
    std::vector<double> beatPositionsSeconds;
    std::vector<double> barPositionsSeconds;
};

int findLeadingContentStartSample (const AudioPluginAudioProcessor::LoadedSampleData& sampleData,
                                   int startSample,
                                   int endSample) noexcept
{
    const auto numChannels = sampleData.buffer.getNumChannels();
    if (numChannels <= 0 || endSample - startSample <= 1 || sampleData.sampleRate <= 0.0)
        return startSample;

    float globalPeak = 0.0f;
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const auto* channelData = sampleData.buffer.getReadPointer (channel);
        for (int sampleIndex = startSample; sampleIndex < endSample; ++sampleIndex)
            globalPeak = juce::jmax (globalPeak, std::abs (channelData[sampleIndex]));
    }

    if (globalPeak <= 1.0e-5f)
        return startSample;

    const auto windowSize = juce::jmax (16, (int) std::round (sampleData.sampleRate * leadingSilenceWindowSeconds));
    const auto preRollSamples = juce::jmax (0, (int) std::round (sampleData.sampleRate * leadingSilencePreRollSeconds));
    const auto peakThreshold = juce::jmax (minimumLeadingPeakThreshold, globalPeak * relativeLeadingPeakThresholdScale);
    const auto meanThreshold = juce::jmax (minimumLeadingMeanThreshold, globalPeak * relativeLeadingMeanThresholdScale);

    for (int windowStart = startSample; windowStart < endSample; windowStart += windowSize)
    {
        const auto windowEnd = juce::jmin (windowStart + windowSize, endSample);
        double meanAbsolute = 0.0;
        float windowPeak = 0.0f;
        int frameCount = 0;

        for (int sampleIndex = windowStart; sampleIndex < windowEnd; ++sampleIndex)
        {
            double monoAbsolute = 0.0;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const auto currentSample = sampleData.buffer.getSample (channel, sampleIndex);
                monoAbsolute += std::abs (currentSample);
                windowPeak = juce::jmax (windowPeak, std::abs (currentSample));
            }

            meanAbsolute += monoAbsolute / (double) numChannels;
            ++frameCount;
        }

        if (frameCount <= 0)
            continue;

        meanAbsolute /= (double) frameCount;

        if (windowPeak >= peakThreshold || meanAbsolute >= meanThreshold)
        {
            return juce::jmax (startSample, windowStart - preRollSamples);
        }
    }

    return startSample;
}

int findAutoCueStartSample (const AudioPluginAudioProcessor::LoadedSampleData& sampleData,
                            int startSample,
                            int endSample) noexcept
{
    const auto numChannels = sampleData.buffer.getNumChannels();
    if (numChannels <= 0 || endSample - startSample <= 1 || sampleData.sampleRate <= 0.0)
        return startSample;

    const auto maxSearchSamples = juce::jmax (1, (int) std::round (sampleData.sampleRate * autoCueSearchSeconds));
    const auto searchEnd = juce::jmin (endSample, startSample + maxSearchSamples);
    if (searchEnd <= startSample + 1)
        return startSample;

    float searchPeak = 0.0f;
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const auto* channelData = sampleData.buffer.getReadPointer (channel);
        for (int sampleIndex = startSample; sampleIndex < searchEnd; ++sampleIndex)
            searchPeak = juce::jmax (searchPeak, std::abs (channelData[sampleIndex]));
    }

    if (searchPeak <= 1.0e-6f)
        return startSample;

    const auto threshold = juce::jmax (autoCueMinimumPeakThreshold,
                                       searchPeak * autoCueRelativePeakThreshold);
    const auto windowSize = juce::jmax (8, (int) std::round (sampleData.sampleRate * autoCueWindowSeconds));
    const auto preRollSamples = juce::jmax (0, (int) std::round (sampleData.sampleRate * autoCuePreRollSeconds));

    for (int windowStart = startSample; windowStart < searchEnd; windowStart += windowSize)
    {
        const auto windowEnd = juce::jmin (windowStart + windowSize, searchEnd);
        float windowPeak = 0.0f;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto* channelData = sampleData.buffer.getReadPointer (channel);
            for (int sampleIndex = windowStart; sampleIndex < windowEnd; ++sampleIndex)
                windowPeak = juce::jmax (windowPeak, std::abs (channelData[sampleIndex]));
        }

        if (windowPeak >= threshold)
            return juce::jlimit (startSample, endSample - 1, windowStart - preRollSamples);
    }

    return startSample;
}

AnalysisWindow getAnalysisWindow (const AudioPluginAudioProcessor::LoadedSampleData& sampleData,
                                  const AudioPluginAudioProcessor::TempoEditState* editState) noexcept
{
    AnalysisWindow window;
    window.startSample = juce::jlimit (0,
                                       juce::jmax (0, sampleData.buffer.getNumSamples() - 1),
                                       sampleData.leadingContentStartSample);
    window.endSample = sampleData.buffer.getNumSamples();
    bool hasExplicitRegion = false;

    if (editState != nullptr && window.endSample > 0
        && editState->regionStartSample >= 0 && editState->regionEndSample > editState->regionStartSample)
    {
        hasExplicitRegion = true;
        window.startSample = juce::jlimit (0, window.endSample - 1, editState->regionStartSample);
        window.endSample = juce::jlimit (window.startSample + 1, window.endSample, editState->regionEndSample);
    }

    if (window.endSample > 0 && hasExplicitRegion)
        window.startSample = findLeadingContentStartSample (sampleData, window.startSample, window.endSample);

    const auto maxAnalysisSamples = juce::jmax (1, (int) std::round (sampleData.sampleRate * maximumTempoAnalysisSeconds));
    if (window.endSample - window.startSample > maxAnalysisSamples)
        window.endSample = juce::jmin (sampleData.buffer.getNumSamples(), window.startSample + maxAnalysisSamples);

    return window;
}

std::vector<float> buildAnalysisEnvelope (const AudioPluginAudioProcessor::LoadedSampleData& sampleData,
                                          const AnalysisWindow& analysisWindow,
                                          double& analysisSampleRate)
{
    const auto sourceSamples = juce::jmax (0, analysisWindow.endSample - analysisWindow.startSample);
    const auto sourceChannels = sampleData.buffer.getNumChannels();

    if (sourceSamples <= 1 || sourceChannels <= 0 || sampleData.sampleRate <= 0.0)
    {
        analysisSampleRate = tempoAnalysisTargetRate;
        return {};
    }

    const auto downsampleFactor = juce::jmax (1, (int) std::floor (sampleData.sampleRate / tempoAnalysisTargetRate));
    analysisSampleRate = sampleData.sampleRate / (double) downsampleFactor;

    std::vector<float> envelope;
    envelope.reserve ((size_t) juce::jmax (1, sourceSamples / downsampleFactor));

    std::vector<float> previousSamples ((size_t) sourceChannels, 0.0f);

    for (int start = 0; start < sourceSamples; start += downsampleFactor)
    {
        const auto end = juce::jmin (start + downsampleFactor, sourceSamples);
        double transientEnergy = 0.0;
        double absoluteEnergy = 0.0;
        int frameCount = 0;

        for (int sampleIndex = start; sampleIndex < end; ++sampleIndex)
        {
            const auto absoluteSampleIndex = analysisWindow.startSample + sampleIndex;
            double monoSample = 0.0;
            double monoDifference = 0.0;

            for (int channel = 0; channel < sourceChannels; ++channel)
            {
                const auto currentSample = sampleData.buffer.getSample (channel, absoluteSampleIndex);
                monoSample += currentSample;
                monoDifference += std::abs (currentSample - previousSamples[(size_t) channel]);
                previousSamples[(size_t) channel] = currentSample;
            }

            monoSample /= (double) sourceChannels;
            monoDifference /= (double) sourceChannels;

            absoluteEnergy += std::abs (monoSample);
            transientEnergy += monoDifference;
            ++frameCount;
        }

        const auto averagedEnergy = frameCount > 0 ? absoluteEnergy / (double) frameCount : 0.0;
        const auto averagedTransient = frameCount > 0 ? transientEnergy / (double) frameCount : 0.0;
        envelope.push_back ((float) (averagedTransient * 0.72 + averagedEnergy * 0.28));
    }

    if (envelope.empty())
        return envelope;

    const int smoothingRadius = juce::jmax (1, (int) std::round (analysisSampleRate * 0.025));
    std::vector<float> smoothed (envelope.size(), 0.0f);

    for (size_t index = 0; index < envelope.size(); ++index)
    {
        const auto start = juce::jmax (0, (int) index - smoothingRadius);
        const auto end = juce::jmin ((int) envelope.size() - 1, (int) index + smoothingRadius);
        const auto count = juce::jmax (1, end - start + 1);

        double sum = 0.0;
        for (int sampleIndex = start; sampleIndex <= end; ++sampleIndex)
            sum += envelope[(size_t) sampleIndex];

        smoothed[index] = (float) (sum / (double) count);
    }

    const int thresholdRadius = juce::jmax (4, (int) std::round (analysisSampleRate * 0.18));
    std::vector<float> novelty (smoothed.size(), 0.0f);
    float peakNovelty = 0.0f;

    for (size_t index = 0; index < smoothed.size(); ++index)
    {
        const auto start = juce::jmax (0, (int) index - thresholdRadius);
        const auto end = juce::jmin ((int) smoothed.size() - 1, (int) index + thresholdRadius);
        const auto count = juce::jmax (1, end - start + 1);

        double localMean = 0.0;
        for (int sampleIndex = start; sampleIndex <= end; ++sampleIndex)
            localMean += smoothed[(size_t) sampleIndex];

        localMean /= (double) count;
        const auto emphasised = juce::jmax (0.0f, smoothed[index] - (float) (localMean * 1.08));
        novelty[index] = emphasised;
        peakNovelty = juce::jmax (peakNovelty, emphasised);
    }

    if (peakNovelty > 0.0f)
    {
        for (auto& sample : novelty)
            sample /= peakNovelty;
    }

    return novelty;
}

double getWindowTempoScore (const std::vector<float>& novelty,
                            int startFrame,
                            int endFrame,
                            double analysisSampleRate) noexcept
{
    const auto frameCount = juce::jmax (0, endFrame - startFrame);
    if (frameCount < 64 || analysisSampleRate <= 0.0)
        return 0.0;

    const auto minLag = juce::jmax (1, (int) std::floor (analysisSampleRate * 60.0 / maximumTempoBpm));
    const auto maxLag = juce::jmin (frameCount / 2,
                                    (int) std::ceil (analysisSampleRate * 60.0 / minimumTempoBpm));

    if (maxLag <= minLag)
        return 0.0;

    double zeroLagEnergy = 0.0;
    for (int frame = startFrame; frame < endFrame; ++frame)
        zeroLagEnergy += (double) novelty[(size_t) frame] * (double) novelty[(size_t) frame];

    if (zeroLagEnergy <= 1.0e-9)
        return 0.0;

    std::vector<double> rawCorrelation ((size_t) (maxLag + 1), 0.0);

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double correlation = 0.0;
        for (int frame = startFrame + lag; frame < endFrame; ++frame)
            correlation += (double) novelty[(size_t) frame] * (double) novelty[(size_t) (frame - lag)];

        rawCorrelation[(size_t) lag] = correlation / zeroLagEnergy;
    }

    auto getCorrelationAtLag = [&rawCorrelation, maxLag] (int lag) noexcept
    {
        if (lag <= 0 || lag > maxLag)
            return 0.0;

        return rawCorrelation[(size_t) lag];
    };

    double bestScore = 0.0;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        const auto bpm = 60.0 * analysisSampleRate / (double) lag;
        const auto composite = (getCorrelationAtLag (lag)
                                + getCorrelationAtLag (lag * 2) * 0.35
                                + getCorrelationAtLag (lag / 2) * 0.2)
                               * getTempoPreferenceWeight (bpm);

        bestScore = juce::jmax (bestScore, composite);
    }

    return bestScore;
}

AnalysisWindow selectRepresentativeTempoWindow (const AudioPluginAudioProcessor::LoadedSampleData& sampleData,
                                                const AnalysisWindow& baseWindow) noexcept
{
    const auto baseSampleCount = juce::jmax (0, baseWindow.endSample - baseWindow.startSample);
    if (baseSampleCount <= 1 || sampleData.sampleRate <= 0.0)
        return baseWindow;

    const auto baseDurationSeconds = (double) baseSampleCount / sampleData.sampleRate;
    if (baseDurationSeconds <= representativeTempoWindowSeconds * 1.25)
        return baseWindow;

    double analysisSampleRate = tempoAnalysisTargetRate;
    const auto novelty = buildAnalysisEnvelope (sampleData, baseWindow, analysisSampleRate);
    if (novelty.size() < 64 || analysisSampleRate <= 0.0)
        return baseWindow;

    const auto targetFrames = juce::jmin ((int) novelty.size(),
                                          juce::jmax (64, (int) std::round (analysisSampleRate * representativeTempoWindowSeconds)));

    if ((int) novelty.size() <= targetFrames)
        return baseWindow;

    const auto maxStartFrame = juce::jmax (0, (int) novelty.size() - targetFrames);
    const auto stepFrames = juce::jmax (1, (int) std::round (analysisSampleRate * representativeWindowStepSeconds));

    double bestScore = -1.0;
    int bestStartFrame = 0;

    auto considerWindow = [&] (int candidateStartFrame)
    {
        const auto startFrame = juce::jlimit (0, maxStartFrame, candidateStartFrame);
        const auto endFrame = startFrame + targetFrames;
        const auto score = getWindowTempoScore (novelty, startFrame, endFrame, analysisSampleRate);

        if (score > bestScore)
        {
            bestScore = score;
            bestStartFrame = startFrame;
        }
    };

    for (int startFrame = 0; startFrame <= maxStartFrame; startFrame += stepFrames)
        considerWindow (startFrame);

    considerWindow (maxStartFrame);

    if (bestScore <= 0.0)
        return baseWindow;

    AnalysisWindow refinedWindow = baseWindow;
    refinedWindow.startSample = juce::jlimit (baseWindow.startSample, baseWindow.endSample - 1,
                                              baseWindow.startSample
                                                  + (int) std::round (((double) bestStartFrame / analysisSampleRate)
                                                                      * sampleData.sampleRate));
    refinedWindow.endSample = juce::jlimit (refinedWindow.startSample + 1, baseWindow.endSample,
                                            baseWindow.startSample
                                                + (int) std::round (((double) (bestStartFrame + targetFrames) / analysisSampleRate)
                                                                    * sampleData.sampleRate));
    return refinedWindow;
}

TempoWindowCandidateSet analyzeTempoWindowCandidates (const std::vector<float>& novelty,
                                                      int startFrame,
                                                      int endFrame,
                                                      double analysisSampleRate)
{
    TempoWindowCandidateSet result;
    result.startFrame = startFrame;
    result.endFrame = endFrame;

    const auto frameCount = juce::jmax (0, endFrame - startFrame);
    if (frameCount < 64 || analysisSampleRate <= 0.0)
        return result;

    const auto minLag = juce::jmax (1, (int) std::floor (analysisSampleRate * 60.0 / maximumTempoBpm));
    const auto maxLag = juce::jmin (frameCount / 2,
                                    (int) std::ceil (analysisSampleRate * 60.0 / minimumTempoBpm));

    if (maxLag <= minLag)
        return result;

    double zeroLagEnergy = 0.0;
    for (int frame = startFrame; frame < endFrame; ++frame)
        zeroLagEnergy += (double) novelty[(size_t) frame] * (double) novelty[(size_t) frame];

    if (zeroLagEnergy <= 1.0e-9)
        return result;

    std::vector<double> rawCorrelation ((size_t) (maxLag + 1), 0.0);
    std::vector<double> compositeScores ((size_t) (maxLag + 1), 0.0);

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double correlation = 0.0;
        for (int frame = startFrame + lag; frame < endFrame; ++frame)
            correlation += (double) novelty[(size_t) frame] * (double) novelty[(size_t) (frame - lag)];

        rawCorrelation[(size_t) lag] = correlation / zeroLagEnergy;
    }

    auto getCorrelationAtLag = [&rawCorrelation, maxLag] (int lag) noexcept
    {
        if (lag <= 0 || lag > maxLag)
            return 0.0;

        return rawCorrelation[(size_t) lag];
    };

    double bestScore = 0.0;
    int bestLag = 0;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        const auto bpm = 60.0 * analysisSampleRate / (double) lag;
        const auto composite = (getCorrelationAtLag (lag)
                                + getCorrelationAtLag (lag * 2) * 0.35
                                + getCorrelationAtLag (lag / 2) * 0.2)
                               * getTempoPreferenceWeight (bpm);

        compositeScores[(size_t) lag] = composite;

        if (composite > bestScore)
        {
            bestScore = composite;
            bestLag = lag;
        }
    }

    if (bestLag <= 0 || bestScore <= 0.0)
        return result;

    // Parabolic sub-lag interpolation: fits a parabola through the three samples around
    // each integer lag peak to find the true fractional peak position.
    // At 200 Hz analysis rate, this reduces BPM quantization error from ~1.2 BPM to ~0.1 BPM
    // at 120 BPM, and the more precise BPM feeds a better beat grid into evaluateTempoHypothesis.
    auto interpolateLag = [&] (int lag) noexcept -> double
    {
        if (lag <= minLag || lag >= maxLag)
            return (double) lag;
        const double a     = compositeScores[(size_t) (lag - 1)];
        const double b     = compositeScores[(size_t) lag];
        const double c     = compositeScores[(size_t) (lag + 1)];
        const double denom = a - 2.0 * b + c;
        if (std::abs (denom) < 1e-12)
            return (double) lag;
        return (double) lag - 0.5 * (c - a) / denom;
    };

    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        const auto score = compositeScores[(size_t) lag];
        if (score <= 0.0)
            continue;

        if (score >= compositeScores[(size_t) (lag - 1)]
            && score >= compositeScores[(size_t) (lag + 1)])
        {
            const auto refinedLag = interpolateLag (lag);
            result.candidates.push_back ({ lag, 60.0 * analysisSampleRate / refinedLag, score });
        }
    }

    const auto hasBestLag = std::any_of (result.candidates.begin(), result.candidates.end(),
                                         [bestLag] (const auto& candidate) noexcept { return candidate.lag == bestLag; });
    if (! hasBestLag)
    {
        const auto refinedBestLag = interpolateLag (bestLag);
        result.candidates.push_back ({ bestLag, 60.0 * analysisSampleRate / refinedBestLag, bestScore });
    }

    std::sort (result.candidates.begin(), result.candidates.end(),
               [] (const auto& lhs, const auto& rhs) noexcept
               {
                   if (std::abs (lhs.score - rhs.score) > 1.0e-9)
                       return lhs.score > rhs.score;

                   return std::abs (lhs.bpm - preferredTempoLow) < std::abs (rhs.bpm - preferredTempoLow);
               });

    if ((int) result.candidates.size() > localTempoCandidatesPerWindow)
        result.candidates.resize ((size_t) localTempoCandidatesPerWindow);

    result.bestScore = result.candidates.empty() ? 0.0 : result.candidates.front().score;
    return result;
}

bool windowsOverlapTooMuch (const TempoWindowCandidateSet& first,
                            const TempoWindowCandidateSet& second) noexcept
{
    const auto overlapStart = juce::jmax (first.startFrame, second.startFrame);
    const auto overlapEnd = juce::jmin (first.endFrame, second.endFrame);
    const auto overlapFrames = juce::jmax (0, overlapEnd - overlapStart);
    const auto shorterWindow = juce::jmax (1, juce::jmin (first.endFrame - first.startFrame,
                                                          second.endFrame - second.startFrame));
    return (double) overlapFrames / (double) shorterWindow > representativeWindowOverlapRatio;
}

std::vector<TempoWindowCandidateSet> selectConsensusTempoWindows (const std::vector<float>& novelty,
                                                                  double analysisSampleRate)
{
    std::vector<TempoWindowCandidateSet> selectedWindows;
    if (novelty.size() < 64 || analysisSampleRate <= 0.0)
        return selectedWindows;

    const auto targetFrames = juce::jmin ((int) novelty.size(),
                                          juce::jmax (64, (int) std::round (analysisSampleRate * representativeTempoWindowSeconds)));

    if ((int) novelty.size() <= targetFrames)
    {
        auto singleWindow = analyzeTempoWindowCandidates (novelty, 0, (int) novelty.size(), analysisSampleRate);
        if (! singleWindow.candidates.empty())
            selectedWindows.push_back (std::move (singleWindow));
        return selectedWindows;
    }

    const auto maxStartFrame = juce::jmax (0, (int) novelty.size() - targetFrames);
    const auto stepFrames = juce::jmax (1, (int) std::round (analysisSampleRate * representativeWindowStepSeconds));

    std::vector<TempoWindowCandidateSet> allWindows;
    auto addWindow = [&] (int startFrame)
    {
        const auto clampedStart = juce::jlimit (0, maxStartFrame, startFrame);
        auto window = analyzeTempoWindowCandidates (novelty, clampedStart, clampedStart + targetFrames, analysisSampleRate);
        if (! window.candidates.empty())
            allWindows.push_back (std::move (window));
    };

    for (int startFrame = 0; startFrame <= maxStartFrame; startFrame += stepFrames)
        addWindow (startFrame);

    addWindow (maxStartFrame);

    std::sort (allWindows.begin(), allWindows.end(),
               [] (const auto& lhs, const auto& rhs) noexcept { return lhs.bestScore > rhs.bestScore; });

    for (const auto& window : allWindows)
    {
        const auto overlapsExisting = std::any_of (selectedWindows.begin(), selectedWindows.end(),
                                                   [&window] (const auto& existing) noexcept
                                                   {
                                                       return windowsOverlapTooMuch (window, existing);
                                                   });

        if (! overlapsExisting)
        {
            selectedWindows.push_back (window);

            if ((int) selectedWindows.size() >= representativeConsensusWindowCount)
                break;
        }
    }

    if (selectedWindows.empty() && ! allWindows.empty())
        selectedWindows.push_back (allWindows.front());

    return selectedWindows;
}

double getWindowTempoSupportScore (const TempoWindowCandidateSet& window,
                                   double targetBpm,
                                   bool* wasDirectSupport = nullptr) noexcept
{
    double bestScore = 0.0;
    bool bestWasDirect = false;

    for (const auto& candidate : window.candidates)
    {
        for (const auto multiplier : { 1.0, 0.5, 2.0 })
        {
            const auto mappedBpm = candidate.bpm * multiplier;
            if (mappedBpm < minimumTempoBpm || mappedBpm > maximumTempoBpm)
                continue;

            const auto ratioDifference = std::abs (mappedBpm - targetBpm) / juce::jmax (mappedBpm, targetBpm);
            if (ratioDifference > tempoConsensusTolerance)
                continue;

            const auto closeness = 1.0 - ratioDifference / tempoConsensusTolerance;
            const auto directSupport = std::abs (multiplier - 1.0) < 1.0e-6;
            const auto mappedScore = candidate.score
                                   * (0.65 + 0.35 * closeness)
                                   * (directSupport ? 1.0 : harmonicMappedSupportPenalty);

            if (mappedScore > bestScore)
            {
                bestScore = mappedScore;
                bestWasDirect = directSupport;
            }
        }
    }

    if (wasDirectSupport != nullptr)
        *wasDirectSupport = bestWasDirect;

    return bestScore;
}

TempoConsensusChoice chooseConsensusTempo (const std::vector<TempoWindowCandidateSet>& windows)
{
    TempoConsensusChoice bestChoice;
    if (windows.empty())
        return bestChoice;

    std::vector<double> targetBpms;
    auto addTarget = [&targetBpms] (double bpm)
    {
        if (bpm < minimumTempoBpm || bpm > maximumTempoBpm)
            return;

        for (auto& existing : targetBpms)
        {
            if (std::abs (existing - bpm) / juce::jmax (existing, bpm) <= 0.02)
            {
                existing = 0.5 * (existing + bpm);
                return;
            }
        }

        targetBpms.push_back (bpm);
    };

    for (const auto& window : windows)
    {
        for (const auto& candidate : window.candidates)
        {
            addTarget (candidate.bpm);
            addTarget (candidate.bpm * 0.5);
            addTarget (candidate.bpm * 2.0);
        }
    }

    for (const auto targetBpm : targetBpms)
    {
        double totalScore = 0.0;
        double strongestWindowScore = 0.0;
        int strongestWindowIndex = 0;
        int supportingWindows = 0;
        int directSupportWindows = 0;

        for (int windowIndex = 0; windowIndex < (int) windows.size(); ++windowIndex)
        {
            bool wasDirectSupport = false;
            const auto supportScore = getWindowTempoSupportScore (windows[(size_t) windowIndex],
                                                                 targetBpm,
                                                                 &wasDirectSupport);

            if (supportScore <= 0.0)
                continue;

            totalScore += supportScore;
            ++supportingWindows;

            if (wasDirectSupport)
                ++directSupportWindows;

            if (supportScore > strongestWindowScore)
            {
                strongestWindowScore = supportScore;
                strongestWindowIndex = windowIndex;
            }
        }

        if (supportingWindows <= 0)
            continue;

        totalScore += (double) supportingWindows * 0.12
                   + (double) directSupportWindows * 0.08
                   + getTempoPreferenceWeight (targetBpm) * 0.15;

        if (! bestChoice.valid
            || totalScore > bestChoice.score
            || (std::abs (totalScore - bestChoice.score) <= 1.0e-9
                && directSupportWindows > bestChoice.directSupportWindows))
        {
            bestChoice.valid = true;
            bestChoice.bpm = targetBpm;
            bestChoice.score = totalScore;
            bestChoice.strongestWindowIndex = strongestWindowIndex;
            bestChoice.supportingWindows = supportingWindows;
            bestChoice.directSupportWindows = directSupportWindows;
            bestChoice.normalizedSupport = juce::jlimit (0.0, 1.0,
                                                         totalScore / ((double) juce::jmax (1, supportingWindows) * 1.4));
        }
    }

    return bestChoice;
}

TempoHypothesisEvaluation evaluateTempoHypothesis (const std::vector<float>& novelty,
                                                   const TempoWindowCandidateSet& window,
                                                   double analysisSampleRate,
                                                   double baseWindowStartSeconds,
                                                   double bpm)
{
    TempoHypothesisEvaluation result;

    const auto frameCount = juce::jmax (0, window.endFrame - window.startFrame);
    if (frameCount < 64 || analysisSampleRate <= 0.0 || bpm <= 0.0)
        return result;

    const auto lag = (int) std::round (analysisSampleRate * 60.0 / bpm);
    if (lag <= 0 || lag >= frameCount / 2)
        return result;

    const auto beatWindow = juce::jmax (1, lag / 6);
    auto findBeatNear = [&] (int predictedIndex)
    {
        const auto searchStart = juce::jmax (window.startFrame, predictedIndex - beatWindow);
        const auto searchEnd = juce::jmin (window.endFrame - 1, predictedIndex + beatWindow);
        auto strongestIndex = juce::jlimit (window.startFrame, window.endFrame - 1, predictedIndex);
        auto strongestValue = novelty[(size_t) strongestIndex];

        for (int index = searchStart; index <= searchEnd; ++index)
        {
            if (novelty[(size_t) index] > strongestValue)
            {
                strongestValue = novelty[(size_t) index];
                strongestIndex = index;
            }
        }

        return strongestIndex;
    };

    int bestPhase = 0;
    double bestPhaseScore = -1.0;

    for (int phase = 0; phase < lag; ++phase)
    {
        double phaseScore = 0.0;
        int phaseBeats = 0;

        for (int predicted = window.startFrame + phase; predicted < window.endFrame; predicted += lag)
        {
            phaseScore += novelty[(size_t) findBeatNear (predicted)];
            ++phaseBeats;
        }

        if (phaseBeats > 0)
            phaseScore /= (double) phaseBeats;

        if (phaseScore > bestPhaseScore)
        {
            bestPhaseScore = phaseScore;
            bestPhase = phase;
        }
    }

    std::vector<int> beatIndices;
    std::vector<float> beatStrengths;

    for (int predicted = window.startFrame + bestPhase; predicted < window.endFrame; predicted += lag)
    {
        const auto beatIndex = findBeatNear (predicted);
        if (! beatIndices.empty() && beatIndex == beatIndices.back())
            continue;

        beatIndices.push_back (beatIndex);
        beatStrengths.push_back (novelty[(size_t) beatIndex]);
    }

    if (beatIndices.size() < 2)
        return result;

    std::vector<double> beatPositionsSeconds;
    beatPositionsSeconds.reserve (beatIndices.size());
    for (const auto beatIndex : beatIndices)
        beatPositionsSeconds.push_back (baseWindowStartSeconds + (double) beatIndex / analysisSampleRate);

    std::vector<double> beatIntervalsSeconds;
    beatIntervalsSeconds.reserve (beatPositionsSeconds.size() - 1);
    for (size_t index = 1; index < beatPositionsSeconds.size(); ++index)
    {
        const auto interval = beatPositionsSeconds[index] - beatPositionsSeconds[index - 1];
        if (interval > 0.15 && interval < 2.0)
            beatIntervalsSeconds.push_back (interval);
    }

    if (beatIntervalsSeconds.empty())
        return result;

    // Median is robust against the snapped beat positions occasionally landing far from
    // the predicted lag grid, which would inflate a plain mean.
    std::vector<double> sortedIntervals = beatIntervalsSeconds;
    std::sort (sortedIntervals.begin(), sortedIntervals.end());
    const size_t nIv = sortedIntervals.size();
    const double medianInterval = (nIv % 2 == 0)
        ? 0.5 * (sortedIntervals[nIv / 2 - 1] + sortedIntervals[nIv / 2])
        : sortedIntervals[nIv / 2];

    if (medianInterval <= 0.0)
        return result;

    // Trimmed mean around median for precision once outliers are removed.
    double intervalSum   = 0.0;
    int    intervalCount = 0;
    for (const auto iv : beatIntervalsSeconds)
    {
        if (std::abs (iv - medianInterval) / medianInterval <= 0.20)
        {
            intervalSum += iv;
            ++intervalCount;
        }
    }
    const double meanInterval = (intervalCount >= 3)
        ? intervalSum / (double) intervalCount
        : medianInterval;

    double variance = 0.0;
    for (const auto interval : beatIntervalsSeconds)
    {
        const auto delta = interval - medianInterval;
        variance += delta * delta;
    }

    variance /= (double) beatIntervalsSeconds.size();
    const auto driftAmount = std::sqrt (variance) / medianInterval;

    double zeroLagEnergy = 0.0;
    for (int frame = window.startFrame; frame < window.endFrame; ++frame)
        zeroLagEnergy += (double) novelty[(size_t) frame] * (double) novelty[(size_t) frame];

    double correlation = 0.0;
    if (zeroLagEnergy > 1.0e-9)
    {
        for (int frame = window.startFrame + lag; frame < window.endFrame; ++frame)
            correlation += (double) novelty[(size_t) frame] * (double) novelty[(size_t) (frame - lag)];
        correlation /= zeroLagEnergy;
    }

    const auto totalBeatStrength = std::accumulate (beatStrengths.begin(), beatStrengths.end(), 0.0);
    const auto meanBeatStrength = totalBeatStrength / (double) beatStrengths.size();

    int downbeatPhase = 0;
    double bestDownbeatScore = -1.0;
    for (int phase = 0; phase < 4; ++phase)
    {
        double phaseScore = 0.0;
        for (int beatIndex = phase; beatIndex < (int) beatStrengths.size(); beatIndex += 4)
            phaseScore += (double) beatStrengths[(size_t) beatIndex];

        if (phaseScore > bestDownbeatScore)
        {
            bestDownbeatScore = phaseScore;
            downbeatPhase = phase;
        }
    }

    const auto beatCountScore = juce::jlimit (0.0, 1.0, (double) beatIndices.size() / 16.0);
    const auto consistencyScore = juce::jlimit (0.0, 1.0, 1.0 - driftAmount * 6.5);
    const auto barAccentScore = totalBeatStrength > 1.0e-6
                              ? juce::jlimit (0.0, 1.0, ((bestDownbeatScore / totalBeatStrength) - 0.25) / 0.35)
                              : 0.0;

    result.valid = true;
    result.bpm = 60.0 / meanInterval;
    result.beatPeriodSeconds = meanInterval;
    result.firstBeatSeconds = beatPositionsSeconds.front();
    result.confidence = juce::jlimit (0.0f, 1.0f,
                                      (float) (correlation * 0.35
                                               + meanBeatStrength * 0.3
                                               + consistencyScore * 0.2
                                               + barAccentScore * 0.1
                                               + beatCountScore * 0.05));
    result.score = (double) result.confidence * 0.75
                 + meanBeatStrength * 0.1
                 + barAccentScore * 0.1
                 + getTempoPreferenceWeight (result.bpm) * 0.05;
    result.likelyDrifting = driftAmount > 0.03;
    result.downbeatPhase = downbeatPhase;
    result.beatPositionsSeconds = beatPositionsSeconds;

    for (int beatIndex = downbeatPhase; beatIndex < (int) beatPositionsSeconds.size(); beatIndex += 4)
        result.barPositionsSeconds.push_back (beatPositionsSeconds[(size_t) beatIndex]);

    return result;
}

bool isValidTempoAnalysis (const AudioPluginAudioProcessor::TempoAnalysisData& analysis) noexcept
{
    return analysis.estimatedBpm > 0.0
        && analysis.beatPeriodSeconds > 0.0
        && analysis.analysisEndSeconds > analysis.analysisStartSeconds;
}

bool doTemposAgree (double firstBpm, double secondBpm, double toleranceRatio) noexcept
{
    if (firstBpm <= 0.0 || secondBpm <= 0.0)
        return false;

    return std::abs (firstBpm - secondBpm) / juce::jmax (firstBpm, secondBpm) <= toleranceRatio;
}

double getSamplingTempoScore (const AudioPluginAudioProcessor::TempoAnalysisData& analysis) noexcept
{
    if (! isValidTempoAnalysis (analysis))
        return 0.0;

    const auto beatCountScore = juce::jlimit (0.0, 1.0, (double) analysis.beatPositionsSeconds.size() / 24.0);
    const auto barCountScore = juce::jlimit (0.0, 1.0, (double) analysis.barPositionsSeconds.size() / 8.0);
    const auto stabilityScore = analysis.likelyDrifting ? 0.2 : 1.0;

    return (double) analysis.confidence * 0.55
         + getTempoPreferenceWeight (analysis.estimatedBpm) * 0.2
         + beatCountScore * 0.15
         + barCountScore * 0.1
         + stabilityScore * 0.1;
}

AudioPluginAudioProcessor::TempoAnalysisData makeTempoAnalysisFromBeatThis (
    const BeatThisAnalyzer::Result& btResult,
    const AnalysisWindow& analysisWindow,
    double sampleRate)
{
    AudioPluginAudioProcessor::TempoAnalysisData result;
    result.estimatedBpm = btResult.estimatedBpm;
    result.confidence = btResult.confidence;
    result.likelyDrifting = btResult.likelyDrifting;
    result.beatPeriodSeconds = btResult.beatPeriodSecs;
    result.firstBeatSeconds = btResult.firstBeatSecs;
    result.beatPositionsSeconds = btResult.beatPositionsSecs;
    result.barPositionsSeconds = btResult.barPositionsSecs;
    result.downbeatPhase = btResult.downbeatPhase;
    result.analysisStartSeconds = (double) analysisWindow.startSample / sampleRate;
    result.analysisEndSeconds = (double) analysisWindow.endSample / sampleRate;
    return result;
}

void refineSampleTempoAnalysisWithBeatThis (AudioPluginAudioProcessor::TempoAnalysisData& baseAnalysis,
                                            const AudioPluginAudioProcessor::TempoAnalysisData& neuralAnalysis)
{
    baseAnalysis.confidence = juce::jmax (baseAnalysis.confidence,
                                          juce::jlimit (0.0f, 1.0f, neuralAnalysis.confidence * 0.92f));
    baseAnalysis.likelyDrifting = baseAnalysis.likelyDrifting && neuralAnalysis.likelyDrifting;

    if (! neuralAnalysis.barPositionsSeconds.empty())
        baseAnalysis.barPositionsSeconds = neuralAnalysis.barPositionsSeconds;

    if (! neuralAnalysis.beatPositionsSeconds.empty())
    {
        baseAnalysis.beatPositionsSeconds = neuralAnalysis.beatPositionsSeconds;
        baseAnalysis.firstBeatSeconds = neuralAnalysis.firstBeatSeconds;
    }

    baseAnalysis.downbeatPhase = neuralAnalysis.downbeatPhase;
}

AudioPluginAudioProcessor::TempoAnalysisData analyzeTempoSample (const AudioPluginAudioProcessor::LoadedSampleData& sampleData,
                                                                const AudioPluginAudioProcessor::TempoEditState* editState)
{
    AudioPluginAudioProcessor::TempoAnalysisData result;
    const auto baseWindow = getAnalysisWindow (sampleData, editState);
    double analysisSampleRate = tempoAnalysisTargetRate;
    auto novelty = buildAnalysisEnvelope (sampleData, baseWindow, analysisSampleRate);

    if (novelty.size() < 64 || analysisSampleRate <= 0.0)
        return result;

    const auto representativeWindows = selectConsensusTempoWindows (novelty, analysisSampleRate);
    if (representativeWindows.empty())
        return result;

    const auto consensus = chooseConsensusTempo (representativeWindows);
    if (! consensus.valid)
        return result;

    const auto& strongestWindow = representativeWindows[(size_t) consensus.strongestWindowIndex];
    const auto baseWindowStartSeconds = (double) baseWindow.startSample / sampleData.sampleRate;

    std::vector<double> hypothesisBpms;
    auto addHypothesis = [&hypothesisBpms] (double bpm)
    {
        if (bpm < minimumTempoBpm || bpm > maximumTempoBpm)
            return;

        for (const auto existing : hypothesisBpms)
        {
            if (std::abs (existing - bpm) / juce::jmax (existing, bpm) <= 0.02)
                return;
        }

        hypothesisBpms.push_back (bpm);
    };

    addHypothesis (consensus.bpm);
    addHypothesis (consensus.bpm * 0.5);
    addHypothesis (consensus.bpm * 2.0);

    TempoHypothesisEvaluation selectedHypothesis;

    for (const auto hypothesisBpm : hypothesisBpms)
    {
        auto hypothesis = evaluateTempoHypothesis (novelty,
                                                   strongestWindow,
                                                   analysisSampleRate,
                                                   baseWindowStartSeconds,
                                                   hypothesisBpm);
        if (! hypothesis.valid)
            continue;

        if (! selectedHypothesis.valid)
        {
            selectedHypothesis = std::move (hypothesis);
            continue;
        }

        const auto requiresFamilySwitch = std::abs (hypothesisBpm - consensus.bpm) / juce::jmax (hypothesisBpm, consensus.bpm) > 0.02;
        const auto requiredMargin = requiresFamilySwitch ? halfDoubleScoreMargin : 0.0;

        if (hypothesis.score > selectedHypothesis.score + requiredMargin)
            selectedHypothesis = std::move (hypothesis);
    }

    if (! selectedHypothesis.valid)
        return result;

    result.estimatedBpm = selectedHypothesis.bpm;
    result.beatPeriodSeconds = selectedHypothesis.beatPeriodSeconds;
    result.firstBeatSeconds = selectedHypothesis.firstBeatSeconds;
    result.downbeatPhase = selectedHypothesis.downbeatPhase;
    result.likelyDrifting = selectedHypothesis.likelyDrifting;
    result.analysisStartSeconds = baseWindowStartSeconds + (double) strongestWindow.startFrame / analysisSampleRate;
    result.analysisEndSeconds = baseWindowStartSeconds + (double) strongestWindow.endFrame / analysisSampleRate;
    result.confidence = juce::jlimit (0.0f, 1.0f,
                                      (float) ((double) selectedHypothesis.confidence * 0.8
                                               + consensus.normalizedSupport * 0.2));
    result.beatPositionsSeconds = std::move (selectedHypothesis.beatPositionsSeconds);
    result.barPositionsSeconds = std::move (selectedHypothesis.barPositionsSeconds);

    return result;
}
} // namespace

class AudioPluginAudioProcessor::TempoAnalysisJob final : public juce::ThreadPoolJob
{
public:
    TempoAnalysisJob (AudioPluginAudioProcessor& ownerIn,
                      std::shared_ptr<const LoadedSampleData> sampleDataIn,
                      std::shared_ptr<const TempoEditState> editStateIn,
                      uint64_t generationIn)
        : juce::ThreadPoolJob ("Tempo Analysis"),
          owner (ownerIn),
          sampleData (std::move (sampleDataIn)),
          editState (std::move (editStateIn)),
          generation (generationIn)
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit() || sampleData == nullptr)
            return jobHasFinished;

        auto result = std::make_shared<TempoAnalysisData> (analyzeTempoSample (*sampleData, editState.get()));
        const auto classicValid = isValidTempoAnalysis (*result);

        // For sample chopping, the classical detector is faster and usually a better
        // source of BPM/grid spacing. Only pay the neural cost when the classical
        // answer is weak or when BeatThis strongly agrees and can refine downbeats.
        if (classicValid && result->confidence >= strongClassicTempoConfidence && ! result->likelyDrifting)
        {
            owner.publishTempoAnalysis (std::move (result), generation);
            return jobHasFinished;
        }

        if (shouldExit())
            return jobHasFinished;

        if (owner.beatThisAnalyzer != nullptr && owner.beatThisAnalyzer->isReady())
        {
            const auto baseWindow = getAnalysisWindow (*sampleData, editState.get());
            const auto analysisWindow = selectRepresentativeTempoWindow (*sampleData, baseWindow);

            const auto btResult = owner.beatThisAnalyzer->analyze (sampleData->buffer,
                                                                   sampleData->sampleRate,
                                                                   analysisWindow.startSample,
                                                                   analysisWindow.endSample);

            if (btResult.valid)
            {
                auto neuralAnalysis = makeTempoAnalysisFromBeatThis (btResult, analysisWindow, sampleData->sampleRate);

                if (classicValid
                    && btResult.confidence >= strongNeuralTempoConfidence
                    && doTemposAgree (result->estimatedBpm, neuralAnalysis.estimatedBpm, neuralAgreementTolerance))
                {
                    refineSampleTempoAnalysisWithBeatThis (*result, neuralAnalysis);
                    juce::Logger::writeToLog ("Tempo analysis: classical BPM retained, BeatThis used to refine beat/downbeat placement");
                }
                else if (! classicValid
                         || getSamplingTempoScore (neuralAnalysis) > getSamplingTempoScore (*result) + neuralSelectionScoreMargin)
                {
                    *result = std::move (neuralAnalysis);
                    juce::Logger::writeToLog ("Tempo analysis: BeatThis selected over classical detector");
                }
                else
                {
                    juce::Logger::writeToLog ("Tempo analysis: classical detector retained over BeatThis");
                }
            }
        }

        owner.publishTempoAnalysis (std::move (result), generation);
        return jobHasFinished;
    }

private:
    AudioPluginAudioProcessor& owner;
    std::shared_ptr<const LoadedSampleData> sampleData;
    std::shared_ptr<const TempoEditState> editState;
    uint64_t generation = 0;
};

// Offline HTDemucs-FT pass for one sample. Mirrors TempoAnalysisJob: holds the
// generation captured at launch, bails via shouldExit()/stale-generation, and
// publishes through a generation-guarded publishStems(). On any failure it
// publishes a null result, leaving the original buffer in place.
class AudioPluginAudioProcessor::StemSeparationJob final : public juce::ThreadPoolJob
{
public:
    StemSeparationJob (AudioPluginAudioProcessor& ownerIn,
                       std::shared_ptr<LoadedSampleData> sampleDataIn,
                       uint64_t generationIn)
        : juce::ThreadPoolJob ("Stem Separation"),
          owner (ownerIn),
          sampleData (std::move (sampleDataIn)),
          generation (generationIn)
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit() || sampleData == nullptr)
            return jobHasFinished;

        if (owner.stemSeparator == nullptr || ! owner.stemSeparator->isReady())
        {
            owner.publishStems (nullptr, generation); // no models → stay on original
            return jobHasFinished;
        }

        auto stale = [this]
        {
            return shouldExit() || generation != owner.stemGeneration.load (std::memory_order_acquire);
        };

        auto onProgress = [this] (float f)
        {
            if (generation == owner.stemGeneration.load (std::memory_order_acquire))
                owner.stemProgress.store (juce::jlimit (0.0f, 1.0f, f), std::memory_order_release);
        };

        const auto result = owner.stemSeparator->separate (sampleData->buffer,
                                                            sampleData->sampleRate,
                                                            onProgress, stale);

        if (stale())
            return jobHasFinished;

        if (! result.valid)
        {
            owner.publishStems (nullptr, generation);
            return jobHasFinished;
        }

        auto set = std::make_shared<StemSet>();
        set->source = sampleData;             // pristine original (shared, not copied)
        set->drums  = result.drums;
        set->bass   = result.bass;
        set->vocals = result.vocals;
        owner.publishStems (std::move (set), generation);
        return jobHasFinished;
    }

private:
    AudioPluginAudioProcessor& owner;
    std::shared_ptr<LoadedSampleData> sampleData;
    uint64_t generation = 0;
};

// Coalesced background remix after a mute toggle. Only the latest generation
// actually rebuilds; superseded ones return immediately. Runs on stemThreadPool.
class AudioPluginAudioProcessor::RemixJob final : public juce::ThreadPoolJob
{
public:
    RemixJob (AudioPluginAudioProcessor& ownerIn, uint64_t generationIn)
        : juce::ThreadPoolJob ("Stem Remix"), owner (ownerIn), generation (generationIn) {}

    JobStatus runJob() override
    {
        if (shouldExit() || generation != owner.stemRemixGeneration.load (std::memory_order_acquire))
            return jobHasFinished;
        owner.rebuildActiveMix();
        return jobHasFinished;
    }

private:
    AudioPluginAudioProcessor& owner;
    uint64_t generation = 0;
};

class AudioPluginAudioProcessor::DeferredSampleRestoreJob final : public juce::ThreadPoolJob
{
public:
    DeferredSampleRestoreJob (AudioPluginAudioProcessor& ownerIn,
                              DeferredRestoreStateData restoreStateIn,
                              uint64_t generationIn)
        : juce::ThreadPoolJob ("Deferred Sample Restore"),
          owner (ownerIn),
          restoreState (std::move (restoreStateIn)),
          generation (generationIn)
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit())
            return jobHasFinished;

        auto restoredSample = std::shared_ptr<LoadedSampleData> {};
        if (restoreState.hasSavedSample && restoreState.embeddedSampleData.getSize() > 0)
            restoredSample = owner.createLoadedSampleDataFromStateData (restoreState.embeddedSampleData,
                                                                        restoreState.sampleFileName);

        if (restoredSample == nullptr && restoreState.samplePath.isNotEmpty())
            restoredSample = owner.createLoadedSampleDataFromFile (juce::File (restoreState.samplePath));

        if (shouldExit())
            return jobHasFinished;

        owner.completeDeferredSampleRestore (restoreState, std::move (restoredSample), generation);
        return jobHasFinished;
    }

private:
    AudioPluginAudioProcessor& owner;
    DeferredRestoreStateData restoreState;
    uint64_t generation = 0;
};

class AudioPluginAudioProcessor::KeyDetectionJob final : public juce::ThreadPoolJob
{
public:
    KeyDetectionJob (AudioPluginAudioProcessor& ownerIn,
                     uint64_t generationIn,
                     juce::File sourceFileIn,
                     juce::AudioBuffer<float> bufferIn,
                     double sampleRateIn)
        : juce::ThreadPoolJob ("Key Detection"),
          owner (ownerIn),
          generation (generationIn),
          sourceFile (std::move (sourceFileIn)),
          buffer (std::move (bufferIn)),
          sampleRate (sampleRateIn)
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit())
            return finish();

        // Data flywheel: when telemetry is enabled, compute the content
        // fingerprint up front so the jar can group events by sample, and stamp
        // the sample context. (The online key lookup that used to provide a
        // "truth" label was removed — AcousticBrainz, which mapped fingerprints
        // to keys, was shut down. Ground-truth key labels now come from the
        // user's key override instead.)
        const bool wantTelemetry = owner.editTelemetry.isEnabled();
        if (wantTelemetry)
        {
            int fpDuration = 0;
            const auto fingerprint = owner.audioFingerprinter.localFingerprint (buffer, sampleRate, fpDuration);
            const auto durationSeconds = sampleRate > 0.0 ? (double) buffer.getNumSamples() / sampleRate : 0.0;
            owner.editTelemetry.setSampleContext (fingerprint, durationSeconds, sampleRate, buffer.getNumChannels());
        }

        // Prefer an embedded key tag in the file metadata when present.
        auto metadataResult = owner.tryReadKeyFromMetadata (sourceFile);
        const bool haveMetadataKey = metadataResult.valid;

        // Always compute the local FFT guess (it's also what we publish when no
        // metadata key exists, and what telemetry logs against any truth).
        auto localResult = owner.keyDetector.detect (buffer, sampleRate);

        if (wantTelemetry)
            owner.editTelemetry.recordKeyObservation (localResult.key, localResult.confidence,
                                                      haveMetadataKey ? metadataResult.key : std::string(),
                                                      std::string());

        publishResult (haveMetadataKey ? std::move (metadataResult) : std::move (localResult));
        return jobHasFinished;
    }

private:
    bool isCurrent() const noexcept
    {
        return generation == owner.keyDetectionGeneration.load (std::memory_order_acquire);
    }

    JobStatus finish() noexcept
    {
        if (isCurrent())
            owner.keyDetectionInProgress.store (false, std::memory_order_release);

        return jobHasFinished;
    }

    bool publishResult (KeyDetector::Result result)
    {
        if (! isCurrent())
            return false;

        {
            const std::lock_guard<std::mutex> lock (owner.keyResultMutex);
            owner.detectedKeyResult = std::move (result);
        }

        owner.keyDetectionInProgress.store (false, std::memory_order_release);
        owner.tempoUiRevision.fetch_add (1, std::memory_order_acq_rel);
        return true;
    }

    AudioPluginAudioProcessor& owner;
    uint64_t generation = 0;
    juce::File sourceFile;
    juce::AudioBuffer<float> buffer;
    double sampleRate = 0.0;
};

class AudioPluginAudioProcessor::WarpRenderJob final : public juce::ThreadPoolJob
{
public:
    WarpRenderJob (AudioPluginAudioProcessor& processorIn,
                   cuesampler::ChopAudioCache& cacheIn,
                   std::shared_ptr<const LoadedSampleData> sampleIn,
                   std::shared_ptr<const ChopState> chopStateIn,
                   int chopIdIn,
                   std::uint64_t generationIn)
        : juce::ThreadPoolJob ("Warp Render"),
          processor (processorIn),
          cache (cacheIn),
          sample (std::move (sampleIn)),
          chopStateSnapshot (std::move (chopStateIn)),
          chopId (chopIdIn),
          generation (generationIn)
    {
    }

    JobStatus runJob() override
    {
        if (shouldExit() || sample == nullptr || chopStateSnapshot == nullptr)
            return jobHasFinished;

        // Find the requested chop in the snapshot.
        const ChopDefinition* targetChop = nullptr;
        for (const auto& c : chopStateSnapshot->chops)
        {
            if (c.id == chopId)
            {
                targetChop = &c;
                break;
            }
        }

        if (targetChop == nullptr)
            return jobHasFinished;

        if (shouldExit())
            return jobHasFinished;

        // Check if this job is stale by comparing warp markers with the current state.
        if (auto currentChopState = processor.getChopState())
        {
            const ChopDefinition* currentChop = nullptr;
            for (const auto& c : currentChopState->chops)
            {
                if (c.id == chopId)
                {
                    currentChop = &c;
                    break;
                }
            }

            if (currentChop == nullptr || currentChop->warpMarkers != targetChop->warpMarkers)
            {
                // The user has edited the warp markers for this chop since this job was queued.
                // Abort rendering to prevent CPU backlog and UI lag.
                return jobHasFinished;
            }
        }

        auto entry = cuesampler::ChopAudioCache::renderChopSync (
            sample->buffer,
            sample->sampleRate,
            chopId,
            targetChop->startSample,
            targetChop->endSample,
            targetChop->warpMarkers,
            generation);

        if (! shouldExit() && entry != nullptr)
            cache.store (entry);

        return jobHasFinished;
    }

private:
    AudioPluginAudioProcessor& processor;
    cuesampler::ChopAudioCache& cache;
    std::shared_ptr<const LoadedSampleData> sample;
    std::shared_ptr<const ChopState> chopStateSnapshot;
    int chopId = 0;
    std::uint64_t generation = 0;
};

//==============================================================================
// Bakes every non-warp chop's pitch+time-stretched audio (at the current global
// pitch/stretch) into the prepared-entry cache so the audio thread can stream it
// 1:1 — no real-time Bungee. Skips chops that are unity (the live path is already
// cheap there) or warped (those need per-sample WarpMap mapping; left on the live
// path). Self-cancels when superseded by a newer warm (prepareWarmGeneration).
class AudioPluginAudioProcessor::PreparedWarmJob final : public juce::ThreadPoolJob
{
public:
    PreparedWarmJob (AudioPluginAudioProcessor& processorIn,
                     cuesampler::ChopAudioCache& cacheIn,
                     std::shared_ptr<const LoadedSampleData> sampleIn,
                     std::shared_ptr<const ChopState> chopStateIn,
                     double sourceRateIn,
                     double outputRateIn,
                     float globalPitchSemitonesIn,
                     float stretchRatioIn,
                     int priorityChopIdIn,
                     std::uint64_t generationIn)
        : juce::ThreadPoolJob ("Prepared Warm"),
          processor (processorIn),
          cache (cacheIn),
          sample (std::move (sampleIn)),
          chopStateSnapshot (std::move (chopStateIn)),
          sourceRate (sourceRateIn),
          outputRate (outputRateIn),
          globalPitchSemitones (globalPitchSemitonesIn),
          stretchRatio (stretchRatioIn),
          priorityChopId (priorityChopIdIn),
          generation (generationIn)
    {
    }

    JobStatus runJob() override
    {
        if (sample == nullptr || chopStateSnapshot == nullptr
            || sample->buffer.getNumSamples() <= 0)
            return jobHasFinished;

        const bool stretchUnity = std::abs (stretchRatio - 1.0f) < 0.005f;
        const bool ratesMatch   = std::abs (sourceRate - outputRate) < 0.5;

        // Bake one chop (no-op if warped, unity, already cached, or superseded).
        auto bakeOne = [&] (const ChopDefinition& chop) -> bool
        {
            if (shouldExit()
                || processor.prepareWarmGeneration.load (std::memory_order_acquire) != generation)
                return false;

            // Warp chops stay on the live path (their WarpMap mapping is not a
            // straight 1:1 read), so don't spend RAM/CPU baking them here.
            if (! chop.warpMarkers.empty())
                return true;

            const float effPitch = juce::jlimit (-24.0f, 24.0f,
                                                 globalPitchSemitones + chop.pitchSemitones);
            const bool pitchUnity = std::abs (effPitch) < 0.01f;

            // Unity playback already uses the cheap interpolation path — no Bungee
            // to displace, so a prepared buffer would only waste memory.
            if (pitchUnity && stretchUnity && ratesMatch)
                return true;

            const auto key = cuesampler::ChopAudioCache::makePreparedKey (
                chop.startSample, chop.endSample, chop.cueOffsetSamples,
                chop.warpMarkers, sourceRate, outputRate, effPitch, stretchRatio);

            if (cache.getPrepared (chop.id, key) != nullptr)
                return true; // already baked for the current settings

            auto entry = cuesampler::ChopAudioCache::renderPreparedChopSync (
                sample->buffer, sourceRate, outputRate, chop.id,
                chop.startSample, chop.endSample, chop.cueOffsetSamples,
                chop.warpMarkers, effPitch, stretchRatio, generation);

            if (entry != nullptr && entry->buffer != nullptr
                && entry->buffer->getNumSamples() > 0
                && ! shouldExit()
                && processor.prepareWarmGeneration.load (std::memory_order_acquire) == generation)
                cache.storePrepared (entry);

            return true;
        };

        // Bake the chop the user is most likely hearing first, so the current
        // loop turns smooth in the first pass rather than the second.
        if (priorityChopId >= 0)
            for (const auto& chop : chopStateSnapshot->chops)
                if (chop.id == priorityChopId)
                {
                    if (! bakeOne (chop))
                        return jobHasFinished;
                    break;
                }

        for (const auto& chop : chopStateSnapshot->chops)
            if (! bakeOne (chop))
                break;

        return jobHasFinished;
    }

private:
    AudioPluginAudioProcessor& processor;
    cuesampler::ChopAudioCache& cache;
    std::shared_ptr<const LoadedSampleData> sample;
    std::shared_ptr<const ChopState> chopStateSnapshot;
    double sourceRate = 44100.0;
    double outputRate = 44100.0;
    float  globalPitchSemitones = 0.0f;
    float  stretchRatio = 1.0f;
    int    priorityChopId = -1;
    std::uint64_t generation = 0;
};

//==============================================================================
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
{
    std::atomic_store (&tempoEditState, std::make_shared<TempoEditState>());
    std::atomic_store (&chopState, std::make_shared<ChopState>());

    // Kick off the (throttled) software-update check. Loads any cached result
    // immediately and refreshes from GitHub Releases at most once per day.
    updateChecker.start();

    // Initialise beat_this neural analyzer.
    // The ONNX model sits next to the plugin binary; for development builds
    // it lives in assets/ next to this source file.
    auto findOnnxModel = [] () -> juce::String
    {
        const auto binaryDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                   .getParentDirectory();

        // 1. Contents/Resources/ — standard bundle location for distribution builds
        const auto atResources = binaryDir.getParentDirectory()
                                     .getChildFile ("Resources/beat_this.onnx");
        if (atResources.existsAsFile())
            return atResources.getFullPathName();

        // 2. Next to the binary — legacy / fallback
        const auto atBinary = binaryDir.getChildFile ("beat_this.onnx");
        if (atBinary.existsAsFile())
            return atBinary.getFullPathName();

        // 3. assets/ folder alongside source (development build). __FILE__ is
        //    the absolute source path baked in at compile time, so on a dev
        //    machine this resolves to the repo's assets/ folder directly.
        const auto atSource = juce::File (__FILE__).getParentDirectory()
                                  .getChildFile ("assets/beat_this.onnx");
        if (atSource.existsAsFile())
            return atSource.getFullPathName();

        return {};
    };

    const auto onnxPath = findOnnxModel();
    if (onnxPath.isNotEmpty())
    {
        beatThisAnalyzer = std::make_unique<BeatThisAnalyzer> (onnxPath);
        juce::Logger::writeToLog ("BeatThisAnalyzer: ready=" + juce::String (beatThisAnalyzer->isReady() ? "YES" : "NO"));
    }
    else
    {
        juce::Logger::writeToLog ("BeatThisAnalyzer: beat_this.onnx not found, using autocorrelation fallback");
    }

    // Initialise the stem separator. The single base-htdemucs model lives in an
    // htdemucs/ folder resolved with the SAME precedence as beat_this above
    // (Contents/Resources → next to the binary → dev-tree assets/). A missing
    // model just leaves the separator not-ready and the plugin behaves as before.
    auto findStemModel = [] () -> juce::File
    {
        const auto binaryDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                   .getParentDirectory();

        const auto atResources = binaryDir.getParentDirectory().getChildFile ("Resources/htdemucs/htdemucs.onnx");
        if (atResources.existsAsFile())
            return atResources;

        const auto atBinary = binaryDir.getChildFile ("htdemucs/htdemucs.onnx");
        if (atBinary.existsAsFile())
            return atBinary;

        const auto atSource = juce::File (__FILE__).getParentDirectory().getChildFile ("assets/htdemucs/htdemucs.onnx");
        if (atSource.existsAsFile())
            return atSource;

        return {};
    };

    if (const auto stemModelFile = findStemModel(); stemModelFile != juce::File())
    {
        StemSeparator::ModelPaths paths;
        paths.model = stemModelFile.getFullPathName();
        stemSeparator = std::make_unique<StemSeparator> (paths);
        juce::Logger::writeToLog ("StemSeparator: model at " + stemModelFile.getFullPathName()
                                  + " ready=" + juce::String (stemSeparator->isReady() ? "YES" : "NO"));
    }
    else
    {
        juce::Logger::writeToLog ("StemSeparator: htdemucs model not found, stem separation disabled");
    }

    // Keep the prepared-render cache warm independently of the editor (the UI may
    // be closed). Polls often enough to start baking promptly after a sample loads
    // or settings change; it only kicks a background bake when something relevant
    // actually changed.
    startTimerHz (20);
}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    editTelemetry.flush();
    keyDetectionGeneration.fetch_add (1, std::memory_order_acq_rel);
    prepareWarmGeneration.fetch_add (1, std::memory_order_acq_rel);
    stemGeneration.fetch_add (1, std::memory_order_acq_rel);
    stemRemixGeneration.fetch_add (1, std::memory_order_acq_rel);
    restoreThreadPool.removeAllJobs (true, 2000);
    analysisThreadPool.removeAllJobs (true, 2000);
    warpRenderThreadPool.removeAllJobs (true, 2000);
    keyDetectionThreadPool.removeAllJobs (true, 2000);
    prepareRenderThreadPool.removeAllJobs (true, 2000);
    // Separation can be mid-flight (tens of seconds); give it a generous window.
    stemThreadPool.removeAllJobs (true, 10000);
}

void AudioPluginAudioProcessor::timerCallback()
{
    warmPreparedCacheTick();
}

void AudioPluginAudioProcessor::warmPreparedCacheTick()
{
    const auto sample = std::atomic_load (&loadedSample);
    const auto chops  = std::atomic_load (&chopState);
    if (sample == nullptr || chops == nullptr || chops->chops.empty()
        || sample->buffer.getNumSamples() <= 0)
        return;

    const double sourceRate = juce::jmax (1.0, sample->sampleRate);
    const double hostRate   = juce::jmax (1.0, hostSampleRate.load (std::memory_order_acquire));

    // Effective global pitch/stretch the audio thread will look up with. Mirror
    // exactly the render path: half-time doubles the stretch ratio (clamped).
    const float globalPitch = pitchSemitones.load (std::memory_order_acquire);
    float stretch = juce::jlimit (0.25f, 4.0f, timeStretchRatio.load (std::memory_order_acquire));
    if (halfTimeEnabled.load (std::memory_order_acquire))
        stretch = juce::jlimit (0.25f, 4.0f, stretch * 2.0f);

    const int pitchCents = (int) std::lround (globalPitch * 100.0f);
    const int stretchPpm = (int) std::lround (stretch * 100000.0f);

    // Nothing relevant changed since the last warm → the cache is already correct.
    // 'sample' identity is part of the key: a stem mute swaps loadedSample to a new
    // buffer with the SAME chopState, so without this the prepared cache would keep
    // serving pre-mute audio.
    if (lastWarmValid
        && pitchCents == lastWarmPitchCents
        && stretchPpm == lastWarmStretchPpm
        && std::abs (sourceRate - lastWarmSourceRate) < 0.5
        && std::abs (hostRate - lastWarmHostRate) < 0.5
        && chops == lastWarmChopState
        && sample == lastWarmSample)
        return;

    lastWarmValid      = true;
    lastWarmPitchCents = pitchCents;
    lastWarmStretchPpm = stretchPpm;
    lastWarmSourceRate = sourceRate;
    lastWarmHostRate   = hostRate;
    lastWarmChopState  = chops;
    lastWarmSample     = sample;

    // Supersede any in-flight warm for stale settings, then bake the new set,
    // starting with the chop the user is most likely hearing right now.
    const int priorityChopId = lastTriggeredChopId.load (std::memory_order_acquire);
    const auto gen = prepareWarmGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    prepareRenderThreadPool.removeAllJobs (false, 0);
    prepareRenderThreadPool.addJob (new PreparedWarmJob (*this, chopAudioCache, sample, chops,
                                                         sourceRate, hostRate, globalPitch, stretch,
                                                         priorityChopId, gen),
                                    true);
}

//==============================================================================
const juce::String AudioPluginAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioPluginAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int AudioPluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioPluginAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioPluginAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate.store (sampleRate, std::memory_order_release);
    outputMeterLevel.store (0.0f, std::memory_order_release);

    const auto compChannels = juce::jmax (1, getTotalNumOutputChannels());
    compressor.prepare (sampleRate, juce::jmax (1, samplesPerBlock), compChannels);
    bitCrusher.prepare (sampleRate, juce::jmax (1, samplesPerBlock), compChannels);

    for (auto& v : voices)
        v.reset();

    // (Re)build the per-voice Bungee engines so they match the new host rate.
    // If a sample is already loaded, ensurePitchEnginesReady will use its
    // sample rate; otherwise it tears the engines down until a sample lands.
    const auto loaded   = std::atomic_load (&loadedSample);
    const auto srcRate  = (loaded != nullptr) ? loaded->sampleRate : sampleRate;
    const auto channels = juce::jmax (1, getTotalNumOutputChannels());
    ensurePitchEnginesReady (srcRate, sampleRate, channels);
}

void AudioPluginAudioProcessor::releaseResources()
{
    for (auto& v : voices)
        v.reset();

    std::atomic_store (&pitchEngineSet, std::shared_ptr<const VoicePitchEngineSet> {});
    compressor.reset();
    bitCrusher.reset();
    outputMeterLevel.store (0.0f, std::memory_order_release);
}

void AudioPluginAudioProcessor::ensurePitchEnginesReady (double sourceRate,
                                                          double hostRate,
                                                          int    channels)
{
    const int srcRateInt  = (int) std::round (juce::jmax (1.0, sourceRate));
    const int hostRateInt = (int) std::round (juce::jmax (1.0, hostRate));
    channels = juce::jlimit (1, 8, channels);

    const auto current = std::atomic_load (&pitchEngineSet);
    if (current != nullptr
        && current->inputSampleRate == srcRateInt
        && current->outputSampleRate == hostRateInt
        && current->channelCount == channels)
    {
        return;
    }

    auto nextSet = std::make_shared<VoicePitchEngineSet>();
    nextSet->generation = pitchEngineGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    nextSet->inputSampleRate = srcRateInt;
    nextSet->outputSampleRate = hostRateInt;
    nextSet->channelCount = channels;

    for (auto& slot : nextSet->voices)
    {
        auto engine = std::make_unique<VoicePitchEngine>();
        engine->channelCount     = channels;
        engine->inputSampleRate  = srcRateInt;
        engine->outputSampleRate = hostRateInt;

        const Bungee::SampleRates rates { srcRateInt, hostRateInt };
        // log2SynthesisHopAdjust = -1 → smaller grains, lower latency on note
        // triggers and snappier transients. Matches the offline export path.
        engine->stretcher = std::make_unique<Bungee::Stretcher<Bungee::Basic>> (rates, channels, -1);

        // Stream's input ring is sized for `stretcher.maxInputFrameCount() + maxInputFrameCount`
        // frames per channel; choose a comfortable per-call max so we never have to grow.
        const int maxStreamInput = juce::jmax (1024, engine->stretcher->maxInputFrameCount());
        engine->stream = std::make_unique<Bungee::Stream<Bungee::Basic>> (*engine->stretcher,
                                                                          maxStreamInput,
                                                                          channels);

        // Preallocate the scratch buffers used while priming and chunked
        // rendering. Using stretcher->maxInputFrameCount() guarantees we can
        // satisfy any single grain in one shot.
        engine->scratchInput .setSize (channels, maxStreamInput, false, true, true);
        engine->discardOutput.setSize (channels, maxStreamInput, false, true, true);
        engine->scratchInput .clear();
        engine->discardOutput.clear();

        slot = std::move (engine);
    }

    std::atomic_store (&pitchEngineSet,
                       std::static_pointer_cast<const VoicePitchEngineSet> (nextSet));
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

void AudioPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());
    const auto analysis = std::atomic_load (&tempoAnalysis);

    // Mirror host MIDI into the on-screen keyboard state and inject notes the
    // user plays by clicking it, so clicked keys trigger chops like real MIDI.
    keyboardState.processNextMidiBuffer (midiMessages, 0, buffer.getNumSamples(), true);
    
    // Host sync snapshot
    double blockStartHostPpq = 0.0;
    bool blockHasHostPpq = false;
    bool blockHostTransportPlaying = false;

    if (auto* playHead = getPlayHead())
    {
        if (const auto pos = playHead->getPosition())
        {
            if (const auto bpm = pos->getBpm())
                hostBpm.store (*bpm, std::memory_order_release);

            if (const auto ppq = pos->getPpqPosition())
            {
                blockStartHostPpq = *ppq;
                blockHasHostPpq = true;
                hostPpqPosition.store (*ppq, std::memory_order_release);
            }

            blockHostTransportPlaying = pos->getIsPlaying();
        }
    }

    hostHasPpqPosition.store (blockHasHostPpq, std::memory_order_release);
    hostTransportPlaying.store (blockHostTransportPlaying, std::memory_order_release);

    if (syncToHost.load (std::memory_order_acquire))
    {
        const auto currentHostBpm = hostBpm.load (std::memory_order_acquire);
        if (analysis != nullptr)
        {
            updateHostSyncStretchRatio (*analysis, currentHostBpm);
        }
        else if (std::abs (timeStretchRatio.load (std::memory_order_acquire) - 1.0f) > 0.0005f)
        {
            timeStretchRatio.store (1.0f, std::memory_order_release);
            voices[activeVoiceIdx].bungeeResetPending = true;
        }
    }

    juce::ignoreUnused (totalNumInputChannels);

    for (auto i = 0; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const auto sampleForBlock = std::atomic_load (&loadedSample);
    const auto currentChopState = sampleForBlock != nullptr ? std::atomic_load (&chopState) : nullptr;
    const auto currentHostRate = juce::jmax (1.0, hostSampleRate.load (std::memory_order_acquire));
    const auto enginesForBlock = std::atomic_load (&pitchEngineSet);

    const auto pendingCommand = (TransportCommand) pendingTransportCommand.exchange ((int) TransportCommand::none,
                                                                                     std::memory_order_acq_rel);
    if (pendingCommand != TransportCommand::none)
    {
        auto& v = voices[activeVoiceIdx];

        if (pendingCommand == TransportCommand::pause)
        {
            v.playbackActive = false;
            playbackActive.store (false, std::memory_order_release);
        }
        else if (pendingCommand == TransportCommand::stop)
        {
            for (auto& voice : voices)
                voice.reset();

            playbackActive.store (false, std::memory_order_release);
            playbackSamplePosition.store (0.0, std::memory_order_release);
        }
        else if (pendingCommand == TransportCommand::silence)
        {
            for (auto& voice : voices)
                voice.reset();

            playbackActive.store (false, std::memory_order_release);
        }
        else if (pendingCommand == TransportCommand::seek)
        {
            if (sampleForBlock == nullptr || sampleForBlock->buffer.getNumSamples() == 0)
            {
                playbackSamplePosition.store (0.0, std::memory_order_release);
            }
            else
            {
                const auto maxPosition = juce::jmax (0.0, (double) sampleForBlock->buffer.getNumSamples() - 1.0);
                const auto clampedPosition = juce::jlimit (0.0, maxPosition,
                                                           pendingTransportSeekPosition.load (std::memory_order_acquire));

                v.playbackSamplePosition = clampedPosition;
                v.playbackStopSample = -1.0;
                v.bungeeResetPending = true;

                if (syncToHost.load (std::memory_order_acquire)
                    && blockHasHostPpq
                    && hostBpm.load (std::memory_order_acquire) > 0.0)
                {
                    v.playbackSyncStartPpq = blockStartHostPpq;
                    v.playbackSyncStartSample = clampedPosition;
                    v.playbackSyncAnchorValid = true;
                }
                else
                {
                    v.playbackSyncAnchorValid = false;
                }

                playbackSamplePosition.store (clampedPosition, std::memory_order_release);
            }
        }
        else if (pendingCommand == TransportCommand::start
                 && sampleForBlock != nullptr
                 && sampleForBlock->buffer.getNumSamples() > 0)
        {
            voices[1 - activeVoiceIdx].reset();
            v.playbackSamplePosition = pendingStartPosition.load (std::memory_order_acquire);
            v.playbackStopSample = pendingStartStopSample.load (std::memory_order_acquire);
            v.playbackLoopStartSample = pendingStartLoopSample.load (std::memory_order_acquire);
            v.playbackTriggeredByMidi = false;
            v.playbackActive = true;
            v.bungeeResetPending = true;
            v.fadeGain = 1.0f;
            v.fadeTarget = 1.0f;
            v.fadeStep = 0.0f;
            v.playbackSyncStartPpq = pendingStartSyncPpq.load (std::memory_order_acquire);
            v.playbackSyncStartSample = pendingStartSyncSample.load (std::memory_order_acquire);
            v.playbackSyncAnchorValid = pendingStartSyncAnchorValid.load (std::memory_order_acquire);

            playbackActive.store (true, std::memory_order_release);
            playbackSamplePosition.store (v.playbackSamplePosition, std::memory_order_release);
        }
    }
    
    int samplesToProcess = buffer.getNumSamples();
    int currentOffset = 0;
    
    auto renderVoiceSegment = [&](VoiceState& v, int voiceIndex, int segmentOffset, int segmentSamples)
    {
        if (! v.playbackActive || sampleForBlock == nullptr)
            return;

        const auto sourceLength = sampleForBlock->buffer.getNumSamples();
        const auto outputChannels = buffer.getNumChannels();
        const auto sourceRate = juce::jmax (1.0, sampleForBlock->sampleRate);
        
        auto stretchRatio = juce::jlimit (0.25f, 4.0f, timeStretchRatio.load (std::memory_order_acquire));
        const auto halfTimeActive = halfTimeEnabled.load (std::memory_order_acquire);
        if (halfTimeActive) stretchRatio = juce::jlimit (0.25f, 4.0f, stretchRatio * 2.0f);

        const auto sourceFramesPerHostFrame = sourceRate / currentHostRate;
        const auto sourceFramesPerOutputFrame = sourceFramesPerHostFrame / (double) stretchRatio;
        
        // Handle Host Sync for this voice if applicable
        if (! v.playbackTriggeredByMidi
            && syncToHost.load (std::memory_order_acquire) && blockHostTransportPlaying && blockHasHostPpq &&
            hostBpm.load (std::memory_order_acquire) > 0.0 && analysis != nullptr &&
            analysis->estimatedBpm > 0.0 && v.playbackSyncAnchorValid)
        {
            double beatPeriodSeconds, barPeriodSeconds, chopPeriodSeconds, gridAnchorSeconds;
            if (computeGridTimingMetrics (*analysis, beatPeriodSeconds, barPeriodSeconds, chopPeriodSeconds, gridAnchorSeconds))
            {
                const auto syncAnchorPpq = v.playbackSyncStartPpq;
                const auto syncAnchorSample = v.playbackSyncStartSample;
                // Offset current segment timestamp to host PPQ
                double currentPpq = blockStartHostPpq + (double) segmentOffset / currentHostRate * (hostBpm.load() / 60.0);
                auto syncedSourcePosition = syncAnchorSample + (currentPpq - syncAnchorPpq) * beatPeriodSeconds * sourceRate * (halfTimeActive ? 0.5 : 1.0);
                
                if (v.playbackStopSample > syncAnchorSample + 1.0)
                    syncedSourcePosition = wrapToLoopRange (syncedSourcePosition, syncAnchorSample, v.playbackStopSample - syncAnchorSample);

                if (v.playbackStopSample < 0.0 && syncedSourcePosition >= (double) sourceLength)
                {
                    v.playbackActive = false;
                    return;
                }
                
                if (std::abs (syncedSourcePosition - v.playbackSamplePosition) > juce::jmax (32.0, sourceFramesPerOutputFrame * 4.0))
                    v.bungeeResetPending = true;

                v.playbackSamplePosition = juce::jlimit (0.0, (double) juce::jmax (0, sourceLength - 1), syncedSourcePosition);
            }
        }

        int segmentWriteOffset = 0;
        while (segmentWriteOffset < segmentSamples && v.playbackActive)
        {
            int remainingInSegment = segmentSamples - segmentWriteOffset;
            int framesToRender = remainingInSegment;

            // Single live sampler path: source sample + current chop edits are
            // evaluated directly, so playback never swaps between render engines.
            const auto* activeChop = findChopAtSample (currentChopState.get(), v.playbackSamplePosition);
            float chopGainLinear = 1.0f;
            auto effectivePitchSemitones = pitchSemitones.load (std::memory_order_acquire);
            double cueStart = v.playbackSamplePosition;
            if (activeChop != nullptr
                && v.playbackSamplePosition >= (double) activeChop->startSample
                && v.playbackSamplePosition <  (double) activeChop->endSample)
            {
                chopGainLinear = juce::Decibels::decibelsToGain (activeChop->gainDecibels);
                effectivePitchSemitones = juce::jlimit (-24.0f, 24.0f,
                                                        effectivePitchSemitones + activeChop->pitchSemitones);
                cueStart = (double) juce::jlimit (activeChop->startSample,
                                                   juce::jmax (activeChop->startSample, activeChop->endSample - 1),
                                                   activeChop->startSample + activeChop->cueOffsetSamples);
            }
            const bool chopHasWarp = (activeChop != nullptr && ! activeChop->warpMarkers.empty());

            // Try to fetch the pre-baked pitch-neutral warp buffer from the cache.
            // renderWarpedChopBungee bakes each segment at pitchFactor=1.0 so the
            // warped buffer plays at the correct pitch regardless of stretch ratio.
            std::shared_ptr<const cuesampler::ChopAudioCache::Entry> warpCacheEntry;
            const juce::AudioBuffer<float>* warpedBuf = nullptr;
            if (chopHasWarp && activeChop != nullptr)
            {
                warpCacheEntry = chopAudioCache.get (activeChop->id);
                if (warpCacheEntry != nullptr && warpCacheEntry->warpedBuffer != nullptr
                    && warpCacheEntry->warpedBuffer->getNumSamples() > 0)
                {
                    warpedBuf = warpCacheEntry->warpedBuffer.get();
                }
            }

            // When reading from the cached warped buffer, playbackSamplePosition is
            // treated as an offset into that buffer (in source-sample-rate units).
            // We advance at the stretch-adjusted rate (sourceFramesPerOutputFrame)
            // so warped chops correctly time-stretch to fit DAW tempo changes.
            const auto interpolatedSourceStep = sourceFramesPerOutputFrame;  // for non-warp path
            const auto warpedBufStep          = sourceFramesPerOutputFrame;  // for warp-cache path
            const auto livePitchFactor = std::pow (2.0, (double) effectivePitchSemitones / 12.0);
            const bool pitchIsUnity   = std::abs (effectivePitchSemitones) < 0.01f;
            const bool stretchIsUnity = std::abs (stretchRatio - 1.0f) < 0.005f;

            // Choose step for chunk-size calculation.
            const auto activeStep = (warpedBuf != nullptr) ? warpedBufStep : interpolatedSourceStep;

            int chunk = framesToRender;
            if (v.playbackStopSample > 0.0 && v.playbackSamplePosition < v.playbackStopSample)
            {
                chunk = juce::jmin (chunk,
                                     juce::jmax (1,
                                                 (int) std::floor ((v.playbackStopSample - v.playbackSamplePosition)
                                                                     / activeStep)));
            }

            const int interpRadius = 3;
            const int fadeSamples  = v.playbackTriggeredByMidi ? midiSliceStartFadeSamples
                                                                : defaultSliceStartFadeSamples;

            // Decide which engine handles this chunk.
            VoicePitchEngine* pitchEngine = nullptr;
            if (enginesForBlock != nullptr
                && voiceIndex >= 0
                && voiceIndex < (int) enginesForBlock->voices.size()
                && enginesForBlock->generation != 0)
            {
                pitchEngine = enginesForBlock->voices[(size_t) voiceIndex].get();
                if (v.observedPitchEngineGeneration != enginesForBlock->generation)
                {
                    v.observedPitchEngineGeneration = enginesForBlock->generation;
                    v.bungeeResetPending = true;
                }
            }

            const bool engineReady =
                pitchEngine != nullptr
                && pitchEngine->stretcher != nullptr
                && pitchEngine->stream != nullptr
                && pitchEngine->channelCount > 0
                && pitchEngine->inputSampleRate == (int) std::round (sourceRate)
                && pitchEngine->outputSampleRate == (int) std::round (currentHostRate);

            const bool useBungee =
                engineReady
                && (! pitchIsUnity || ! stretchIsUnity)
                && (! chopHasWarp || warpedBuf != nullptr);

            // Source abstraction: warp chops feed the warp buffer into Bungee
            // for pitch and time-stretch processing. Non-warp chops use the
            // original source buffer with stretch.
            const bool   bungeeUsesWarpBuf = chopHasWarp && warpedBuf != nullptr;
            const auto*  bungeeSource      = bungeeUsesWarpBuf ? warpedBuf : &sampleForBlock->buffer;
            const int    bungeeSrcLen      = bungeeSource->getNumSamples();
            const int    bungeeSrcStart    = juce::jlimit (0, juce::jmax (0, bungeeSrcLen - 1),
                (int) std::floor (bungeeUsesWarpBuf
                                      ? v.playbackSamplePosition - (double) activeChop->startSample
                                      : v.playbackSamplePosition));
            const double bungeeInPerOut    = sourceFramesPerOutputFrame;
            const double bungeePosStep     = sourceFramesPerOutputFrame;

            // ----- Track if this iteration was a Bungee path so we can skip
            // the Lanczos fallback below when it succeeds. -----
            bool handledByBungee = false;

            // ----- Pre-rendered (prepared) fast path -------------------------------
            // If a background warm has baked this non-warp chop's pitch+time-stretched
            // audio for the current settings, stream it 1:1 instead of running Bungee
            // on the audio thread. This is what keeps CPU flat during playback (esp.
            // when the host downclocks the audio thread with the UI closed). Any miss
            // (settings just changed, not baked yet) simply falls through to the live
            // Bungee path below, so playback is never interrupted.
            bool handledByPrepared = false;
            if (! chopHasWarp && activeChop != nullptr && (! pitchIsUnity || ! stretchIsUnity))
            {
                const auto preparedKey = cuesampler::ChopAudioCache::makePreparedKey (
                    activeChop->startSample, activeChop->endSample, activeChop->cueOffsetSamples,
                    activeChop->warpMarkers, sourceRate, currentHostRate,
                    effectivePitchSemitones, stretchRatio);

                if (auto prepared = chopAudioCache.getPrepared (activeChop->id, preparedKey);
                    prepared != nullptr && prepared->buffer != nullptr
                    && prepared->buffer->getNumSamples() > 0)
                {
                    const auto*  pbuf     = prepared->buffer.get();
                    const int    pLen     = pbuf->getNumSamples();
                    const int    pChans   = pbuf->getNumChannels();
                    const double invStep  = 1.0 / juce::jmax (1.0e-9, sourceFramesPerOutputFrame);
                    const double chopStart = (double) activeChop->startSample;

                    for (int i = 0; i < chunk; ++i)
                    {
                        const double preparedFrame = (v.playbackSamplePosition - chopStart) * invStep;
                        const auto boundaryGain = computeSliceBoundaryGain (v.playbackSamplePosition, cueStart,
                                                                             v.playbackStopSample, fadeSamples,
                                                                             sliceEndFadeSamples);
                        const float totalGain = chopGainLinear * v.midiVelocity * boundaryGain * v.fadeGain;

                        for (int ch = 0; ch < outputChannels; ++ch)
                        {
                            const auto srcCh = juce::jmin (ch, pChans - 1);
                            const auto* pdata = pbuf->getReadPointer (srcCh);
                            auto* outData = buffer.getWritePointer (ch);
                            outData[segmentOffset + segmentWriteOffset + i] +=
                                interpolateSampleLanczos (pdata, pLen, preparedFrame, interpRadius) * totalGain;
                        }
                        v.playbackSamplePosition += interpolatedSourceStep;
                        v.updateFade();
                    }

                    segmentWriteOffset += chunk;
                    // If we later fall back to Bungee (settings change mid-note), it
                    // must re-prime from the new position.
                    v.bungeeResetPending = true;
                    handledByPrepared = true;
                }
            }

            if (! handledByPrepared && useBungee)
            {
                auto& engine    = *pitchEngine;
                auto& stream    = *engine.stream;
                auto& stretcher = *engine.stretcher;
                const int channels = engine.channelCount;
                const int maxScratchInFrames  = engine.scratchInput.getNumSamples();
                const int maxScratchOutFrames = engine.discardOutput.getNumSamples();

                // At extreme stretch ratios the input we'd need to feed to
                // produce `chunk` output samples can exceed scratchInput. Cap
                // chunk for this iteration so the outer while-loop picks up
                // the remainder on the next pass — never leaves a hole.
                const double safeStep = juce::jmax (1.0e-3, bungeeInPerOut);
                const int maxChunkForInputBuf = juce::jmax (1,
                    (int) std::floor ((double) maxScratchInFrames / safeStep));
                const int maxChunkForOutputBuf = juce::jmax (1, maxScratchOutFrames);
                chunk = juce::jmin (chunk, juce::jmin (maxChunkForInputBuf, maxChunkForOutputBuf));

                // Synchronous prime on note-on / position jump / sample swap.
                if (v.bungeeResetPending)
                {
                    stream.reset();

                    const int leadInWanted = stretcher.maxInputFrameCount();
                    const int leadInAvail  = juce::jmin (bungeeSrcStart, leadInWanted);

                    // The synchronous note-on prime synthesises roughly
                    // (leadInAvail / primeInPerOut) discarded grains to warm Bungee's
                    // look-ahead. Slow-down host-sync stretch and half-time push
                    // bungeeInPerOut below 1.0, which would multiply that grain count
                    // (e.g. ~2x at half-time) — a per-note CPU burst large enough to
                    // overrun the audio buffer when the host has downclocked the core
                    // (commonly with the UI closed), causing audible spikes.
                    //
                    // Warming the look-ahead only depends on how much INPUT we feed:
                    // Bungee centres its final grain on inputBuffer.endPosition(), not on
                    // the discarded output count (see Stream.h). So priming at an
                    // effective speed of >= 1.0 reaches the same warmed state and the same
                    // playback start position, while capping note-on cost at that of a
                    // normal unity-stretch note for every pitch/stretch/half-time setting.
                    // The actual (stretched/half-time) render below is left untouched.
                    const double primeInPerOut = juce::jmax (1.0, bungeeInPerOut);

                    if (leadInAvail > 0)
                    {
                        // Cap per-call input so output (input / primeInPerOut)
                        // can never exceed the discard buffer at extreme stretch.
                        const double safeOutputPerInput = juce::jmax (0.25, 1.0 / juce::jmax (1.0e-3, primeInPerOut));
                        const int    perCallInputCap   = juce::jmax (1,
                            juce::jmin (maxScratchInFrames,
                                         (int) std::floor ((double) (maxScratchOutFrames - 4) / safeOutputPerInput)));
                        const int    primeBlock        = juce::jmin (perCallInputCap, leadInAvail);

                        int fed = 0;
                        while (fed < leadInAvail)
                        {
                            const int feedNow      = juce::jmin (primeBlock, leadInAvail - fed);
                            const int readStart    = bungeeSrcStart - leadInAvail + fed;
                            const double primeOut  = juce::jlimit (1.0,
                                                                    (double) maxScratchOutFrames,
                                                                    juce::jmax (1.0,
                                                                                std::ceil ((double) feedNow / juce::jmax (1.0e-3, primeInPerOut))));

                            for (int ch = 0; ch < channels; ++ch)
                            {
                                const auto srcCh = juce::jmin (ch, bungeeSource->getNumChannels() - 1);
                                const auto* srcData = bungeeSource->getReadPointer (srcCh, readStart);
                                auto* dst = engine.scratchInput.getWritePointer (ch);
                                std::copy (srcData, srcData + feedNow, dst);
                            }

                            const float* inPtrs[8]  = {};
                            float*       outPtrs[8] = {};
                            for (int ch = 0; ch < juce::jmin (channels, 8); ++ch)
                            {
                                inPtrs[ch]  = engine.scratchInput.getReadPointer (ch);
                                outPtrs[ch] = engine.discardOutput.getWritePointer (ch);
                            }
                            stream.process (inPtrs, outPtrs, feedNow, primeOut, livePitchFactor);
                            fed += feedNow;
                        }
                    }

                    v.bungeeResetPending = false;
                }

                // Compute how many source frames to feed for this output chunk.
                int inputFrames = juce::jmax (1,
                                               (int) std::round ((double) chunk * bungeeInPerOut));
                inputFrames = juce::jmin (inputFrames, maxScratchInFrames);
                const int sourceAvailable = juce::jmax (0, bungeeSrcLen - bungeeSrcStart);
                const int inputCopy       = juce::jmin (inputFrames, sourceAvailable);

                // Feed scratchInput from the source buffer, zero-pad if we ran
                // off the end (chop boundary or end-of-sample).
                for (int ch = 0; ch < channels; ++ch)
                {
                    const auto srcCh = juce::jmin (ch, bungeeSource->getNumChannels() - 1);
                    const auto* srcData = inputCopy > 0
                                            ? bungeeSource->getReadPointer (srcCh, bungeeSrcStart)
                                            : nullptr;
                    auto* dst = engine.scratchInput.getWritePointer (ch);
                    if (inputCopy > 0)
                        std::copy (srcData, srcData + inputCopy, dst);
                    if (inputCopy < inputFrames)
                        std::fill (dst + inputCopy, dst + inputFrames, 0.0f);
                }

                const float* inPtrs[8]  = {};
                float*       outPtrs[8] = {};
                for (int ch = 0; ch < juce::jmin (channels, 8); ++ch)
                {
                    inPtrs[ch]  = engine.scratchInput.getReadPointer (ch);
                    outPtrs[ch] = engine.discardOutput.getWritePointer (ch);
                }
                const int rendered = stream.process (inPtrs, outPtrs, inputFrames, (double) chunk, livePitchFactor);
                const int writeCount = juce::jmin (rendered, chunk);

                // Apply per-sample fades / chop boundary gain / MIDI velocity
                // and mix into the output. Position is the conceptual source
                // position the output sample corresponds to (same formula the
                // Lanczos fallback uses), so boundary fades stay aligned.
                const double conceptualPosStart = v.playbackSamplePosition;
                for (int i = 0; i < writeCount; ++i)
                {
                    const double conceptualPos = conceptualPosStart + (double) i * bungeePosStep;
                    const auto boundaryGain = computeSliceBoundaryGain (conceptualPos, cueStart,
                                                                         v.playbackStopSample, fadeSamples,
                                                                         sliceEndFadeSamples);
                    const float totalGain = chopGainLinear * v.midiVelocity * boundaryGain * v.fadeGain;

                    for (int ch = 0; ch < outputChannels; ++ch)
                    {
                        const auto srcCh    = juce::jmin (ch, channels - 1);
                        const auto* outRead = engine.discardOutput.getReadPointer (srcCh);
                        auto*       outData = buffer.getWritePointer (ch);
                        outData[segmentOffset + segmentWriteOffset + i] += outRead[i] * totalGain;
                    }
                    v.updateFade();
                }

                // Advance the conceptual source position.
                v.playbackSamplePosition += (double) chunk * bungeePosStep;
                segmentWriteOffset += chunk;
                handledByBungee = true;
            }

            if (! handledByBungee && ! handledByPrepared)
            {
                if (warpedBuf != nullptr)
                {
                    // ---- Pitch-neutral warp path -----------------------------------------
                    // Read from the pre-baked Bungee warp buffer at a constant rate.
                    // The warpedBuffer was rendered with pitchFactor=1.0 per segment so
                    // every warp shape change is time-only — pitch stays constant.
                    const int warpBufLength = warpedBuf->getNumSamples();
                    const int warpBufChans  = warpedBuf->getNumChannels();

                    for (int i = 0; i < chunk; ++i)
                    {
                        const auto boundaryGain = computeSliceBoundaryGain (v.playbackSamplePosition, cueStart,
                                                                             v.playbackStopSample, fadeSamples,
                                                                             sliceEndFadeSamples);
                        const float totalGain = chopGainLinear * v.midiVelocity * boundaryGain * v.fadeGain;

                        // Translate source-domain position to an offset into the warp buffer.
                        // The warp buffer starts at chopStart and covers chopEnd-chopStart frames.
                        const double warpBufPos = v.playbackSamplePosition
                                                  - (double) activeChop->startSample;
                        const int wbPos = juce::jlimit (0, juce::jmax (0, warpBufLength - 1),
                                                        (int) std::floor (warpBufPos));

                        for (int ch = 0; ch < outputChannels; ++ch)
                        {
                            const auto srcCh = juce::jmin (ch, warpBufChans - 1);
                            const auto* wbData = warpedBuf->getReadPointer (srcCh);
                            auto* outData = buffer.getWritePointer (ch);
                            outData[segmentOffset + segmentWriteOffset + i] +=
                                interpolateSampleLanczos (wbData, warpBufLength, warpBufPos, interpRadius)
                                * totalGain;
                            juce::ignoreUnused (wbPos);
                        }
                        v.playbackSamplePosition += warpedBufStep;
                        v.updateFade();
                    }

                    segmentWriteOffset += chunk;
                    v.bungeeResetPending = true; // re-prime Bungee if pitch/stretch re-engages
                }
                else
                {
                    // ---- Unity / warp-cache-miss fallback path ---------------------------
                    // Cache not ready yet: use Lanczos+warp mapping (pitch will shift
                    // until the Bungee cache entry arrives, which is a brief transition).
                    for (int i = 0; i < chunk; ++i)
                    {
                        const auto boundaryGain = computeSliceBoundaryGain (v.playbackSamplePosition, cueStart,
                                                                             v.playbackStopSample, fadeSamples,
                                                                             sliceEndFadeSamples);
                        const float totalGain = chopGainLinear * v.midiVelocity * boundaryGain * v.fadeGain;

                        for (int ch = 0; ch < outputChannels; ++ch)
                        {
                            const auto srcCh = juce::jmin (ch, sampleForBlock->buffer.getNumChannels() - 1);
                            const auto* srcData = sampleForBlock->buffer.getReadPointer (srcCh);
                            auto* outData       = buffer.getWritePointer (ch);
                            const auto mappedSourcePosition = mapChopTimelineToSourceSample (activeChop,
                                                                                              v.playbackSamplePosition,
                                                                                              sourceRate);
                            outData[segmentOffset + segmentWriteOffset + i] +=
                                interpolateSampleLanczos (srcData, sourceLength, mappedSourcePosition, interpRadius)
                                * totalGain;
                        }
                        v.playbackSamplePosition += interpolatedSourceStep;
                        v.updateFade();
                    }

                    segmentWriteOffset += chunk;
                    v.bungeeResetPending = true;
                }
            }

            if ((v.playbackStopSample > 0.0 && v.playbackSamplePosition >= v.playbackStopSample)
                || v.playbackSamplePosition >= (double) sourceLength)
            {
                if (v.playbackLoopStartSample >= 0.0
                    && v.playbackLoopStartSample < (double) sourceLength)
                {
                    // Transport loop: wrap to the loop start and keep rendering.
                    // Bungee needs to re-prime from the new source position.
                    v.playbackSamplePosition = v.playbackLoopStartSample;
                    v.bungeeResetPending = true;
                }
                else
                {
                    v.playbackActive = false;
                    v.playbackSamplePosition = v.playbackStopSample > 0.0
                                                    ? juce::jmin (v.playbackStopSample, (double) sourceLength)
                                                    : (double) sourceLength;
                }
            }
        }

        if (v.fadeGain <= 0.0f && v.fadeTarget == 0.0f) v.playbackActive = false;
    };

    auto midiIterator = midiMessages.begin();
    auto midiEnd = midiMessages.end();
    while (currentOffset < samplesToProcess)
    {
        int segmentSamples = samplesToProcess - currentOffset;
        while (midiIterator != midiEnd)
        {
            int midiTimestamp = (*midiIterator).samplePosition;
            if (midiTimestamp <= currentOffset)
            {
                handleMidiEvent ((*midiIterator).getMessage(), 
                                 midiTimestamp, 
                                 currentChopState, 
                                 currentHostRate, 
                                 blockHasHostPpq, 
                                 blockHostTransportPlaying, 
                                 blockStartHostPpq);
                ++midiIterator;
                continue;
            }
            segmentSamples = juce::jmin (segmentSamples, midiTimestamp - currentOffset);
            break;
        }

        renderVoiceSegment (voices[0], 0, currentOffset, segmentSamples);
        renderVoiceSegment (voices[1], 1, currentOffset, segmentSamples);
        currentOffset += segmentSamples;
    }

    auto& activeVoice = voices[activeVoiceIdx];
    playbackActive.store (activeVoice.playbackActive, std::memory_order_release);
    playbackSamplePosition.store (activeVoice.playbackSamplePosition, std::memory_order_release);

    bitCrusher.process (buffer);

    // Capture a rolling mono window of the crushed output for the UI scope
    // visualizer (before the compressor colours it). Cheap: a handful of ring
    // writes plus a 64-slot relaxed publish, only while the crusher is on.
    if (bitCrusher.isEnabled())
    {
        const int   n  = buffer.getNumSamples();
        const auto* L  = buffer.getReadPointer (0);
        const auto* R  = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            bitCrusherScopeRing[(size_t) bitCrusherScopeWritePos] = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
            bitCrusherScopeWritePos = (bitCrusherScopeWritePos + 1) % kBitCrusherScopeSize;
        }
        for (int i = 0; i < kBitCrusherScopeSize; ++i)
        {
            const int idx = (bitCrusherScopeWritePos + i) % kBitCrusherScopeSize; // oldest first
            bitCrusherScope[(size_t) i].store (bitCrusherScopeRing[(size_t) idx], std::memory_order_relaxed);
        }
    }

    compressor.process (buffer);

    float blockPeak = 0.0f;
    for (int ch = 0; ch < totalNumOutputChannels; ++ch)
    {
        const auto* channelData = buffer.getReadPointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            blockPeak = juce::jmax (blockPeak, std::abs (channelData[i]));
    }
    const auto currentMeter = outputMeterLevel.load (std::memory_order_acquire);
    const auto releasedMeter = juce::jmax (blockPeak, currentMeter - 0.045f);
    outputMeterLevel.store (juce::jlimit (0.0f, 1.0f, releasedMeter), std::memory_order_release);
}

void AudioPluginAudioProcessor::handleMidiEvent (const juce::MidiMessage& msg, 
                                                int sampleOffset, 
                                                const std::shared_ptr<ChopState>& currentChopState,
                                                double currentHostRate,
                                                bool blockHasHostPpq,
                                                bool blockHostTransportPlaying,
                                                double blockStartHostPpq)
{
    if (msg.isNoteOff())
    {
        if (msg.getNoteNumber() == heldMidiNote.load())
        {
            heldMidiNote.store (-1, std::memory_order_release);
            voices[activeVoiceIdx].startFade (0.0f, (double) midiVoiceReleaseSamples);
        }
        return;
    }

    if (! msg.isNoteOn()) return;

    const auto effectiveRootNote = midiRootNote + midiOctaveOffset.load (std::memory_order_acquire) * 12;
    const auto chopIdx = msg.getNoteNumber() - effectiveRootNote;
    if (currentChopState == nullptr || chopIdx < 0 || chopIdx >= (int) currentChopState->chops.size()) return;

    const auto& chop = currentChopState->chops[(size_t) chopIdx];
    
    // Sampling pads are monophonic/choked: a new chop stops the previous chop
    // immediately, so slices cannot overlap or sum into clipping.
    activeVoiceIdx = 0;
    voices[1].reset();
    
    auto& vs = voices[activeVoiceIdx];
    vs.reset();
    vs.playbackActive = true;
    vs.midiVelocity = msg.getVelocity() / 127.0f;
    
    const auto currentSample = std::atomic_load (&loadedSample);
    const double sourceRate = currentSample != nullptr ? juce::jmax (1.0, currentSample->sampleRate) : 44100.0;
    
    double startPos = (double) juce::jlimit (chop.startSample, chop.endSample, chop.startSample + chop.cueOffsetSamples);
    if (! chop.warpMarkers.empty() && sourceRate > 0.0)
    {
        cuesampler::WarpMap tempMap;
        tempMap.build (chop.startSample, chop.endSample, chop.warpMarkers, sourceRate);
        const double cueLocalSeconds = tempMap.localTimeAtSourceSample (startPos);
        startPos = chop.startSample + (cueLocalSeconds * sourceRate);
    }
    
    vs.playbackSamplePosition = startPos;
    vs.playbackStopSample = (double) chop.endSample;
    vs.playbackTriggeredByMidi = true;
    vs.fadeGain = 1.0f;
    vs.fadeTarget = 1.0f;
    vs.fadeStep = 0.0f;
    heldMidiNote.store (msg.getNoteNumber(), std::memory_order_release);
    lastTriggeredChopId.store (chop.id, std::memory_order_release);
    chopTriggerRevision.fetch_add (1, std::memory_order_acq_rel);

    if (syncToHost.load() && blockHasHostPpq && blockHostTransportPlaying && hostBpm.load() > 0.0)
    {
        const auto ppqPerSample = hostBpm.load() / (60.0 * currentHostRate);
        vs.playbackSyncStartPpq = blockStartHostPpq + (double) sampleOffset * ppqPerSample;
        vs.playbackSyncStartSample = vs.playbackSamplePosition;
        vs.playbackSyncAnchorValid = true;
    }
}

//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor (*this);
}

//==============================================================================
void AudioPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("CueSamplerState");
    state.setProperty ("version", 2, nullptr);
    state.setProperty ("gridBpmTrim", (double) gridBpmTrim.load (std::memory_order_acquire), nullptr);
    state.setProperty ("gridStartOffset", (double) gridStartOffset.load (std::memory_order_acquire), nullptr);
    state.setProperty ("waveformZoom", (double) waveformZoom.load (std::memory_order_acquire), nullptr);
    state.setProperty ("waveformScroll", (double) waveformScroll.load (std::memory_order_acquire), nullptr);
    state.setProperty ("globalPitchSemitones", (double) pitchSemitones.load (std::memory_order_acquire), nullptr);
    state.setProperty ("syncToHost", syncToHost.load (std::memory_order_acquire), nullptr);
    state.setProperty ("halfTimeEnabled", halfTimeEnabled.load (std::memory_order_acquire), nullptr);
    state.setProperty ("muteDrums", muteDrums.load (std::memory_order_acquire), nullptr);
    state.setProperty ("muteBass", muteBass.load (std::memory_order_acquire), nullptr);
    state.setProperty ("muteVocals", muteVocals.load (std::memory_order_acquire), nullptr);
    state.setProperty ("chopBarsCount", chopBarsCount.load (std::memory_order_acquire), nullptr);
    state.setProperty ("midiOctaveOffset", midiOctaveOffset.load (std::memory_order_acquire), nullptr);
    state.setProperty ("playbackSamplePosition", playbackSamplePosition.load (std::memory_order_acquire), nullptr);

    const auto currentSample = std::atomic_load (&loadedSample);
    state.setProperty ("hasLoadedSample", currentSample != nullptr, nullptr);

    if (currentSample != nullptr)
    {
        state.setProperty ("samplePath", currentSample->filePath, nullptr);
        state.setProperty ("sampleFileName", currentSample->fileName, nullptr);

        if (currentSample->serializedStateData.getSize() > 0)
            state.setProperty ("embeddedSampleData", currentSample->serializedStateData, nullptr);
    }

    if (const auto currentEditState = std::atomic_load (&tempoEditState); currentEditState != nullptr)
    {
        juce::ValueTree editStateTree ("TempoEditState");
        editStateTree.setProperty ("regionStartSample", currentEditState->regionStartSample, nullptr);
        editStateTree.setProperty ("regionEndSample", currentEditState->regionEndSample, nullptr);
        state.addChild (editStateTree, -1, nullptr);
    }

    if (const auto currentAnalysis = std::atomic_load (&tempoAnalysis); currentAnalysis != nullptr)
    {
        juce::ValueTree analysisTree ("TempoAnalysis");
        analysisTree.setProperty ("estimatedBpm", currentAnalysis->estimatedBpm, nullptr);
        analysisTree.setProperty ("confidence", currentAnalysis->confidence, nullptr);
        analysisTree.setProperty ("likelyDrifting", currentAnalysis->likelyDrifting, nullptr);
        analysisTree.setProperty ("beatPeriodSeconds", currentAnalysis->beatPeriodSeconds, nullptr);
        analysisTree.setProperty ("firstBeatSeconds", currentAnalysis->firstBeatSeconds, nullptr);
        analysisTree.setProperty ("downbeatPhase", currentAnalysis->downbeatPhase, nullptr);
        analysisTree.setProperty ("analysisStartSeconds", currentAnalysis->analysisStartSeconds, nullptr);
        analysisTree.setProperty ("analysisEndSeconds", currentAnalysis->analysisEndSeconds, nullptr);
        analysisTree.addChild (createSequenceTree ("BeatPositions", "Beat", "seconds",
                                                   currentAnalysis->beatPositionsSeconds),
                               -1, nullptr);
        analysisTree.addChild (createSequenceTree ("BarPositions", "Bar", "seconds",
                                                   currentAnalysis->barPositionsSeconds),
                               -1, nullptr);
        state.addChild (analysisTree, -1, nullptr);
    }

    if (const auto currentChopState = std::atomic_load (&chopState); currentChopState != nullptr)
    {
        juce::ValueTree chopStateTree ("ChopState");
        chopStateTree.setProperty ("selectedChopId", currentChopState->selectedChopId, nullptr);
        chopStateTree.setProperty ("nextChopId", currentChopState->nextChopId, nullptr);

        for (const auto& chop : currentChopState->chops)
        {
            juce::ValueTree chopTree ("Chop");
            chopTree.setProperty ("id", chop.id, nullptr);
            chopTree.setProperty ("startSample", chop.startSample, nullptr);
            chopTree.setProperty ("endSample", chop.endSample, nullptr);
            chopTree.setProperty ("cueOffsetSamples", chop.cueOffsetSamples, nullptr);
            chopTree.setProperty ("gainDecibels", chop.gainDecibels, nullptr);
            chopTree.setProperty ("pitchSemitones", chop.pitchSemitones, nullptr);
            chopTree.setProperty ("favorite", chop.favorite, nullptr);

            for (const auto& marker : chop.warpMarkers)
            {
                juce::ValueTree markerTree ("WarpMarker");
                markerTree.setProperty ("sourceSample", marker.sourceSample, nullptr);
                markerTree.setProperty ("localTimeSeconds", marker.localTimeSeconds, nullptr);
                markerTree.setProperty ("snappedToGrid", marker.snappedToGrid, nullptr);
                markerTree.setProperty ("gridFingerprint", marker.gridFingerprint, nullptr);
                chopTree.addChild (markerTree, -1, nullptr);
            }

            chopStateTree.addChild (chopTree, -1, nullptr);
        }

        state.addChild (chopStateTree, -1, nullptr);
    }

    juce::MemoryOutputStream output (destData, false);
    output.write (cueSamplerStateMagic, 4);
    state.writeToStream (output);
}

void AudioPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    juce::ValueTree state;

    if (const auto xml = getXmlFromBinary (data, sizeInBytes))
        state = juce::ValueTree::fromXml (*xml);

    if ((! state.isValid() || ! state.hasType ("CueSamplerState"))
        && sizeInBytes > 4
        && std::memcmp (data, cueSamplerStateMagic, 4) == 0)
    {
        state = juce::ValueTree::readFromData (static_cast<const char*> (data) + 4,
                                               (size_t) sizeInBytes - 4);
    }

    if (! state.isValid() || ! state.hasType ("CueSamplerState"))
    {
        // Compatibility with the brief untagged binary state format used in the previous build.
        state = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    }

    if (! state.isValid() || ! state.hasType ("CueSamplerState"))
        return;

    const auto restoreState = parseDeferredRestoreState (state);
    if (! restoreState.hasExplicitSampleState)
        return;

    const auto restoreStateGeneration = restoreGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;

    restoreThreadPool.removeAllJobs (false, 0);
    analysisThreadPool.removeAllJobs (false, 0);
    chopAudioCache.clearPrepared();
    tempoAnalysisGeneration.fetch_add (1, std::memory_order_acq_rel);
    tempoAnalysisInProgress.store (false, std::memory_order_release);

    applyParsedRestoreState (restoreState);
    touchTempoUiRevision();
    sampleChangeBroadcaster.sendChangeMessage();
    notifyEditStateChanged();

    if (restoreState.hasSavedSample && (restoreState.embeddedSampleData.getSize() > 0 || restoreState.samplePath.isNotEmpty()))
    {
        restoreThreadPool.addJob (new DeferredSampleRestoreJob (*this, restoreState, restoreStateGeneration), true);
        return;
    }
}

AudioPluginAudioProcessor::DeferredRestoreStateData
AudioPluginAudioProcessor::parseDeferredRestoreState (const juce::ValueTree& state) const
{
    DeferredRestoreStateData restoreState;
    restoreState.hasExplicitSampleState = state.hasProperty ("hasLoadedSample")
                                       || state.hasProperty ("samplePath")
                                       || state.hasProperty ("sampleFileName");
    restoreState.hasSavedSample = (bool) state.getProperty ("hasLoadedSample", false);
    restoreState.samplePath = state.getProperty ("samplePath").toString();
    restoreState.sampleFileName = state.getProperty ("sampleFileName").toString();
    if (const auto* embeddedSampleData = state.getProperty ("embeddedSampleData").getBinaryData())
    {
        if (embeddedSampleData->getSize() <= maximumEmbeddedSampleBytes)
            restoreState.embeddedSampleData = *embeddedSampleData;
    }

    restoreState.editState = std::make_shared<TempoEditState>();
    if (const auto editStateTree = state.getChildWithName ("TempoEditState"); editStateTree.isValid())
    {
        restoreState.editState->regionStartSample = (int) editStateTree.getProperty ("regionStartSample", -1);
        restoreState.editState->regionEndSample = (int) editStateTree.getProperty ("regionEndSample", -1);
    }

    if (const auto analysisTree = state.getChildWithName ("TempoAnalysis"); analysisTree.isValid())
    {
        auto analysis = std::make_shared<TempoAnalysisData>();
        analysis->estimatedBpm = (double) analysisTree.getProperty ("estimatedBpm", 0.0);
        analysis->confidence = (float) (double) analysisTree.getProperty ("confidence", 0.0);
        analysis->likelyDrifting = (bool) analysisTree.getProperty ("likelyDrifting", false);
        analysis->beatPeriodSeconds = (double) analysisTree.getProperty ("beatPeriodSeconds", 0.0);
        analysis->firstBeatSeconds = (double) analysisTree.getProperty ("firstBeatSeconds", 0.0);
        analysis->downbeatPhase = (int) analysisTree.getProperty ("downbeatPhase", 0);
        analysis->analysisStartSeconds = (double) analysisTree.getProperty ("analysisStartSeconds", 0.0);
        analysis->analysisEndSeconds = (double) analysisTree.getProperty ("analysisEndSeconds", 0.0);
        analysis->beatPositionsSeconds = parseDoubleSequenceTree (analysisTree.getChildWithName ("BeatPositions"), "seconds");
        analysis->barPositionsSeconds = parseDoubleSequenceTree (analysisTree.getChildWithName ("BarPositions"), "seconds");

        if (analysis->estimatedBpm > 0.0 && analysis->beatPeriodSeconds > 0.0)
            restoreState.analysis = std::move (analysis);
    }

    restoreState.chopState = std::make_shared<ChopState>();
    if (const auto chopStateTree = state.getChildWithName ("ChopState"); chopStateTree.isValid())
    {
        restoreState.chopState->selectedChopId = (int) chopStateTree.getProperty ("selectedChopId", -1);
        restoreState.chopState->nextChopId = (int) chopStateTree.getProperty ("nextChopId", 1);

        int highestChopId = 0;

        for (const auto chopTree : chopStateTree)
        {
            if ((int) restoreState.chopState->chops.size() >= maximumRestoredChops)
                break;

            if (! chopTree.hasType ("Chop"))
                continue;

            ChopDefinition chop;
            chop.id = (int) chopTree.getProperty ("id", 0);
            chop.startSample = (int) chopTree.getProperty ("startSample", 0);
            chop.endSample = (int) chopTree.getProperty ("endSample", 0);
            chop.cueOffsetSamples = (int) chopTree.getProperty ("cueOffsetSamples", 0);
            chop.gainDecibels = juce::jlimit (-24.0f, 12.0f,
                                              (float) (double) chopTree.getProperty ("gainDecibels", 0.0));
            chop.pitchSemitones = juce::jlimit (-12.0f, 12.0f,
                                                (float) (double) chopTree.getProperty ("pitchSemitones", 0.0));
            chop.favorite = (bool) chopTree.getProperty ("favorite", false);

            for (const auto markerTree : chopTree)
            {
                if ((int) chop.warpMarkers.size() >= maximumRestoredWarpMarkersPerChop)
                    break;

                if (! markerTree.hasType ("WarpMarker"))
                    continue;

                ChopWarpMarker marker;
                marker.sourceSample = (int) markerTree.getProperty ("sourceSample", 0);
                marker.localTimeSeconds = (double) markerTree.getProperty ("localTimeSeconds", 0.0);
                marker.snappedToGrid = (bool) markerTree.getProperty ("snappedToGrid", false);
                marker.gridFingerprint = (double) markerTree.getProperty ("gridFingerprint", 0.0);
                if (std::isfinite (marker.localTimeSeconds)
                    && std::isfinite (marker.gridFingerprint)
                    && marker.localTimeSeconds >= 0.0)
                {
                    chop.warpMarkers.push_back (marker);
                }
            }

            std::sort (chop.warpMarkers.begin(), chop.warpMarkers.end(),
                       [] (const ChopWarpMarker& a, const ChopWarpMarker& b)
                       {
                           return a.sourceSample < b.sourceSample;
                       });

            if (chop.id > 0 && chop.endSample > chop.startSample)
            {
                highestChopId = juce::jmax (highestChopId, chop.id);
                restoreState.chopState->chops.push_back (chop);
            }
        }

        std::sort (restoreState.chopState->chops.begin(), restoreState.chopState->chops.end(),
                   [] (const ChopDefinition& lhs, const ChopDefinition& rhs)
                   {
                       if (lhs.startSample != rhs.startSample)
                           return lhs.startSample < rhs.startSample;

                       return lhs.id < rhs.id;
                   });

        if (restoreState.chopState->nextChopId <= highestChopId)
            restoreState.chopState->nextChopId = highestChopId + 1;
    }

    restoreState.restoredGridBpmTrim = juce::jlimit (-10.0f, 10.0f,
                                                     (float) (double) state.getProperty ("gridBpmTrim", 0.0));
    restoreState.restoredGridStartOffset = juce::jlimit (-1.0f, 1.0f,
                                                         (float) (double) state.getProperty ("gridStartOffset", 0.0));
    restoreState.restoredWaveformZoom = juce::jlimit (0.0f, 1.0f,
                                                      (float) (double) state.getProperty ("waveformZoom", 0.25));
    restoreState.restoredWaveformScroll = juce::jlimit (0.0f, 1.0f,
                                                        (float) (double) state.getProperty ("waveformScroll", 0.0));
    restoreState.restoredGlobalPitch = juce::jlimit (-24.0f, 24.0f,
                                                     (float) (double) state.getProperty ("globalPitchSemitones", 0.0));
    restoreState.restoredSyncToHost = (bool) state.getProperty ("syncToHost", false);
    restoreState.restoredHalfTime = (bool) state.getProperty ("halfTimeEnabled", false);
    restoreState.restoredMuteDrums = (bool) state.getProperty ("muteDrums", false);
    restoreState.restoredMuteBass = (bool) state.getProperty ("muteBass", false);
    restoreState.restoredMuteVocals = (bool) state.getProperty ("muteVocals", false);
    restoreState.restoredBarsPerChop = juce::jlimit (1, 8, (int) state.getProperty ("chopBarsCount", 1));
    restoreState.restoredMidiOctaveOffset = juce::jlimit (midiOctaveOffsetMin, midiOctaveOffsetMax,
                                                          (int) state.getProperty ("midiOctaveOffset", 0));
    restoreState.restoredPlaybackPosition = (double) state.getProperty ("playbackSamplePosition", 0.0);
    if (! std::isfinite (restoreState.restoredPlaybackPosition) || restoreState.restoredPlaybackPosition < 0.0)
        restoreState.restoredPlaybackPosition = 0.0;
    return restoreState;
}

void AudioPluginAudioProcessor::applyParsedRestoreState (const DeferredRestoreStateData& restoreState)
{
    std::atomic_store (&loadedSample, std::shared_ptr<LoadedSampleData> {});
    std::atomic_store (&tempoEditState, restoreState.editState != nullptr
                                            ? std::make_shared<TempoEditState> (*restoreState.editState)
                                            : std::make_shared<TempoEditState> ());
    std::atomic_store (&tempoAnalysis, restoreState.analysis != nullptr
                                           ? std::make_shared<TempoAnalysisData> (*restoreState.analysis)
                                           : std::shared_ptr<TempoAnalysisData> {});
    std::atomic_store (&chopState, std::make_shared<ChopState> ());

    loadedFileName = restoreState.sampleFileName.isNotEmpty()
        ? restoreState.sampleFileName
        : juce::File (restoreState.samplePath).getFileNameWithoutExtension();
    sampleSampleRate = 0.0;

    gridBpmTrim.store (restoreState.restoredGridBpmTrim, std::memory_order_release);
    gridStartOffset.store (restoreState.restoredGridStartOffset, std::memory_order_release);
    waveformZoom.store (restoreState.restoredWaveformZoom, std::memory_order_release);
    waveformScroll.store (restoreState.restoredWaveformScroll, std::memory_order_release);
    pitchSemitones.store (restoreState.restoredGlobalPitch, std::memory_order_release);
    syncToHost.store (restoreState.restoredSyncToHost, std::memory_order_release);
    halfTimeEnabled.store (restoreState.restoredHalfTime, std::memory_order_release);

    // Restore the mute flags and clear stem state — the new sample re-separates
    // (stem audio is never serialized), then these mutes apply on publish.
    muteDrums.store (restoreState.restoredMuteDrums, std::memory_order_release);
    muteBass.store (restoreState.restoredMuteBass, std::memory_order_release);
    muteVocals.store (restoreState.restoredMuteVocals, std::memory_order_release);
    std::atomic_store (&stemSet, std::shared_ptr<const StemSet> {});
    stemsReady.store (false, std::memory_order_release);
    stemSeparationInProgress.store (false, std::memory_order_release);
    stemProgress.store (0.0f, std::memory_order_release);
    appliedStemMask.store (-1, std::memory_order_release);

    chopBarsCount.store ((restoreState.restoredBarsPerChop <= 1) ? 1
                                                                 : (restoreState.restoredBarsPerChop <= 2) ? 2
                                                                 : (restoreState.restoredBarsPerChop <= 4) ? 4
                                                                                                            : 8,
                         std::memory_order_release);
    midiOctaveOffset.store (restoreState.restoredMidiOctaveOffset, std::memory_order_release);

    playbackSamplePosition.store (restoreState.restoredPlaybackPosition, std::memory_order_release);
    playbackActive.store (false, std::memory_order_release);
    pendingTransportCommand.store ((int) TransportCommand::silence, std::memory_order_release);

    if (restoreState.restoredSyncToHost && restoreState.analysis != nullptr)
    {
        const auto currentHostBpm = hostBpm.load (std::memory_order_acquire);
        updateHostSyncStretchRatio (*restoreState.analysis, currentHostBpm);
    }
    else
    {
        timeStretchRatio.store (1.0f, std::memory_order_release);
    }
}

void AudioPluginAudioProcessor::completeDeferredSampleRestore (const DeferredRestoreStateData& restoreState,
                                                               std::shared_ptr<LoadedSampleData> restoredSample,
                                                               uint64_t completedRestoreGeneration)
{
    if (completedRestoreGeneration != restoreGeneration.load (std::memory_order_acquire))
        return;

    if (restoredSample == nullptr)
    {
        touchTempoUiRevision();
        sampleChangeBroadcaster.sendChangeMessage();
        notifyEditStateChanged();
        return;
    }

    auto restoredEditState = restoreState.editState != nullptr
        ? std::make_shared<TempoEditState> (*restoreState.editState)
        : std::make_shared<TempoEditState> ();
    auto restoredAnalysis = restoreState.analysis != nullptr
        ? std::make_shared<TempoAnalysisData> (*restoreState.analysis)
        : std::shared_ptr<TempoAnalysisData> {};
    auto restoredChopState = restoreState.chopState != nullptr
        ? std::make_shared<ChopState> (*restoreState.chopState)
        : std::make_shared<ChopState> ();

    if (restoredSample->filePath.isEmpty() && restoreState.samplePath.isNotEmpty())
        restoredSample->filePath = restoreState.samplePath;
    if (restoredSample->sourceFile.getFullPathName().isEmpty() && restoredSample->filePath.isNotEmpty())
        restoredSample->sourceFile = juce::File (restoredSample->filePath);

    const auto totalSamples = restoredSample->buffer.getNumSamples();
    if (restoredEditState->regionStartSample < 0 || restoredEditState->regionEndSample <= restoredEditState->regionStartSample)
    {
        restoredEditState->regionStartSample = -1;
        restoredEditState->regionEndSample = -1;
    }
    else
    {
        restoredEditState->regionStartSample = juce::jlimit (0, totalSamples - 1, restoredEditState->regionStartSample);
        restoredEditState->regionEndSample = juce::jlimit (restoredEditState->regionStartSample + 1,
                                                           totalSamples,
                                                           restoredEditState->regionEndSample);
    }

    int highestChopId = 0;
    for (auto& chop : restoredChopState->chops)
    {
        chop.startSample = juce::jlimit (0, totalSamples - 1, chop.startSample);
        chop.endSample = juce::jlimit (chop.startSample + 1, totalSamples, chop.endSample);
        const auto maxCueOffset = juce::jmax (0, chop.endSample - chop.startSample - 1);
        chop.cueOffsetSamples = juce::jlimit (0, maxCueOffset, chop.cueOffsetSamples);
        if (chop.cueOffsetSamples == 0)
        {
            const auto autoCueStart = findAutoCueStartSample (*restoredSample, chop.startSample, chop.endSample);
            chop.cueOffsetSamples = juce::jlimit (0, maxCueOffset, autoCueStart - chop.startSample);
        }

        // Drop warp markers that fall outside the (possibly clamped) chop range.
        chop.warpMarkers.erase (std::remove_if (chop.warpMarkers.begin(),
                                                chop.warpMarkers.end(),
                                                [&chop] (const ChopWarpMarker& m)
                                                {
                                                    return m.sourceSample <= chop.startSample
                                                        || m.sourceSample >= chop.endSample;
                                                }),
                                chop.warpMarkers.end());

        highestChopId = juce::jmax (highestChopId, chop.id);
    }

    restoredChopState->chops.erase (std::remove_if (restoredChopState->chops.begin(),
                                                    restoredChopState->chops.end(),
                                                    [] (const ChopDefinition& chop)
                                                    {
                                                        return chop.id <= 0 || chop.endSample <= chop.startSample;
                                                    }),
                                    restoredChopState->chops.end());

    std::sort (restoredChopState->chops.begin(), restoredChopState->chops.end(),
               [] (const ChopDefinition& lhs, const ChopDefinition& rhs)
               {
                   if (lhs.startSample != rhs.startSample)
                       return lhs.startSample < rhs.startSample;

                   return lhs.id < rhs.id;
               });

    if (restoredChopState->nextChopId <= highestChopId)
        restoredChopState->nextChopId = highestChopId + 1;

    const auto hasSelectedChop = std::any_of (restoredChopState->chops.begin(),
                                              restoredChopState->chops.end(),
                                              [selectedChopId = restoredChopState->selectedChopId] (const auto& chop)
                                              {
                                                  return chop.id == selectedChopId;
                                              });
    if (! hasSelectedChop)
        restoredChopState->selectedChopId = restoredChopState->chops.empty() ? -1
                                                                              : restoredChopState->chops.front().id;

    if (completedRestoreGeneration != restoreGeneration.load (std::memory_order_acquire))
        return;

    // Restored sample may have a different sample rate from any prior sample
    // — rebuild the per-voice Bungee engines before the audio thread sees it.
    ensurePitchEnginesReady (restoredSample->sampleRate,
                              hostSampleRate.load (std::memory_order_acquire),
                              juce::jmax (1, getTotalNumOutputChannels()));

    pendingTransportCommand.store ((int) TransportCommand::silence, std::memory_order_release);
    std::atomic_store (&loadedSample, restoredSample);
    std::atomic_store (&tempoEditState, restoredEditState);
    std::atomic_store (&tempoAnalysis, restoredAnalysis);
    std::atomic_store (&chopState, restoredChopState);

    loadedFileName = restoredSample->fileName;
    sampleSampleRate = restoredSample->sampleRate;

    const auto maxPlaybackPosition = juce::jmax (0.0, (double) restoredSample->buffer.getNumSamples() - 1.0);
    const auto fallbackStartSample = (double) juce::jlimit (0,
                                                            juce::jmax (0, totalSamples - 1),
                                                            restoredSample->leadingContentStartSample);
    const auto restoredPlaybackPosition = juce::jlimit (0.0, maxPlaybackPosition, restoreState.restoredPlaybackPosition);
    playbackSamplePosition.store (restoredPlaybackPosition > 0.0 ? restoredPlaybackPosition : fallbackStartSample,
                                  std::memory_order_release);
    playbackActive.store (false, std::memory_order_release);

    if (restoreState.restoredSyncToHost && restoredAnalysis != nullptr)
    {
        const auto currentHostBpm = hostBpm.load (std::memory_order_acquire);
        updateHostSyncStretchRatio (*restoredAnalysis, currentHostBpm);
    }
    else
    {
        timeStretchRatio.store (1.0f, std::memory_order_release);
    }

    if (restoredAnalysis != nullptr && restoredChopState->chops.empty())
        buildChopsFromAnalysis (*restoredAnalysis);

    if (restoredAnalysis == nullptr)
        launchTempoAnalysis (restoredSample);
    else
        touchTempoUiRevision();

    // Re-separate on restore (stem audio is never serialized). The mute flags were
    // already restored in applyParsedRestoreState; they apply once stems publish.
    launchStemSeparation (restoredSample);

    // Restored chops may carry warp markers — re-bake those entries.
    if (const auto restoredState = std::atomic_load (&chopState); restoredState != nullptr)
    {
        for (const auto& c : restoredState->chops)
        {
            if (! c.warpMarkers.empty())
                requestChopWarpRender (c.id);
        }
    }
    clearEditUndoHistory(); // restored session state is the new baseline
    sampleChangeBroadcaster.sendChangeMessage();
    notifyEditStateChanged();
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}

std::shared_ptr<AudioPluginAudioProcessor::LoadedSampleData>
AudioPluginAudioProcessor::createLoadedSampleDataFromFile (const juce::File& file)
{
    juce::AudioFormatManager localFormatManager;
    localFormatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader> (localFormatManager.createReaderFor (file));
    if (reader == nullptr)
        return {};

    auto sampleData = readValidatedSampleData (*reader);
    if (sampleData == nullptr)
        return {};

    sampleData->sourceFile = file;
    sampleData->filePath = file.getFullPathName();
    sampleData->fileName = file.getFileNameWithoutExtension();
    sampleData->leadingContentStartSample = findLeadingContentStartSample (*sampleData, 0,
                                                                           sampleData->buffer.getNumSamples());
    serializeSampleToStateData (*sampleData, sampleData->serializedStateData);
    return sampleData;
}

// Reads musical-key metadata from common audio tags and normalizes it into a key result.
KeyDetector::Result AudioPluginAudioProcessor::tryReadKeyFromMetadata (const juce::File& file)
{
    KeyDetector::Result result;

    juce::AudioFormatManager localFormatManager;
    localFormatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader> (localFormatManager.createReaderFor (file));
    if (reader == nullptr)
        return result;

    static constexpr std::array<const char*, 6> keyTagNames
    {
        "INITIALKEY", "initialkey", "Initial Key", "TKEY", "KEY", "key"
    };

    for (const auto* tagName : keyTagNames)
    {
        const auto tagValue = reader->metadataValues.getValue (tagName, juce::String()).trim();
        if (tagValue.isEmpty())
            continue;

        result = parseMetadataKeyString (tagValue);
        if (result.valid)
            return result;
    }

    return {};
}

bool AudioPluginAudioProcessor::serializeSampleToStateData (const LoadedSampleData& sampleData,
                                                            juce::MemoryBlock& stateData) const
{
    stateData.reset();

    const auto numChannels = sampleData.buffer.getNumChannels();
    const auto numSamples = sampleData.buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0 || sampleData.sampleRate <= 0.0)
        return false;

    auto serializeWithFormat = [&] (juce::AudioFormat& format, int bitsPerSample) -> bool
    {
        juce::MemoryBlock encodedAudio;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (encodedAudio, false);
        const auto options = juce::AudioFormatWriterOptions()
                                 .withSampleRate (sampleData.sampleRate)
                                 .withNumChannels (numChannels)
                                 .withBitsPerSample (bitsPerSample);

        auto writer = format.createWriterFor (stream, options);
        if (writer == nullptr)
            return false;

        if (! writer->writeFromAudioSampleBuffer (sampleData.buffer, 0, numSamples))
            return false;

        writer.reset();

        if (encodedAudio.getSize() == 0)
            return false;

        stateData = std::move (encodedAudio);
        return true;
    };

    juce::FlacAudioFormat flacFormat;
    if (serializeWithFormat (flacFormat, 16))
        return true;

    juce::WavAudioFormat wavFormat;
    return serializeWithFormat (wavFormat, 16);
}

std::shared_ptr<AudioPluginAudioProcessor::LoadedSampleData>
AudioPluginAudioProcessor::createLoadedSampleDataFromStateData (const juce::MemoryBlock& stateData,
                                                                const juce::String& fileName)
{
    if (stateData.getSize() == 0 || stateData.getSize() > maximumEmbeddedSampleBytes)
        return {};

    juce::AudioFormatManager localFormatManager;
    localFormatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader> (
        localFormatManager.createReaderFor (std::make_unique<juce::MemoryInputStream> (stateData, false)));
    if (reader == nullptr)
        return {};

    auto sampleData = readValidatedSampleData (*reader);
    if (sampleData == nullptr)
        return {};

    sampleData->fileName = fileName.isNotEmpty() ? fileName : "Embedded Sample";
    sampleData->leadingContentStartSample = findLeadingContentStartSample (*sampleData, 0,
                                                                           sampleData->buffer.getNumSamples());
    sampleData->serializedStateData = stateData;
    return sampleData;
}

void AudioPluginAudioProcessor::loadAudioFile (const juce::File& file)
{
    restoreGeneration.fetch_add (1, std::memory_order_acq_rel);
    restoreThreadPool.removeAllJobs (false, 0);

    // New sample → drop the entire warp cache and abandon any pending bakes.
    warpRenderThreadPool.removeAllJobs (false, 0);
    chopAudioCache.clear();
    warpRenderGeneration.fetch_add (1, std::memory_order_acq_rel);

    // Flush any pending telemetry for the previous sample and reset context;
    // the new sample's fingerprint is set later by the key-detection job.
    editTelemetry.clearSampleContext();

    if (auto sampleData = createLoadedSampleDataFromFile (file); sampleData != nullptr)
    {
        // Sample rate may have changed, so publish matching engines before
        // exposing the new sample to the audio thread.
        ensurePitchEnginesReady (sampleData->sampleRate,
                                  hostSampleRate.load (std::memory_order_acquire),
                                  juce::jmax (1, getTotalNumOutputChannels()));

        pendingTransportCommand.store ((int) TransportCommand::silence, std::memory_order_release);
        std::atomic_store (&loadedSample, sampleData);
        std::atomic_store (&tempoAnalysis, std::shared_ptr<TempoAnalysisData> {});
        std::atomic_store (&tempoEditState, std::make_shared<TempoEditState> ());
        std::atomic_store (&chopState, std::make_shared<ChopState> ());
        clearEditUndoHistory(); // a fresh sample starts with no edit history
        loadedFileName = sampleData->fileName;
        sampleSampleRate = sampleData->sampleRate;
        waveformZoom.store (0.20f, std::memory_order_release);
        waveformScroll.store (0.0f, std::memory_order_release);
        playbackActive.store (false, std::memory_order_release);
        playbackSamplePosition.store ((double) sampleData->leadingContentStartSample, std::memory_order_release);

        launchTempoAnalysis (sampleData);

        // A fresh sample starts unmuted; launchStemSeparation clears stem state and
        // kicks the background pass (mutes apply automatically once stems publish).
        muteDrums.store (false, std::memory_order_release);
        muteBass.store (false, std::memory_order_release);
        muteVocals.store (false, std::memory_order_release);
        launchStemSeparation (sampleData);

        const auto keyGeneration = keyDetectionGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
        keyDetectionThreadPool.removeAllJobs (false, 0);
        keyDetectionInProgress.store (true, std::memory_order_release);

        auto keyBuffer = sampleData->buffer;
        const auto keySampleRate = sampleData->sampleRate;
        const auto sourceFile = sampleData->sourceFile;

        keyDetectionThreadPool.addJob (new KeyDetectionJob (*this,
                                                            keyGeneration,
                                                            sourceFile,
                                                            std::move (keyBuffer),
                                                            keySampleRate),
                                       true);

        touchTempoUiRevision();
        sampleChangeBroadcaster.sendChangeMessage();
        requestHostStateSync();
    }
}

std::shared_ptr<const AudioPluginAudioProcessor::LoadedSampleData> AudioPluginAudioProcessor::getLoadedSample() const
{
    return std::atomic_load (&loadedSample);
}

std::shared_ptr<const AudioPluginAudioProcessor::TempoAnalysisData> AudioPluginAudioProcessor::getTempoAnalysis() const
{
    return std::atomic_load (&tempoAnalysis);
}

std::shared_ptr<const AudioPluginAudioProcessor::TempoEditState> AudioPluginAudioProcessor::getTempoEditState() const
{
    return std::atomic_load (&tempoEditState);
}

std::shared_ptr<const AudioPluginAudioProcessor::ChopState> AudioPluginAudioProcessor::getChopState() const
{
    return std::atomic_load (&chopState);
}

juce::File AudioPluginAudioProcessor::renderChopToTempWav (int chopId, bool applySync)
{
    const auto sampleData   = getLoadedSample();
    const auto chopSnapshot = getChopState();

    if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
        return {};
    if (chopSnapshot == nullptr)
        return {};

    const ChopDefinition* chop = nullptr;
    for (const auto& c : chopSnapshot->chops)
    {
        if (c.id == chopId)
        {
            chop = &c;
            break;
        }
    }
    if (chop == nullptr)
        return {};

    const auto sourceRate      = juce::jmax (1.0, sampleData->sampleRate);
    const auto currentHostRate = juce::jmax (1.0, hostSampleRate.load (std::memory_order_acquire));
    const auto numChannels     = sampleData->buffer.getNumChannels();

    // Step 12: warped chops export through the warp cache so the WAV reflects
    // the baked warp. If the chop has markers, we synchronously render a fresh
    // cache entry so the export uses the latest marker state (the async cache
    // might be stale if marker edits just happened). For unwarped chops we
    // continue reading from the source buffer with no allocation.
    std::shared_ptr<cuesampler::ChopAudioCache::Entry> exportRenderHold;
    const juce::AudioBuffer<float>* effectiveBuffer = &sampleData->buffer;
    int effectiveSourceLength = sampleData->buffer.getNumSamples();

    if (! chop->warpMarkers.empty())
    {
        auto baked = cuesampler::ChopAudioCache::renderChopSync (
            sampleData->buffer,
            sourceRate,
            chopId,
            chop->startSample,
            chop->endSample,
            chop->warpMarkers,
            warpRenderGeneration.fetch_add (1, std::memory_order_acq_rel) + 1);

        if (baked != nullptr
            && baked->warpedBuffer != nullptr
            && baked->warpedBuffer->getNumSamples() > 0)
        {
            exportRenderHold = std::move (baked);
            effectiveBuffer = exportRenderHold->warpedBuffer.get();
            effectiveSourceLength = effectiveBuffer->getNumSamples();
            // Keep the cache hot for the audio thread too.
            chopAudioCache.store (exportRenderHold);
        }
    }

    const auto sourceLength    = effectiveSourceLength;

    const float chopGainLinear = juce::Decibels::decibelsToGain (chop->gainDecibels);

    const float globalSemitones  = pitchSemitones.load (std::memory_order_acquire);
    const float effectiveSemitones = juce::jlimit (-24.0f, 24.0f,
                                                    globalSemitones + chop->pitchSemitones);
    const double pitchFactor = std::pow (2.0, (double) effectiveSemitones / 12.0);

    // Warped chops first bake their marker-defined local timing into
    // effectiveBuffer, then run through the same global stretch controls as
    // live playback so exported files still match the DAW tempo.
    const bool isWarped = (exportRenderHold != nullptr);

    if (applySync)
    {
        const auto analysis = std::atomic_load (&tempoAnalysis);
        if (analysis != nullptr)
            updateHostSyncStretchRatio (*analysis, hostBpm.load (std::memory_order_acquire));
    }

    auto stretchRatio = applySync
        ? juce::jlimit (0.25f, 4.0f, timeStretchRatio.load (std::memory_order_acquire))
        : 1.0f;

    // For warped chops the effective buffer IS the chop's slot in clip-local
    // time, so we must translate the cue point from source-time to local-time.
    int cueStart = 0;
    int chopEnd  = 0;
    if (isWarped)
    {
        cuesampler::WarpMap tempMap;
        tempMap.build (chop->startSample, chop->endSample, chop->warpMarkers, sourceRate);
        const double cueSource = chop->startSample + chop->cueOffsetSamples;
        const double cueLocalSeconds = tempMap.localTimeAtSourceSample (cueSource);
        const int mappedCue = (int) std::round (cueLocalSeconds * sourceRate);

        cueStart = juce::jlimit (0,
                                  juce::jmax (0, effectiveSourceLength - 1),
                                  mappedCue);
        chopEnd = effectiveSourceLength;
    }
    else
    {
        cueStart = juce::jlimit (chop->startSample,
                                  juce::jmax (chop->startSample, chop->endSample - 1),
                                  chop->startSample + chop->cueOffsetSamples);
        chopEnd = juce::jmin (chop->endSample, effectiveSourceLength);
    }

    const int inputFrames = juce::jmax (0, chopEnd - cueStart);
    if (inputFrames <= 0)
        return {};

    const bool halfTimeActive = halfTimeEnabled.load (std::memory_order_acquire);
    if (halfTimeActive)
        stretchRatio = juce::jlimit (0.25f, 4.0f, stretchRatio * 2.0f);

    const auto sourceFramesPerOutputFrame = (sourceRate / currentHostRate) / (double) stretchRatio;
    const auto outputFramesPerInputFrame = 1.0 / sourceFramesPerOutputFrame;
    const bool sourceRateMatchesHost = std::abs (sourceRate - currentHostRate) < 0.5;
    const bool stretchIsUnity = std::abs (stretchRatio - 1.0f) < 0.005f;
    const bool pitchIsUnity = std::abs (effectiveSemitones) < 0.01f;
    const bool bypassBungee = pitchIsUnity && stretchIsUnity;
    const int finalOutputFrames = juce::jmax (1, (int) std::round ((double) inputFrames * outputFramesPerInputFrame));

    constexpr int kBlock = 512;
    juce::AudioBuffer<float> outputBuffer (numChannels, finalOutputFrames + kBlock * 4);
    outputBuffer.clear();

    int framesToWrite = 0;

    if (bypassBungee)
    {
        if (! sourceRateMatchesHost)
        {
            // Source/host sample-rate conversion at unity pitch.
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* src = effectiveBuffer->getReadPointer (ch);
                auto* dst = outputBuffer.getWritePointer (ch);
                double pos = (double) cueStart;
                for (int i = 0; i < finalOutputFrames; ++i)
                {
                    dst[i] = interpolateSampleLanczos (src, sourceLength, pos) * chopGainLinear;
                    pos += sourceFramesPerOutputFrame;
                }
            }
            framesToWrite = finalOutputFrames;
        }
        else
        {
            // 1:1 straight copy
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* src = effectiveBuffer->getReadPointer (ch);
                auto* dst = outputBuffer.getWritePointer (ch);
                for (int i = 0; i < inputFrames; ++i)
                    dst[i] = src[cueStart + i] * chopGainLinear;
            }
            framesToWrite = inputFrames;
        }
    }
    else
    {
        Bungee::SampleRates rates { (int) std::round (sourceRate), (int) std::round (currentHostRate) };

        auto offlineStretcher = std::make_unique<Bungee::Stretcher<Bungee::Basic>> (rates, numChannels, -1);
        const int maxOfflineInputFrames = juce::jmax (kBlock, offlineStretcher->maxInputFrameCount());
        auto offlineStream    = std::make_unique<Bungee::Stream<Bungee::Basic>> (*offlineStretcher,
                                                                                 maxOfflineInputFrames,
                                                                                 numChannels);

        std::vector<const float*> inPtrs  ((size_t) numChannels);
        std::vector<float*>       outPtrs ((size_t) numChannels);
        juce::AudioBuffer<float> chunkBuffer (numChannels, kBlock + 16);
        juce::AudioBuffer<float> zeroBuf (numChannels, maxOfflineInputFrames);
        zeroBuf.clear();

        const int maxPrerollFrames = offlineStretcher->maxInputFrameCount();
        const int prerollFrames = juce::jmin (cueStart, maxPrerollFrames);
        const int syntheticPrerollFrames = isWarped ? juce::jmax (0, maxPrerollFrames - prerollFrames) : 0;
        if (prerollFrames > 0 || syntheticPrerollFrames > 0)
        {
            const int maxPrerollOutputFrames =
                juce::jmax (kBlock, (int) std::ceil ((double) kBlock * outputFramesPerInputFrame) + 16);
            juce::AudioBuffer<float> discardBuffer (numChannels, maxPrerollOutputFrames);

            int syntheticRendered = 0;
            while (syntheticRendered < syntheticPrerollFrames)
            {
                const int syntheticInputFrames = juce::jmin (kBlock, syntheticPrerollFrames - syntheticRendered);
                const double syntheticOutputFrames = juce::jlimit (
                    1.0,
                    (double) maxPrerollOutputFrames,
                    juce::jmax (1.0, std::ceil ((double) syntheticInputFrames * outputFramesPerInputFrame)));
                discardBuffer.clear();

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* tempInput = zeroBuf.getWritePointer (ch);
                    const auto* sourceData = effectiveBuffer->getReadPointer (ch);

                    for (int i = 0; i < syntheticInputFrames; ++i)
                    {
                        const int mirrorOffset = syntheticPrerollFrames - syntheticRendered - i;
                        const int sourceIndex = juce::jlimit (0,
                                                              juce::jmax (0, sourceLength - 1),
                                                              cueStart + mirrorOffset);
                        tempInput[i] = sourceData[sourceIndex];
                    }

                    inPtrs[(size_t) ch] = tempInput;
                    outPtrs[(size_t) ch] = discardBuffer.getWritePointer (ch);
                }

                offlineStream->process (inPtrs.data(), outPtrs.data(),
                                        syntheticInputFrames, syntheticOutputFrames, pitchFactor);
                syntheticRendered += syntheticInputFrames;
            }

            int prerollRead = cueStart - prerollFrames;

            while (prerollRead < cueStart)
            {
                const int prerollInputFrames = juce::jmin (kBlock, cueStart - prerollRead);
                const double prerollOutputFrames = juce::jlimit (
                    1.0,
                    (double) maxPrerollOutputFrames,
                    juce::jmax (1.0, std::ceil ((double) prerollInputFrames * outputFramesPerInputFrame)));
                discardBuffer.clear();

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    inPtrs[(size_t) ch] = effectiveBuffer->getReadPointer (ch, prerollRead);
                    outPtrs[(size_t) ch] = discardBuffer.getWritePointer (ch);
                }

                offlineStream->process (inPtrs.data(), outPtrs.data(),
                                        prerollInputFrames, prerollOutputFrames, pitchFactor);
                prerollRead += prerollInputFrames;
            }
        }

        double sourcePosition = (double) cueStart;
        const double stopSample = (double) chopEnd;
        int outputWriteOffset = 0;
        int zeroRenderStreak = 0;

        while (outputWriteOffset < finalOutputFrames)
        {
            const int remainingOutput = finalOutputFrames - outputWriteOffset;
            int segmentOutputFrames = juce::jmin (kBlock, remainingOutput);
            int inputFramesRequested = 0;

            if (sourcePosition < stopSample && sourcePosition < (double) sourceLength)
            {
                const auto inputFramesUntilStop = stopSample - sourcePosition;
                const auto outputFramesUntilStop = (int) std::floor (inputFramesUntilStop / sourceFramesPerOutputFrame);
                segmentOutputFrames = juce::jmin (juce::jmax (1, outputFramesUntilStop), segmentOutputFrames);

                inputFramesRequested = juce::jlimit (1, maxOfflineInputFrames,
                                                     (int) std::ceil ((double) segmentOutputFrames * sourceFramesPerOutputFrame));

                if (sourcePosition + inputFramesRequested > stopSample)
                {
                    inputFramesRequested = juce::jmax (1, (int) std::ceil (stopSample - sourcePosition));
                }
            }

            chunkBuffer.clear();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* tempInput = zeroBuf.getWritePointer (ch);
                std::fill (tempInput, tempInput + juce::jmax (1, inputFramesRequested), 0.0f);
                
                if (inputFramesRequested > 0)
                {
                    const auto sourceStart = juce::jlimit (0, juce::jmax (0, sourceLength - 1), (int) std::floor (sourcePosition));
                    const auto availableFrames = juce::jmax (0, sourceLength - sourceStart);
                    const auto copiedFrames = juce::jmin (availableFrames, inputFramesRequested);

                    if (copiedFrames > 0)
                    {
                        const auto* sourceData = effectiveBuffer->getReadPointer (ch, sourceStart);
                        std::copy (sourceData, sourceData + copiedFrames, tempInput);
                    }
                }

                inPtrs[(size_t) ch] = tempInput;
                outPtrs[(size_t) ch] = chunkBuffer.getWritePointer (ch);
            }

            const int renderedFrames = offlineStream->process (inPtrs.data(), outPtrs.data(),
                                                               inputFramesRequested,
                                                               (double) segmentOutputFrames,
                                                               pitchFactor);

            if (renderedFrames > 0)
            {
                const int writableFrames = juce::jmin (renderedFrames,
                                                       finalOutputFrames - outputWriteOffset);
                if (writableFrames <= 0)
                    break;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    const auto* src = chunkBuffer.getReadPointer (ch);
                    auto* dst = outputBuffer.getWritePointer (ch) + outputWriteOffset;
                    for (int i = 0; i < writableFrames; ++i)
                        dst[i] = src[i] * chopGainLinear;
                }

                outputWriteOffset += writableFrames;
                zeroRenderStreak = 0;
            }
            else
            {
                ++zeroRenderStreak;
            }

            sourcePosition += (double) inputFramesRequested;

            if (zeroRenderStreak >= 16)
            {
                // Bungee isn't outputting anymore, force flush by padding with zero to hit finalOutputFrames exactly
                break;
            }
        }

        framesToWrite = finalOutputFrames;
    }

    if (framesToWrite > 0 && bitCrusherEnabledUi.load (std::memory_order_acquire))
    {
        outputBuffer.setSize (numChannels, framesToWrite, true, false, true);

        cuesampler::BitCrusher exportBitCrusher;
        exportBitCrusher.prepare (currentHostRate, framesToWrite, numChannels);
        exportBitCrusher.setParametersImmediately (
            true,
            bitsAmountToDspBits     (bitCrusherBitsUi .load (std::memory_order_acquire)),
            crushAmountToDspPercent (bitCrusherCrushUi.load (std::memory_order_acquire)));
        exportBitCrusher.process (outputBuffer);
    }

    if (framesToWrite > 0 && compressorEnabledUi.load (std::memory_order_acquire))
    {
        outputBuffer.setSize (numChannels, framesToWrite, true, false, true);

        cuesampler::SSLBusCompressor exportCompressor;
        exportCompressor.prepare (currentHostRate, framesToWrite, numChannels);
        exportCompressor.setParametersImmediately (
            true,
            compressorThresholdUiDb.load (std::memory_order_acquire),
            compressorMakeupUiDb.load (std::memory_order_acquire));
        exportCompressor.process (outputBuffer);
    }

    const auto sampleName = sampleData->fileName.isNotEmpty() ? sampleData->fileName
                                                               : juce::String ("chop");
    auto tempDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory);
    if (! tempDirectory.createDirectory())
        tempDirectory = juce::File ("/tmp");

    const auto tempFile = tempDirectory
                              .getChildFile (sampleName
                                             + "_chop" + juce::String (chopId)
                                             + "_gPitch_" + juce::String (globalSemitones, 2)
                                             + "_cPitch_" + juce::String (chop->pitchSemitones, 2)
                                             + "_export_" + juce::String (juce::Time::currentTimeMillis())
                                             + ".wav");
    tempFile.deleteFile();

    auto rawStream = std::make_unique<juce::FileOutputStream> (tempFile);
    if (! rawStream->openedOk())
        return {};

    std::unique_ptr<juce::OutputStream> outputStream = std::move (rawStream);

    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate (currentHostRate)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (24);

    auto writer = wav.createWriterFor (outputStream, options);
    if (writer == nullptr)
        return {};

    if (! writer->writeFromAudioSampleBuffer (outputBuffer, 0, framesToWrite))
    {
        writer.reset();
        tempFile.deleteFile();
        return {};
    }

    writer.reset();
    return tempFile;
}

void AudioPluginAudioProcessor::startPlayback() noexcept
{
    const auto currentSample = std::atomic_load (&loadedSample);
    if (currentSample == nullptr || currentSample->buffer.getNumSamples() == 0)
        return;

    double startPosition = 0.0;
    double stopSample = -1.0;
    double loopStartSample = -1.0;

    const auto currentChopState = std::atomic_load (&chopState);
    const auto* selectedChop = findSelectedChop (currentChopState.get());

    if (selectedChop != nullptr)
    {
        double cueStartSample = (double) juce::jlimit (selectedChop->startSample,
                                                       juce::jmax (selectedChop->startSample, selectedChop->endSample - 1),
                                                       selectedChop->startSample + selectedChop->cueOffsetSamples);

        const double sourceRate = juce::jmax (1.0, currentSample->sampleRate);
        if (! selectedChop->warpMarkers.empty() && sourceRate > 0.0)
        {
            cuesampler::WarpMap tempMap;
            tempMap.build (selectedChop->startSample, selectedChop->endSample, selectedChop->warpMarkers, sourceRate);
            const double cueLocalSeconds = tempMap.localTimeAtSourceSample (cueStartSample);
            cueStartSample = selectedChop->startSample + (cueLocalSeconds * sourceRate);
        }

        const auto currentPosition = playbackSamplePosition.load (std::memory_order_acquire);
        startPosition = currentPosition;
        if (currentPosition < cueStartSample
            || (selectedChop->warpMarkers.empty() && currentPosition >= (double) selectedChop->endSample))
        {
            startPosition = cueStartSample;
        }

        stopSample = (double) selectedChop->endSample;
        loopStartSample = cueStartSample;

        lastTriggeredChopId.store (selectedChop->id, std::memory_order_release);
        chopTriggerRevision.fetch_add (1, std::memory_order_acq_rel);
    }
    else
    {
        const auto defaultStartSample = juce::jlimit (0,
                                                      juce::jmax (0, currentSample->buffer.getNumSamples() - 1),
                                                      currentSample->leadingContentStartSample);
        const auto currentPosition = playbackSamplePosition.load (std::memory_order_acquire);
        startPosition = currentPosition;

        if (currentPosition < (double) defaultStartSample
            || currentPosition >= (double) currentSample->buffer.getNumSamples())
        {
            startPosition = (double) defaultStartSample;
        }

        stopSample = -1.0;
        loopStartSample = (double) defaultStartSample;
    }

    pendingStartPosition.store (startPosition, std::memory_order_release);
    pendingStartStopSample.store (stopSample, std::memory_order_release);
    pendingStartLoopSample.store (loopStartSample, std::memory_order_release);

    if (syncToHost.load (std::memory_order_acquire)
        && hostHasPpqPosition.load (std::memory_order_acquire)
        && hostBpm.load (std::memory_order_acquire) > 0.0)
    {
        pendingStartSyncPpq.store (hostPpqPosition.load (std::memory_order_acquire), std::memory_order_release);
        pendingStartSyncSample.store (startPosition, std::memory_order_release);
        pendingStartSyncAnchorValid.store (true, std::memory_order_release);
    }
    else
    {
        pendingStartSyncAnchorValid.store (false, std::memory_order_release);
    }

    playbackActive.store (true, std::memory_order_release);
    playbackSamplePosition.store (startPosition, std::memory_order_release);
    pendingTransportCommand.store ((int) TransportCommand::start, std::memory_order_release);
}

void AudioPluginAudioProcessor::pausePlayback() noexcept
{
    pendingTransportCommand.store ((int) TransportCommand::pause, std::memory_order_release);
    playbackActive.store (false, std::memory_order_release);
}

void AudioPluginAudioProcessor::stopPlayback() noexcept
{
    pendingTransportCommand.store ((int) TransportCommand::stop, std::memory_order_release);
    playbackActive.store (false, std::memory_order_release);
    playbackSamplePosition.store (0.0, std::memory_order_release);
}

void AudioPluginAudioProcessor::setPlaybackSamplePosition (double newPosition) noexcept
{
    const auto requestedPosition = std::isfinite (newPosition) ? newPosition : 0.0;
    double clampedPosition = 0.0;

    if (const auto currentSample = std::atomic_load (&loadedSample);
        currentSample != nullptr && currentSample->buffer.getNumSamples() > 0)
    {
        const auto maxPosition = juce::jmax (0.0, (double) currentSample->buffer.getNumSamples() - 1.0);
        clampedPosition = juce::jlimit (0.0, maxPosition, requestedPosition);
    }

    playbackSamplePosition.store (clampedPosition, std::memory_order_release);
    pendingTransportSeekPosition.store (clampedPosition, std::memory_order_release);
    pendingTransportCommand.store ((int) TransportCommand::seek, std::memory_order_release);
}

void AudioPluginAudioProcessor::setTimeStretchRatio (float newRatio) noexcept
{
    timeStretchRatio.store (juce::jlimit (0.25f, 4.0f, newRatio), std::memory_order_release);
}

void AudioPluginAudioProcessor::setPitchSemitones (float newSemitones) noexcept
{
    pitchSemitones.store (juce::jlimit (-24.0f, 24.0f, newSemitones), std::memory_order_release);
    // NOTE: intentionally NOT calling requestHostStateSync() here.
    // FL Studio's undo system responds to updateHostDisplay(nonParameterStateChanged)
    // by capturing an undo point and can subsequently restore a stale state snapshot,
    // which overwrites pitchSemitones back to 0.  The pitch value is still persisted
    // correctly via getStateInformation when the project is saved.
}

bool AudioPluginAudioProcessor::isPlaying() const noexcept
{
    return playbackActive.load (std::memory_order_acquire);
}

double AudioPluginAudioProcessor::getPlaybackSamplePosition() const noexcept
{
    return playbackSamplePosition.load (std::memory_order_acquire);
}

int AudioPluginAudioProcessor::getLastTriggeredChopId() const noexcept
{
    return lastTriggeredChopId.load (std::memory_order_acquire);
}

uint64_t AudioPluginAudioProcessor::getChopTriggerRevision() const noexcept
{
    return chopTriggerRevision.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getOutputMeterLevel() const noexcept
{
    return outputMeterLevel.load (std::memory_order_acquire);
}

void AudioPluginAudioProcessor::setCompressorEnabled (bool shouldEnable) noexcept
{
    compressorEnabledUi.store (shouldEnable, std::memory_order_release);
    compressor.setEnabled (shouldEnable);
}

void AudioPluginAudioProcessor::setCompressorThresholdDb (float dB) noexcept
{
    const auto clamped = juce::jlimit (-15.0f, 15.0f, dB);
    compressorThresholdUiDb.store (clamped, std::memory_order_release);
    compressor.setThresholdDb (clamped);
}

void AudioPluginAudioProcessor::setCompressorMakeupDb (float dB) noexcept
{
    const auto clamped = juce::jlimit (0.0f, 20.0f, dB);
    compressorMakeupUiDb.store (clamped, std::memory_order_release);
    compressor.setMakeupDb (clamped);
}

bool AudioPluginAudioProcessor::isCompressorEnabled() const noexcept
{
    return compressorEnabledUi.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getCompressorThresholdDb() const noexcept
{
    return compressorThresholdUiDb.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getCompressorMakeupDb() const noexcept
{
    return compressorMakeupUiDb.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getCompressorGainReductionDb() const noexcept
{
    return compressor.getCurrentGainReductionDb();
}

void AudioPluginAudioProcessor::setBitCrusherEnabled (bool shouldEnable) noexcept
{
    bitCrusherEnabledUi.store (shouldEnable, std::memory_order_release);
    bitCrusher.setEnabled (shouldEnable);
}

void AudioPluginAudioProcessor::setBitCrusherBits (float amount) noexcept
{
    const auto clampedAmount = juce::jlimit (0.0f, 100.0f, amount);
    bitCrusherBitsUi.store (clampedAmount, std::memory_order_release);
    bitCrusher.setBits (bitsAmountToDspBits (clampedAmount));
}

void AudioPluginAudioProcessor::setBitCrusherCrush (float amount) noexcept
{
    const auto clampedAmount = juce::jlimit (0.0f, 100.0f, amount);
    bitCrusherCrushUi.store (clampedAmount, std::memory_order_release);
    bitCrusher.setCrush (crushAmountToDspPercent (clampedAmount));
}

bool AudioPluginAudioProcessor::isBitCrusherEnabled() const noexcept
{
    return bitCrusherEnabledUi.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getBitCrusherBits() const noexcept
{
    return bitCrusherBitsUi.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getBitCrusherCrush() const noexcept
{
    return bitCrusherCrushUi.load (std::memory_order_acquire);
}

void AudioPluginAudioProcessor::readBitCrusherScope (float* dest, int maxSamples) const noexcept
{
    const int n = juce::jmin (maxSamples, kBitCrusherScopeSize);
    for (int i = 0; i < n; ++i)
        dest[i] = bitCrusherScope[(size_t) i].load (std::memory_order_relaxed);
}

float AudioPluginAudioProcessor::getTimeStretchRatio() const noexcept
{
    return timeStretchRatio.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getPitchSemitones() const noexcept
{
    return pitchSemitones.load (std::memory_order_acquire);
}

void AudioPluginAudioProcessor::setSyncToHost (bool shouldSync) noexcept
{
    syncToHost.store (shouldSync, std::memory_order_release);
    auto& v = voices[activeVoiceIdx];
    v.bungeeResetPending = true;

    if (shouldSync)
    {
        const auto analysis = std::atomic_load (&tempoAnalysis);
        const auto currentHostBpm = hostBpm.load (std::memory_order_acquire);
        if (analysis != nullptr)
            updateHostSyncStretchRatio (*analysis, currentHostBpm);
        else
            timeStretchRatio.store (1.0f, std::memory_order_release);

        if (hostHasPpqPosition.load (std::memory_order_acquire) && currentHostBpm > 0.0)
        {
            v.playbackSyncStartPpq = hostPpqPosition.load (std::memory_order_acquire);
            v.playbackSyncStartSample = v.playbackSamplePosition;
            v.playbackSyncAnchorValid = true;
        }
        else
        {
            v.playbackSyncAnchorValid = false;
        }
    }
    else
    {
        timeStretchRatio.store (1.0f, std::memory_order_release);
        v.playbackSyncAnchorValid = false;
    }

    requestHostStateSync();
}

void AudioPluginAudioProcessor::setHalfTimeEnabled (bool shouldEnable) noexcept
{
    halfTimeEnabled.store (shouldEnable, std::memory_order_release);
    voices[activeVoiceIdx].bungeeResetPending = true;
    requestHostStateSync();
}

void AudioPluginAudioProcessor::setMidiOctaveOffset (int octaves) noexcept
{
    const int clamped = juce::jlimit (midiOctaveOffsetMin, midiOctaveOffsetMax, octaves);
    if (clamped == midiOctaveOffset.exchange (clamped, std::memory_order_acq_rel))
        return; // no change → don't churn the UI / host state

    // The note→chop mapping just moved, so refresh the editor (re-lights the
    // on-screen keyboard key for the selected chop) and persist the new offset.
    notifyEditStateChanged();
}

int AudioPluginAudioProcessor::getMidiOctaveOffset() const noexcept
{
    return midiOctaveOffset.load (std::memory_order_acquire);
}

int AudioPluginAudioProcessor::getMidiRootNote() const noexcept
{
    return midiRootNote + midiOctaveOffset.load (std::memory_order_acquire) * 12;
}

int AudioPluginAudioProcessor::getMidiNoteForChopId (int chopId) const noexcept
{
    if (chopId < 0)
        return -1;

    const auto state = std::atomic_load (&chopState);
    if (state == nullptr)
        return -1;

    for (size_t i = 0; i < state->chops.size(); ++i)
        if (state->chops[i].id == chopId)
            return getMidiRootNote() + (int) i;

    return -1;
}

int AudioPluginAudioProcessor::getSelectedChopMidiNote() const noexcept
{
    const auto state = std::atomic_load (&chopState);
    return state != nullptr ? getMidiNoteForChopId (state->selectedChopId) : -1;
}

void AudioPluginAudioProcessor::setGridBpmTrim (float trimBpm)
{
    if (! juce::approximatelyEqual (trimBpm, gridBpmTrim.load (std::memory_order_acquire)))
        pushEditUndoSnapshot ("gridBpmTrim");
    gridBpmTrim.store (trimBpm, std::memory_order_release);
    const auto analysis = std::atomic_load (&tempoAnalysis);
    if (analysis != nullptr)
    {
        buildChopsFromAnalysis (*analysis);
        if (syncToHost.load (std::memory_order_acquire))
            updateHostSyncStretchRatio (*analysis, hostBpm.load (std::memory_order_acquire));

        // Data flywheel: the user is correcting the detected tempo. Capture
        // the algorithm's guess vs. the settled value (coalesced internally).
        editTelemetry.recordBpmCorrection (analysis->estimatedBpm,
                                           analysis->estimatedBpm + (double) trimBpm,
                                           analysis->confidence,
                                           analysis->likelyDrifting);
        notifyEditStateChanged();
    }
    touchTempoUiRevision();
    requestHostStateSync();
}

bool AudioPluginAudioProcessor::getSyncToHost() const noexcept
{
    return syncToHost.load (std::memory_order_acquire);
}

bool AudioPluginAudioProcessor::getHalfTimeEnabled() const noexcept
{
    return halfTimeEnabled.load (std::memory_order_acquire);
}

double AudioPluginAudioProcessor::getHostBpm() const noexcept
{
    return hostBpm.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getGridBpmTrim() const noexcept
{
    return gridBpmTrim.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getGridStartOffset() const noexcept
{
    return gridStartOffset.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getWaveformZoom() const noexcept
{
    return waveformZoom.load (std::memory_order_acquire);
}

float AudioPluginAudioProcessor::getWaveformScroll() const noexcept
{
    return waveformScroll.load (std::memory_order_acquire);
}

double AudioPluginAudioProcessor::getResolvedGridAnchorSeconds() const noexcept
{
    const auto analysis = std::atomic_load (&tempoAnalysis);
    if (analysis == nullptr)
        return 0.0;

    double beatPeriodSeconds = 0.0;
    double barPeriodSeconds = 0.0;
    double chopPeriodSeconds = 0.0;
    double gridAnchorSeconds = 0.0;

    if (! computeGridTimingMetrics (*analysis, beatPeriodSeconds, barPeriodSeconds, chopPeriodSeconds, gridAnchorSeconds))
        return 0.0;

    return gridAnchorSeconds;
}

void AudioPluginAudioProcessor::setGridStartOffset (float offsetSeconds)
{
    if (! juce::approximatelyEqual (offsetSeconds, gridStartOffset.load (std::memory_order_acquire)))
        pushEditUndoSnapshot ("gridStartOffset");
    gridStartOffset.store (offsetSeconds, std::memory_order_release);
    const auto analysis = std::atomic_load (&tempoAnalysis);
    if (analysis != nullptr)
    {
        buildChopsFromAnalysis (*analysis);
        notifyEditStateChanged();
    }
    touchTempoUiRevision();
    requestHostStateSync();
}

void AudioPluginAudioProcessor::resizeChopBoundary (int chopId, int newStartSample, int newEndSample)
{
    juce::ignoreUnused (chopId);

    if (newEndSample <= newStartSample)
        return;

    const auto sampleData = std::atomic_load (&loadedSample);
    if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
        return;

    const double sr = sampleData->sampleRate;
    if (sr <= 0.0)
        return;

    const auto analysis = std::atomic_load (&tempoAnalysis);
    if (analysis == nullptr || analysis->estimatedBpm <= 0.0)
        return;

    const int barsPerChop = juce::jmax (1, chopBarsCount.load (std::memory_order_acquire));

    const int totalSamples = sampleData->buffer.getNumSamples();
    const int clampedStart = juce::jlimit (0, juce::jmax (0, totalSamples - 2), newStartSample);
    const int clampedEnd   = juce::jlimit (clampedStart + 1, totalSamples, newEndSample);

    const double newChopPeriodSec = (double) (clampedEnd - clampedStart) / sr;
    if (newChopPeriodSec <= 0.0)
        return;

    pushEditUndoSnapshot ("resizeChop");

    // Reverse the BPM that would produce this chop length at the current
    // bars-per-chop setting, then convert that into a trim relative to the
    // analysis BPM (which is what computeGridTimingMetrics adds back in).
    const double newBeatPeriodSec = newChopPeriodSec / (4.0 * (double) barsPerChop);
    const double newAdjustedBpm   = 60.0 / newBeatPeriodSec;
    const float  newTrim          = (float) (newAdjustedBpm - analysis->estimatedBpm);
    gridBpmTrim.store (newTrim, std::memory_order_release);

    // Re-derive gridStartOffset so a fresh chop boundary lands exactly at
    // clampedStart. Mirrors the math in computeGridTimingMetrics.
    const double newBarPeriodSec = newBeatPeriodSec * 4.0;
    double gridRefSeconds = analysis->analysisStartSeconds;
    if (! analysis->barPositionsSeconds.empty())
        gridRefSeconds = analysis->barPositionsSeconds.front();
    else if (analysis->firstBeatSeconds > 0.0)
        gridRefSeconds = analysis->firstBeatSeconds - (double) analysis->downbeatPhase * newBeatPeriodSec;

    if (std::isfinite (gridRefSeconds))
        gridRefSeconds -= std::ceil (gridRefSeconds / newBarPeriodSec) * newBarPeriodSec;

    const double newStartSec    = (double) clampedStart / sr;
    const double k              = std::floor (newStartSec / newChopPeriodSec);
    const double desiredAnchor  = newStartSec - k * newChopPeriodSec;
    const double newOffsetSec   = desiredAnchor - gridRefSeconds;

    gridStartOffset.store ((float) newOffsetSec, std::memory_order_release);

    buildChopsFromAnalysis (*analysis);
    if (syncToHost.load (std::memory_order_acquire))
        updateHostSyncStretchRatio (*analysis, hostBpm.load (std::memory_order_acquire));

    // Re-target the selection to the chop whose start lines up with the user's
    // dragged edge so the same slice stays highlighted across the rebuild.
    if (auto rebuilt = std::atomic_load (&chopState); rebuilt != nullptr)
    {
        const int sampleTolerance = juce::jmax (1, (int) std::round (sr * 0.005));
        int matchedId = -1;
        for (const auto& c : rebuilt->chops)
        {
            if (std::abs (c.startSample - clampedStart) <= sampleTolerance)
            {
                matchedId = c.id;
                break;
            }
        }
        if (matchedId >= 0 && matchedId != rebuilt->selectedChopId)
        {
            auto updated = std::make_shared<ChopState> (*rebuilt);
            updated->selectedChopId = matchedId;
            std::atomic_store (&chopState, updated);
        }
    }

    notifyEditStateChanged();
    touchTempoUiRevision();
    requestHostStateSync();
}

void AudioPluginAudioProcessor::setChopBounds (int chopId, int newStartSample, int newEndSample)
{
    if (newEndSample <= newStartSample)
        return;

    const auto sampleData = std::atomic_load (&loadedSample);
    if (sampleData == nullptr || sampleData->buffer.getNumSamples() == 0)
        return;

    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr)
        return;

    const int totalSamples = sampleData->buffer.getNumSamples();
    const int clampedStart = juce::jlimit (0, juce::jmax (0, totalSamples - 2), newStartSample);
    const int clampedEnd   = juce::jlimit (clampedStart + 1, totalSamples, newEndSample);
    if (clampedEnd - clampedStart < 2)
        return;

    auto nextState = std::make_shared<ChopState> (*currentState);
    auto chopIt = std::find_if (nextState->chops.begin(), nextState->chops.end(),
                                [chopId] (const ChopDefinition& chop)
                                {
                                    return chop.id == chopId;
                                });

    if (chopIt == nextState->chops.end())
        return;

    pushEditUndoSnapshot ("chopBounds:" + juce::String (chopId));

    const auto oldStart = chopIt->startSample;
    const auto oldCueSample = oldStart + chopIt->cueOffsetSamples;

    chopIt->startSample = clampedStart;
    chopIt->endSample = clampedEnd;

    const int newLength = juce::jmax (1, clampedEnd - clampedStart - 1);
    const int clampedCueSample = juce::jlimit (clampedStart, juce::jmax (clampedStart, clampedEnd - 1), oldCueSample);
    chopIt->cueOffsetSamples = juce::jlimit (0, newLength, clampedCueSample - clampedStart);

    chopIt->warpMarkers.erase (std::remove_if (chopIt->warpMarkers.begin(), chopIt->warpMarkers.end(),
                                               [clampedStart, clampedEnd] (const ChopWarpMarker& marker)
                                               {
                                                   return marker.sourceSample <= clampedStart
                                                       || marker.sourceSample >= clampedEnd;
                                               }),
                               chopIt->warpMarkers.end());
    const bool hasWarpMarkers = ! chopIt->warpMarkers.empty();

    std::sort (nextState->chops.begin(), nextState->chops.end(),
               [] (const ChopDefinition& left, const ChopDefinition& right)
               {
                   if (left.startSample == right.startSample)
                       return left.id < right.id;

                   return left.startSample < right.startSample;
               });
    nextState->selectedChopId = chopId;
    std::atomic_store (&chopState, nextState);

    chopAudioCache.evict (chopId);
    if (hasWarpMarkers)
        requestChopWarpRender (chopId);

    notifyEditStateChanged();
    touchTempoUiRevision();
    requestHostStateSync();
}

void AudioPluginAudioProcessor::setWaveformZoom (float zoomValue) noexcept
{
    waveformZoom.store (juce::jlimit (0.0f, 1.0f, zoomValue), std::memory_order_release);
    requestHostStateSync();
}

void AudioPluginAudioProcessor::setWaveformScroll (float scrollValue) noexcept
{
    waveformScroll.store (juce::jlimit (0.0f, 1.0f, scrollValue), std::memory_order_release);
    requestHostStateSync();
}

bool AudioPluginAudioProcessor::isTempoAnalysisInProgress() const noexcept
{
    return tempoAnalysisInProgress.load (std::memory_order_acquire);
}

bool AudioPluginAudioProcessor::isKeyDetectionInProgress() const noexcept
{
    return keyDetectionInProgress.load (std::memory_order_acquire);
}

KeyDetector::Result AudioPluginAudioProcessor::getDetectedKey() const
{
    const std::lock_guard<std::mutex> lock (keyResultMutex);
    return detectedKeyResult;
}

void AudioPluginAudioProcessor::setUserKeyOverride (int rootIndex, bool isMajor)
{
    auto newResult = KeyDetector::makeResult (rootIndex, isMajor);

    KeyDetector::Result previous;
    {
        const std::lock_guard<std::mutex> lock (keyResultMutex);
        previous = detectedKeyResult;
        detectedKeyResult = newResult;
    }

    // Cancel any in-flight detection so it can't overwrite the user's choice.
    keyDetectionGeneration.fetch_add (1, std::memory_order_acq_rel);
    keyDetectionInProgress.store (false, std::memory_order_release);

    // Flywheel: the detector's key (previous) vs. the user's choice (truth).
    editTelemetry.recordKeyCorrection (previous.valid ? previous.key : std::string(),
                                       previous.confidence,
                                       newResult.key);

    // Refresh the UI readout.
    tempoUiRevision.fetch_add (1, std::memory_order_acq_rel);
}

uint64_t AudioPluginAudioProcessor::getTempoUiRevision() const noexcept
{
    return tempoUiRevision.load (std::memory_order_acquire);
}

void AudioPluginAudioProcessor::launchTempoAnalysis (std::shared_ptr<const LoadedSampleData> sampleData)
{
    // Drop queued analysis requests so only the latest region/file request survives.
    analysisThreadPool.removeAllJobs (false, 0);
    tempoAnalysisInProgress.store (true, std::memory_order_release);
    const auto generation = tempoAnalysisGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    touchTempoUiRevision();
    analysisThreadPool.addJob (new TempoAnalysisJob (*this, std::move (sampleData), std::atomic_load (&tempoEditState), generation), true);
}

void AudioPluginAudioProcessor::publishTempoAnalysis (std::shared_ptr<TempoAnalysisData> analysisResult,
                                                      uint64_t analysisGeneration)
{
    if (analysisGeneration != tempoAnalysisGeneration.load (std::memory_order_acquire))
        return;

    const auto detectedBpm = (analysisResult != nullptr) ? analysisResult->estimatedBpm : 0.0;

    // Keep host-sync stretch current before rebuilding the chop grid, so live
    // playback reads the same timing state the UI displays.
    if (syncToHost.load (std::memory_order_acquire) && detectedBpm > 0.0)
    {
        const auto currentHostBpm = hostBpm.load (std::memory_order_acquire);
        if (analysisResult != nullptr)
            updateHostSyncStretchRatio (*analysisResult, currentHostBpm);
    }

    if (analysisResult != nullptr)
        buildChopsFromAnalysis (*analysisResult);

    std::atomic_store (&tempoAnalysis, std::move (analysisResult));
    tempoAnalysisInProgress.store (false, std::memory_order_release);

    touchTempoUiRevision();
    notifyEditStateChanged();
}

//==============================================================================
// Stem separation — mirrors the launchTempoAnalysis / publishTempoAnalysis pair.
void AudioPluginAudioProcessor::launchStemSeparation (std::shared_ptr<LoadedSampleData> sampleData)
{
    // Abandon any in-flight separation / queued remix for the previous sample and
    // reset stem state. appliedStemMask = -1 marks loadedSample as the raw original.
    stemThreadPool.removeAllJobs (false, 0);
    std::atomic_store (&stemSet, std::shared_ptr<const StemSet> {});
    stemsReady.store (false, std::memory_order_release);
    stemProgress.store (0.0f, std::memory_order_release);
    stemSeparationSkipped.store (false, std::memory_order_release);
    appliedStemMask.store (-1, std::memory_order_release);

    // No separator (models missing / failed) → behave exactly as before: original
    // plays, mutes stay disabled.
    if (stemSeparator == nullptr || ! stemSeparator->isReady()
        || sampleData == nullptr || sampleData->buffer.getNumSamples() <= 0)
    {
        stemSeparationInProgress.store (false, std::memory_order_release);
        if (stemSeparator == nullptr || ! stemSeparator->isReady())
            juce::Logger::writeToLog ("StemSeparator: separation unavailable (models not ready)");
        return;
    }

    // Length guard: skip separation for very long samples (RAM/time). The panel
    // shows a brief reason; the original plays and mutes stay disabled.
    const double durationSeconds = sampleData->sampleRate > 0.0
        ? (double) sampleData->buffer.getNumSamples() / sampleData->sampleRate
        : 0.0;
    if (durationSeconds > kMaxStemSeparationSeconds)
    {
        stemSeparationInProgress.store (false, std::memory_order_release);
        stemSeparationSkipped.store (true, std::memory_order_release);
        juce::Logger::writeToLog ("StemSeparator: sample too long ("
            + juce::String (durationSeconds, 1) + "s > "
            + juce::String (kMaxStemSeparationSeconds, 0) + "s) — skipping separation");
        return;
    }

    stemSeparationInProgress.store (true, std::memory_order_release);
    const auto generation = stemGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    stemThreadPool.addJob (new StemSeparationJob (*this, std::move (sampleData), generation), true);
}

void AudioPluginAudioProcessor::publishStems (std::shared_ptr<const StemSet> newStemSet, uint64_t generation)
{
    if (generation != stemGeneration.load (std::memory_order_acquire))
        return; // superseded by a newer sample / separation

    stemSeparationInProgress.store (false, std::memory_order_release);

    if (newStemSet == nullptr)
    {
        // Separation unavailable or failed → keep the original buffer in place.
        stemsReady.store (false, std::memory_order_release);
        return;
    }

    std::atomic_store (&stemSet, newStemSet);
    stemProgress.store (1.0f, std::memory_order_release);
    stemsReady.store (true, std::memory_order_release);
    rebuildActiveMix(); // apply any active mutes now that stems exist
}

void AudioPluginAudioProcessor::rebuildActiveMix()
{
    const auto stems = std::atomic_load (&stemSet);
    if (stems == nullptr || stems->source == nullptr)
        return; // no stems → original already playing

    const int desiredMask = (muteDrums.load (std::memory_order_acquire)  ? 1 : 0)
                          | (muteBass.load (std::memory_order_acquire)   ? 2 : 0)
                          | (muteVocals.load (std::memory_order_acquire) ? 4 : 0);

    int currentMask = appliedStemMask.load (std::memory_order_acquire);
    if (currentMask < 0)
        currentMask = 0; // raw original is equivalent to "nothing muted"

    if (desiredMask == currentMask)
    {
        appliedStemMask.store (desiredMask, std::memory_order_release);
        return; // already correct — avoid a needless swap + cache rebuild
    }

    std::shared_ptr<LoadedSampleData> newSample;

    if (desiredMask == 0)
    {
        // Nothing muted → restore the exact pristine original (zero-copy alias).
        newSample = stems->source;
    }
    else
    {
        const auto& orig = stems->source->buffer;
        const int numCh = orig.getNumChannels();
        const int numS  = orig.getNumSamples();

        juce::AudioBuffer<float> mix (numCh, numS);
        for (int ch = 0; ch < numCh; ++ch)
            mix.copyFrom (ch, 0, orig, ch, 0, numS);

        auto subtract = [&] (const juce::AudioBuffer<float>& stem)
        {
            const int sc = juce::jmin (numCh, stem.getNumChannels());
            const int sn = juce::jmin (numS, stem.getNumSamples());
            for (int ch = 0; ch < sc; ++ch)
                mix.addFrom (ch, 0, stem, ch, 0, sn, -1.0f);
        };
        if (desiredMask & 1) subtract (stems->drums);
        if (desiredMask & 2) subtract (stems->bass);
        if (desiredMask & 4) subtract (stems->vocals);

        newSample = std::make_shared<LoadedSampleData>();
        newSample->sampleRate                = stems->source->sampleRate;
        newSample->sourceFile                = stems->source->sourceFile;
        newSample->filePath                  = stems->source->filePath;
        newSample->fileName                  = stems->source->fileName;
        newSample->leadingContentStartSample = stems->source->leadingContentStartSample;
        newSample->serializedStateData       = stems->source->serializedStateData;
        newSample->buffer                    = std::move (mix);
    }

    std::atomic_store (&loadedSample, newSample);
    appliedStemMask.store (desiredMask, std::memory_order_release);

    // Chop list/positions are unchanged but the audio content changed, so the warp
    // + prepared caches are stale. Refresh exactly like a content change does: drop
    // the warp cache, bump generations, re-bake chops with markers. The prepared
    // cache re-warms via warmPreparedCacheTick (now keyed on loadedSample identity).
    warpRenderThreadPool.removeAllJobs (false, 0);
    chopAudioCache.clear();
    warpRenderGeneration.fetch_add (1, std::memory_order_acq_rel);
    prepareWarmGeneration.fetch_add (1, std::memory_order_acq_rel);

    if (const auto currentChopState = std::atomic_load (&chopState); currentChopState != nullptr)
        for (const auto& c : currentChopState->chops)
            if (! c.warpMarkers.empty())
                requestChopWarpRender (c.id);
}

void AudioPluginAudioProcessor::scheduleRebuildActiveMix()
{
    if (std::atomic_load (&stemSet) == nullptr)
        return; // no stems yet → original keeps playing; mutes apply on publish

    const auto generation = stemRemixGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    stemThreadPool.addJob (new RemixJob (*this, generation), true);
}

void AudioPluginAudioProcessor::setMuteDrums (bool shouldMute) noexcept
{
    muteDrums.store (shouldMute, std::memory_order_release);
    scheduleRebuildActiveMix();
}

void AudioPluginAudioProcessor::setMuteBass (bool shouldMute) noexcept
{
    muteBass.store (shouldMute, std::memory_order_release);
    scheduleRebuildActiveMix();
}

void AudioPluginAudioProcessor::setMuteVocals (bool shouldMute) noexcept
{
    muteVocals.store (shouldMute, std::memory_order_release);
    scheduleRebuildActiveMix();
}

bool AudioPluginAudioProcessor::areStemModelsAvailable() const noexcept
{
    return stemSeparator != nullptr && stemSeparator->isReady();
}

void AudioPluginAudioProcessor::buildChopsFromAnalysis (const TempoAnalysisData& analysis)
{
    const auto currentSample = std::atomic_load (&loadedSample);
    if (currentSample == nullptr || analysis.beatPeriodSeconds <= 0.0)
        return;

    const auto sampleRate = currentSample->sampleRate;
    const auto totalSamples = currentSample->buffer.getNumSamples();
    double beatPeriodSeconds = 0.0;
    double barPeriodSeconds = 0.0;
    double chopPeriodSeconds = 0.0;
    double gridAnchorSeconds = 0.0;
    if (! computeGridTimingMetrics (analysis, beatPeriodSeconds, barPeriodSeconds, chopPeriodSeconds, gridAnchorSeconds))
        return;

    const auto totalDuration = (double) totalSamples / sampleRate;

    double chopStart = gridAnchorSeconds;
    if (chopStart < 0.0)
        chopStart += std::ceil (-chopStart / barPeriodSeconds) * barPeriodSeconds;

    const auto existingState = std::atomic_load (&chopState);

    // Find the index of the currently selected chop so we can re-select by index after the rebuild.
    int selectedIndex = 0;
    if (existingState != nullptr && existingState->selectedChopId >= 0)
    {
        for (int i = 0; i < (int) existingState->chops.size(); ++i)
        {
            if (existingState->chops[(size_t) i].id == existingState->selectedChopId)
            {
                selectedIndex = i;
                break;
            }
        }
    }

    auto newChopState = std::make_shared<ChopState>();
    int chopIndex = 0;
    while (chopStart < totalDuration)
    {
        const auto chopEnd = chopStart + chopPeriodSeconds;
        const int startSample = juce::jlimit (0, totalSamples - 1,
                                              (int) std::round (chopStart * sampleRate));
        const int endSample   = juce::jlimit (startSample + 1, totalSamples,
                                              (int) std::round (chopEnd * sampleRate));
        if (endSample > startSample)
        {
            const auto autoCueStart = findAutoCueStartSample (*currentSample, startSample, endSample);
            const auto autoCueOffset = juce::jlimit (0,
                                                     juce::jmax (0, endSample - startSample - 1),
                                                     autoCueStart - startSample);
            ChopDefinition def { newChopState->nextChopId++, startSample, endSample,
                                 autoCueOffset, 0.0f, 0.0f, false, {} };

            // Preserve any per-chop edits from the old chop at the same grid index.
            if (existingState != nullptr && chopIndex < (int) existingState->chops.size())
            {
                const auto& old = existingState->chops[(size_t) chopIndex];
                if (old.cueOffsetSamples > 0)
                    def.cueOffsetSamples = old.cueOffsetSamples;
                def.gainDecibels     = old.gainDecibels;
                def.pitchSemitones   = old.pitchSemitones;
                def.favorite         = old.favorite;

                // Carry warp markers across the rebuild, dropping any that no longer
                // fall inside the new chop bounds.
                for (const auto& marker : old.warpMarkers)
                {
                    if (marker.sourceSample > def.startSample && marker.sourceSample < def.endSample)
                        def.warpMarkers.push_back (marker);
                }
            }

            newChopState->chops.push_back (def);
            ++chopIndex;
        }
        chopStart = chopEnd;
    }

    if (! newChopState->chops.empty())
    {
        const auto clampedIndex = juce::jlimit (0, (int) newChopState->chops.size() - 1, selectedIndex);
        newChopState->selectedChopId = newChopState->chops[(size_t) clampedIndex].id;
    }

    std::atomic_store (&chopState, newChopState);

    // Old chop ids no longer exist after a rebuild — drop the entire warp
    // cache, then re-bake any chops that carried markers across.
    warpRenderThreadPool.removeAllJobs (false, 0);
    chopAudioCache.clear();
    for (const auto& c : newChopState->chops)
    {
        if (! c.warpMarkers.empty())
            requestChopWarpRender (c.id);
    }
}

bool AudioPluginAudioProcessor::computeGridTimingMetrics (const TempoAnalysisData& analysis,
                                                          double& beatPeriodSeconds,
                                                          double& barPeriodSeconds,
                                                          double& chopPeriodSeconds,
                                                          double& gridAnchorSeconds) const noexcept
{
    if (analysis.beatPeriodSeconds <= 0.0)
        return false;

    const auto barsPerChop = chopBarsCount.load (std::memory_order_acquire);
    const auto adjustedBpm = getAdjustedAnalysisBpm (analysis);
    const auto scaleFactor = (adjustedBpm > 0.0 && analysis.estimatedBpm > 0.0)
                           ? analysis.estimatedBpm / adjustedBpm : 1.0;

    beatPeriodSeconds = analysis.beatPeriodSeconds * scaleFactor;
    barPeriodSeconds = beatPeriodSeconds * 4.0;
    chopPeriodSeconds = barPeriodSeconds * (double) barsPerChop;
    if (beatPeriodSeconds <= 0.0 || barPeriodSeconds <= 0.0 || chopPeriodSeconds <= 0.0)
        return false;

    const auto startOffsetSeconds = (double) gridStartOffset.load (std::memory_order_acquire);
    double gridReferenceSeconds = analysis.analysisStartSeconds;

    if (! analysis.barPositionsSeconds.empty())
    {
        gridReferenceSeconds = analysis.barPositionsSeconds.front();
    }
    else if (analysis.firstBeatSeconds > 0.0)
    {
        gridReferenceSeconds = analysis.firstBeatSeconds - (double) analysis.downbeatPhase * beatPeriodSeconds;
    }

    if (std::isfinite (gridReferenceSeconds))
        gridReferenceSeconds -= std::ceil (gridReferenceSeconds / barPeriodSeconds) * barPeriodSeconds;

    gridAnchorSeconds = gridReferenceSeconds + startOffsetSeconds;
    return true;
}

double AudioPluginAudioProcessor::getAdjustedAnalysisBpm (const TempoAnalysisData& analysis) const noexcept
{
    return analysis.estimatedBpm + (double) gridBpmTrim.load (std::memory_order_acquire);
}

void AudioPluginAudioProcessor::updateHostSyncStretchRatio (const TempoAnalysisData& analysis,
                                                            double currentHostBpm) noexcept
{
    const auto adjustedBpm = getAdjustedAnalysisBpm (analysis);
    const auto currentRatio = timeStretchRatio.load (std::memory_order_acquire);
    const auto desiredRatio = (currentHostBpm > 0.0 && adjustedBpm > 0.0)
        ? juce::jlimit (0.25f, 4.0f, (float) (adjustedBpm / currentHostBpm))
        : 1.0f;

    if (std::abs (desiredRatio - currentRatio) <= 0.0005f)
        return;

    timeStretchRatio.store (desiredRatio, std::memory_order_release);
    voices[activeVoiceIdx].bungeeResetPending = true;
}

void AudioPluginAudioProcessor::setChopBarsCount (int bars)
{
    const int clamped = (bars <= 1) ? 1 : (bars <= 2) ? 2 : (bars <= 4) ? 4 : 8;
    if (clamped != chopBarsCount.load (std::memory_order_acquire))
        pushEditUndoSnapshot ({});
    chopBarsCount.store (clamped, std::memory_order_release);

    const auto analysis = std::atomic_load (&tempoAnalysis);
    if (analysis != nullptr)
    {
        buildChopsFromAnalysis (*analysis);
        touchTempoUiRevision();
        notifyEditStateChanged();
    }

    requestHostStateSync();
}

int AudioPluginAudioProcessor::getChopBarsCount() const noexcept
{
    return chopBarsCount.load (std::memory_order_acquire);
}

void AudioPluginAudioProcessor::setTempoAnalysisRegion (int startSample, int endSample)
{
    const auto currentSample = std::atomic_load (&loadedSample);
    if (currentSample == nullptr || currentSample->buffer.getNumSamples() == 0)
        return;

    const auto totalSamples = currentSample->buffer.getNumSamples();
    const auto clampedStart = juce::jlimit (0, totalSamples - 1, juce::jmin (startSample, endSample));
    const auto clampedEnd = juce::jlimit (clampedStart + 1, totalSamples, juce::jmax (startSample, endSample));

    auto nextState = std::make_shared<TempoEditState> (*std::atomic_load (&tempoEditState));
    nextState->regionStartSample = clampedStart;
    nextState->regionEndSample = clampedEnd;
    std::atomic_store (&tempoEditState, nextState);
    addChop (clampedStart, clampedEnd);
    launchTempoAnalysis (currentSample);
}

void AudioPluginAudioProcessor::clearTempoAnalysisRegion()
{
    const auto currentSample = std::atomic_load (&loadedSample);
    auto nextState = std::make_shared<TempoEditState> (*std::atomic_load (&tempoEditState));
    nextState->regionStartSample = -1;
    nextState->regionEndSample = -1;
    std::atomic_store (&tempoEditState, nextState);
    touchTempoUiRevision();

    if (currentSample != nullptr)
        launchTempoAnalysis (currentSample);
}

void AudioPluginAudioProcessor::setWarpDivision (int division) noexcept
{
    const int clamped = juce::jlimit (0, (int) WarpDivision_Sixteenth, division);
    warpDivision.store (clamped, std::memory_order_release);
}

double AudioPluginAudioProcessor::getCurrentGridFingerprint() const noexcept
{
    const auto analysis = std::atomic_load (&tempoAnalysis);
    if (analysis == nullptr || analysis->beatPeriodSeconds <= 0.0)
        return 0.0;

    const auto trimBpm     = (double) gridBpmTrim.load (std::memory_order_acquire);
    const auto adjustedBpm = analysis->estimatedBpm + trimBpm;
    const auto scaleFactor = (adjustedBpm > 0.0 && analysis->estimatedBpm > 0.0)
                              ? analysis->estimatedBpm / adjustedBpm
                              : 1.0;
    return analysis->beatPeriodSeconds * scaleFactor;
}

double AudioPluginAudioProcessor::getWarpDivisionSeconds() const noexcept
{
    const auto analysis = std::atomic_load (&tempoAnalysis);
    if (analysis == nullptr || analysis->beatPeriodSeconds <= 0.0)
        return 0.0;

    const auto trimBpm     = (double) gridBpmTrim.load (std::memory_order_acquire);
    const auto adjustedBpm = analysis->estimatedBpm + trimBpm;
    const auto scaleFactor = (adjustedBpm > 0.0 && analysis->estimatedBpm > 0.0)
                              ? analysis->estimatedBpm / adjustedBpm
                              : 1.0;
    const double beatPeriodSec = analysis->beatPeriodSeconds * scaleFactor;
    if (beatPeriodSec <= 0.0)
        return 0.0;

    switch (warpDivision.load (std::memory_order_acquire))
    {
        case WarpDivision_Bar:       return beatPeriodSec * 4.0;
        case WarpDivision_HalfBeat:  return beatPeriodSec * 0.5;
        case WarpDivision_Sixteenth: return beatPeriodSec * 0.25;
        case WarpDivision_Beat:
        default:                     return beatPeriodSec;
    }
}

void AudioPluginAudioProcessor::requestChopWarpRender (int chopId)
{
    const auto sampleSnapshot = std::atomic_load (&loadedSample);
    const auto chopStateSnapshot = std::atomic_load (&chopState);

    if (sampleSnapshot == nullptr
        || sampleSnapshot->buffer.getNumSamples() <= 0
        || chopStateSnapshot == nullptr)
    {
        chopAudioCache.evict (chopId);
        return;
    }

    // Confirm the chop still exists in the current state before queueing work.
    bool chopExists = false;
    for (const auto& c : chopStateSnapshot->chops)
    {
        if (c.id == chopId)
        {
            chopExists = true;
            break;
        }
    }

    if (! chopExists)
    {
        chopAudioCache.evict (chopId);
        return;
    }

    const auto generation = warpRenderGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;

    warpRenderThreadPool.addJob (new WarpRenderJob (*this,
                                                    chopAudioCache,
                                                    sampleSnapshot,
                                                    chopStateSnapshot,
                                                    chopId,
                                                    generation),
                                  true);
}

namespace
{
// Sort markers by sourceSample; clamp source into chop range minus 1; clamp
// localTime into (0, chopDurationSec). Returns the index of the marker that
// originated at oldSourceSample after the sort, or -1 if not found.
int sortMarkersAndLocate (std::vector<AudioPluginAudioProcessor::ChopWarpMarker>& markers,
                           int trackedSourceSample) noexcept
{
    std::stable_sort (markers.begin(), markers.end(),
                      [] (const auto& a, const auto& b)
                      {
                          return a.sourceSample < b.sourceSample;
                      });
    for (size_t i = 0; i < markers.size(); ++i)
        if (markers[i].sourceSample == trackedSourceSample)
            return (int) i;
    return -1;
}
} // namespace

int AudioPluginAudioProcessor::addOrUpdateChopWarpMarker (int chopId, int sourceSample, double localTimeSeconds, bool snappedToGrid)
{
    const auto sampleSnapshot = std::atomic_load (&loadedSample);
    const auto currentChopState = std::atomic_load (&chopState);
    if (sampleSnapshot == nullptr || currentChopState == nullptr)
        return -1;

    auto next = std::make_shared<ChopState> (*currentChopState);

    ChopDefinition* targetChop = nullptr;
    for (auto& c : next->chops)
    {
        if (c.id == chopId)
        {
            targetChop = &c;
            break;
        }
    }
    if (targetChop == nullptr)
        return -1;

    const int chopStart = targetChop->startSample;
    const int chopEnd   = targetChop->endSample;
    if (chopEnd <= chopStart + 1)
        return -1;

    const double sampleRate     = sampleSnapshot->sampleRate;
    const double chopDurationSec = (double) (chopEnd - chopStart) / sampleRate;
    if (sampleRate <= 0.0 || chopDurationSec <= 0.0)
        return -1;

    const int clampedSource = juce::jlimit (chopStart + 1, chopEnd - 1, sourceSample);
    const double minLocal = 1.0 / sampleRate;
    const double maxLocal = chopDurationSec - 1.0 / sampleRate;
    const double clampedLocal = juce::jlimit (minLocal, maxLocal, localTimeSeconds);

    // If a marker already exists at (or very close to) the same source position,
    // update it in place; otherwise add a new one.
    constexpr int kCoincidentSampleThreshold = 1;
    int existingIndex = -1;
    for (size_t i = 0; i < targetChop->warpMarkers.size(); ++i)
    {
        if (std::abs (targetChop->warpMarkers[i].sourceSample - clampedSource) <= kCoincidentSampleThreshold)
        {
            existingIndex = (int) i;
            break;
        }
    }

    const double fingerprint = getCurrentGridFingerprint();

    pushEditUndoSnapshot ({});

    if (existingIndex >= 0)
    {
        targetChop->warpMarkers[(size_t) existingIndex].sourceSample     = clampedSource;
        targetChop->warpMarkers[(size_t) existingIndex].localTimeSeconds = clampedLocal;
        targetChop->warpMarkers[(size_t) existingIndex].snappedToGrid    = snappedToGrid;
        targetChop->warpMarkers[(size_t) existingIndex].gridFingerprint  = snappedToGrid ? fingerprint : 0.0;
    }
    else
    {
        ChopWarpMarker marker;
        marker.sourceSample     = clampedSource;
        marker.localTimeSeconds = clampedLocal;
        marker.snappedToGrid    = snappedToGrid;
        marker.gridFingerprint  = snappedToGrid ? fingerprint : 0.0;
        targetChop->warpMarkers.push_back (marker);
    }

    const int newIndex = sortMarkersAndLocate (targetChop->warpMarkers, clampedSource);

    std::atomic_store (&chopState, next);
    requestChopWarpRender (chopId);
    touchTempoUiRevision();
    notifyEditStateChanged();
    return newIndex;
}

bool AudioPluginAudioProcessor::setChopWarpMarkerLocalTime (int chopId, int markerIndex, double localTimeSeconds, bool snappedToGrid)
{
    const auto sampleSnapshot = std::atomic_load (&loadedSample);
    const auto currentChopState = std::atomic_load (&chopState);
    if (sampleSnapshot == nullptr || currentChopState == nullptr)
        return false;

    auto next = std::make_shared<ChopState> (*currentChopState);

    ChopDefinition* targetChop = nullptr;
    for (auto& c : next->chops)
    {
        if (c.id == chopId)
        {
            targetChop = &c;
            break;
        }
    }
    if (targetChop == nullptr || markerIndex < 0
        || markerIndex >= (int) targetChop->warpMarkers.size())
        return false;

    const double sampleRate = sampleSnapshot->sampleRate;
    const int    chopStart  = targetChop->startSample;
    const int    chopEnd    = targetChop->endSample;
    if (sampleRate <= 0.0 || chopEnd <= chopStart + 1)
        return false;

    const double chopDurationSec = (double) (chopEnd - chopStart) / sampleRate;
    const double minLocal = 1.0 / sampleRate;
    const double maxLocal = chopDurationSec - 1.0 / sampleRate;

    // Constrain by neighbour markers' local times so monotonicity holds.
    auto& markers = targetChop->warpMarkers;
    double lower = minLocal;
    double upper = maxLocal;
    if (markerIndex > 0)
        lower = juce::jmax (lower, markers[(size_t) (markerIndex - 1)].localTimeSeconds + minLocal);
    if (markerIndex + 1 < (int) markers.size())
        upper = juce::jmin (upper, markers[(size_t) (markerIndex + 1)].localTimeSeconds - minLocal);
    if (lower >= upper)
        return false;

    pushEditUndoSnapshot ("warpLocal:" + juce::String (chopId) + ":" + juce::String (markerIndex));

    const double fingerprint = getCurrentGridFingerprint();
    markers[(size_t) markerIndex].localTimeSeconds = juce::jlimit (lower, upper, localTimeSeconds);
    markers[(size_t) markerIndex].snappedToGrid    = snappedToGrid;
    markers[(size_t) markerIndex].gridFingerprint  = snappedToGrid ? fingerprint : 0.0;

    std::atomic_store (&chopState, next);
    requestChopWarpRender (chopId);
    touchTempoUiRevision();
    notifyEditStateChanged();
    return true;
}

bool AudioPluginAudioProcessor::setChopWarpMarkerSourceSample (int chopId, int markerIndex, int sourceSample)
{
    const auto currentChopState = std::atomic_load (&chopState);
    if (currentChopState == nullptr)
        return false;

    auto next = std::make_shared<ChopState> (*currentChopState);

    ChopDefinition* targetChop = nullptr;
    for (auto& c : next->chops)
    {
        if (c.id == chopId)
        {
            targetChop = &c;
            break;
        }
    }
    if (targetChop == nullptr || markerIndex < 0
        || markerIndex >= (int) targetChop->warpMarkers.size())
        return false;

    const int chopStart = targetChop->startSample;
    const int chopEnd   = targetChop->endSample;
    if (chopEnd <= chopStart + 1)
        return false;

    auto& markers = targetChop->warpMarkers;

    // Constrain by neighbour markers' source samples so order stays consistent.
    int lower = chopStart + 1;
    int upper = chopEnd   - 1;
    if (markerIndex > 0)
        lower = juce::jmax (lower, markers[(size_t) (markerIndex - 1)].sourceSample + 1);
    if (markerIndex + 1 < (int) markers.size())
        upper = juce::jmin (upper, markers[(size_t) (markerIndex + 1)].sourceSample - 1);
    if (lower > upper)
        return false;

    pushEditUndoSnapshot ("warpSource:" + juce::String (chopId) + ":" + juce::String (markerIndex));

    markers[(size_t) markerIndex].sourceSample = juce::jlimit (lower, upper, sourceSample);

    std::atomic_store (&chopState, next);
    requestChopWarpRender (chopId);
    touchTempoUiRevision();
    notifyEditStateChanged();
    return true;
}

bool AudioPluginAudioProcessor::snapChopWarpMarkerToDivision (int chopId, int markerIndex, int divisionIndex)
{
    const auto sampleSnapshot = std::atomic_load (&loadedSample);
    const auto analysis        = std::atomic_load (&tempoAnalysis);
    const auto currentChopState = std::atomic_load (&chopState);
    if (sampleSnapshot == nullptr || analysis == nullptr || currentChopState == nullptr)
        return false;
    if (analysis->beatPeriodSeconds <= 0.0)
        return false;

    const ChopDefinition* targetChop = nullptr;
    for (const auto& c : currentChopState->chops)
    {
        if (c.id == chopId)
        {
            targetChop = &c;
            break;
        }
    }
    if (targetChop == nullptr || markerIndex < 0
        || markerIndex >= (int) targetChop->warpMarkers.size())
        return false;

    const auto trimBpm     = (double) gridBpmTrim.load (std::memory_order_acquire);
    const auto adjustedBpm = analysis->estimatedBpm + trimBpm;
    const auto scaleFactor = (adjustedBpm > 0.0 && analysis->estimatedBpm > 0.0)
                              ? analysis->estimatedBpm / adjustedBpm
                              : 1.0;
    const double beatPeriodSec = analysis->beatPeriodSeconds * scaleFactor;
    if (beatPeriodSec <= 0.0)
        return false;

    double divisionSec = beatPeriodSec;
    switch (divisionIndex)
    {
        case WarpDivision_Bar:       divisionSec = beatPeriodSec * 4.0;  break;
        case WarpDivision_HalfBeat:  divisionSec = beatPeriodSec * 0.5;  break;
        case WarpDivision_Sixteenth: divisionSec = beatPeriodSec * 0.25; break;
        case WarpDivision_Beat:
        default:                     divisionSec = beatPeriodSec;        break;
    }
    if (divisionSec <= 0.0)
        return false;

    const double currentLocal = targetChop->warpMarkers[(size_t) markerIndex].localTimeSeconds;
    const double snappedLocal = std::round (currentLocal / divisionSec) * divisionSec;

    return setChopWarpMarkerLocalTime (chopId, markerIndex, snappedLocal);
}

bool AudioPluginAudioProcessor::removeChopWarpMarker (int chopId, int markerIndex)
{
    const auto currentChopState = std::atomic_load (&chopState);
    if (currentChopState == nullptr)
        return false;

    auto next = std::make_shared<ChopState> (*currentChopState);

    ChopDefinition* targetChop = nullptr;
    for (auto& c : next->chops)
    {
        if (c.id == chopId)
        {
            targetChop = &c;
            break;
        }
    }
    if (targetChop == nullptr || markerIndex < 0
        || markerIndex >= (int) targetChop->warpMarkers.size())
        return false;

    pushEditUndoSnapshot ({});

    targetChop->warpMarkers.erase (targetChop->warpMarkers.begin() + markerIndex);

    std::atomic_store (&chopState, next);
    requestChopWarpRender (chopId);
    touchTempoUiRevision();
    notifyEditStateChanged();
    return true;
}

bool AudioPluginAudioProcessor::clearChopWarpMarkers (int chopId)
{
    const auto currentChopState = std::atomic_load (&chopState);
    if (currentChopState == nullptr)
        return false;

    auto next = std::make_shared<ChopState> (*currentChopState);

    ChopDefinition* targetChop = nullptr;
    for (auto& c : next->chops)
    {
        if (c.id == chopId)
        {
            targetChop = &c;
            break;
        }
    }
    if (targetChop == nullptr)
        return false;

    if (targetChop->warpMarkers.empty())
        return true;

    pushEditUndoSnapshot ({});

    targetChop->warpMarkers.clear();

    std::atomic_store (&chopState, next);
    requestChopWarpRender (chopId);
    touchTempoUiRevision();
    notifyEditStateChanged();
    return true;
}

void AudioPluginAudioProcessor::addChop (int startSample, int endSample)
{
    const auto currentSample = std::atomic_load (&loadedSample);
    if (currentSample == nullptr || currentSample->buffer.getNumSamples() == 0)
        return;

    const auto totalSamples = currentSample->buffer.getNumSamples();
    const auto clampedStart = juce::jlimit (0, totalSamples - 1, juce::jmin (startSample, endSample));
    const auto clampedEnd = juce::jlimit (clampedStart + 1, totalSamples, juce::jmax (startSample, endSample));

    auto nextState = std::make_shared<ChopState> (*std::atomic_load (&chopState));

    for (const auto& existingChop : nextState->chops)
    {
        if (existingChop.startSample == clampedStart && existingChop.endSample == clampedEnd)
        {
            nextState->selectedChopId = existingChop.id;
            std::atomic_store (&chopState, nextState);
            touchTempoUiRevision();
            notifyEditStateChanged();
            return;
        }
    }

    pushEditUndoSnapshot ({});

    const auto newChopId = nextState->nextChopId++;
    nextState->chops.push_back ({ newChopId, clampedStart, clampedEnd, 0, 0.0f, 0.0f, false, {} });
    std::sort (nextState->chops.begin(), nextState->chops.end(),
               [] (const ChopDefinition& left, const ChopDefinition& right)
               {
                   return left.startSample < right.startSample;
               });
    nextState->selectedChopId = newChopId;
    std::atomic_store (&chopState, nextState);
    touchTempoUiRevision();
    notifyEditStateChanged();
}

int AudioPluginAudioProcessor::chopAtTransients (TransientSensitivity sensitivity)
{
    const auto currentSample = std::atomic_load (&loadedSample);
    if (currentSample == nullptr || currentSample->buffer.getNumSamples() == 0)
        return 0;

    const auto& buffer = currentSample->buffer;
    const auto sampleRate = currentSample->sampleRate;
    const int totalSamples = buffer.getNumSamples();
    const int numChannels  = buffer.getNumChannels();
    if (sampleRate <= 0.0 || totalSamples <= 0 || numChannels <= 0)
        return 0;

    // ~10 ms windows with 50% overlap. Broadband RMS captures kicks and
    // snares well; positive flux of the envelope is the onset cue.
    const int windowSize = juce::jmax (32, (int) std::round (sampleRate * 0.010));
    const int hopSize    = juce::jmax (1, windowSize / 2);
    const int numHops    = totalSamples / hopSize;
    if (numHops < 4)
        return 0;

    std::vector<float> rms ((size_t) numHops, 0.0f);
    for (int hop = 0; hop < numHops; ++hop)
    {
        const int start = hop * hopSize;
        const int end   = juce::jmin (totalSamples, start + windowSize);
        double sumSq = 0.0;
        int frames = 0;
        for (int n = start; n < end; ++n)
        {
            double mono = 0.0;
            for (int c = 0; c < numChannels; ++c)
                mono += (double) buffer.getSample (c, n);
            mono /= (double) numChannels;
            sumSq += mono * mono;
            ++frames;
        }
        rms[(size_t) hop] = frames > 0 ? (float) std::sqrt (sumSq / (double) frames) : 0.0f;
    }

    // Positive first-difference of the envelope = onset strength.
    std::vector<float> flux ((size_t) numHops, 0.0f);
    float fluxMax = 0.0f;
    for (int i = 1; i < numHops; ++i)
    {
        const float d = rms[(size_t) i] - rms[(size_t) i - 1];
        flux[(size_t) i] = d > 0.0f ? d : 0.0f;
        fluxMax = juce::jmax (fluxMax, flux[(size_t) i]);
    }
    if (fluxMax <= 0.0f)
        return 0;
    for (auto& v : flux)
        v /= fluxMax;

    // Sensitivity profile: lower threshold + shorter min-gap => more onsets.
    // Light  = only heavy hits; Medium = kicks + snares; Fine = busy / fills.
    float threshold  = 0.18f;
    double minGapSec = 0.120;
    switch (sensitivity)
    {
        case TransientSensitivity::Light:  threshold = 0.35f; minGapSec = 0.250; break;
        case TransientSensitivity::Medium: threshold = 0.18f; minGapSec = 0.120; break;
        case TransientSensitivity::Fine:   threshold = 0.10f; minGapSec = 0.060; break;
    }
    const int minGapHops = juce::jmax (1, (int) std::round (minGapSec * sampleRate / hopSize));

    // Local-maximum peak picker over a small neighbourhood, then enforce
    // min-gap by suppressing weaker peaks within the window of stronger ones.
    const int neighbourhood = juce::jmax (1, minGapHops / 4);
    std::vector<int> peakHops;
    peakHops.reserve (64);
    for (int i = 1; i < numHops - 1; ++i)
    {
        const float v = flux[(size_t) i];
        if (v < threshold)
            continue;
        bool isPeak = true;
        const int lo = juce::jmax (1, i - neighbourhood);
        const int hi = juce::jmin (numHops - 1, i + neighbourhood);
        for (int j = lo; j <= hi; ++j)
        {
            if (j != i && flux[(size_t) j] > v)
            {
                isPeak = false;
                break;
            }
        }
        if (isPeak)
            peakHops.push_back (i);
    }

    // Enforce minimum gap by walking peaks and dropping any that fall too
    // close to the previously kept (stronger) peak.
    std::vector<int> kept;
    kept.reserve (peakHops.size());
    for (int idx : peakHops)
    {
        if (kept.empty() || idx - kept.back() >= minGapHops)
        {
            kept.push_back (idx);
        }
        else if (flux[(size_t) idx] > flux[(size_t) kept.back()])
        {
            kept.back() = idx;
        }
    }

    if (kept.empty())
        return 0;

    // Convert peak hops into sample positions, then build adjacent chops.
    std::vector<int> onsetSamples;
    onsetSamples.reserve (kept.size());
    for (int hop : kept)
        onsetSamples.push_back (juce::jlimit (0, totalSamples - 1, hop * hopSize));

    auto newChopState = std::make_shared<ChopState>();
    for (size_t i = 0; i < onsetSamples.size(); ++i)
    {
        const int startSample = onsetSamples[i];
        const int endSample   = (i + 1 < onsetSamples.size())
                                ? onsetSamples[i + 1]
                                : totalSamples;
        if (endSample <= startSample)
            continue;
        const auto autoCueStart  = findAutoCueStartSample (*currentSample, startSample, endSample);
        const auto autoCueOffset = juce::jlimit (0,
                                                 juce::jmax (0, endSample - startSample - 1),
                                                 autoCueStart - startSample);
        newChopState->chops.push_back ({ newChopState->nextChopId++, startSample, endSample,
                                         autoCueOffset, 0.0f, 0.0f, false, {} });
    }

    if (newChopState->chops.empty())
        return 0;

    pushEditUndoSnapshot ({});

    newChopState->selectedChopId = newChopState->chops.front().id;
    std::atomic_store (&chopState, newChopState);

    warpRenderThreadPool.removeAllJobs (false, 0);
    chopAudioCache.clear();

    touchTempoUiRevision();
    notifyEditStateChanged();
    return (int) newChopState->chops.size();
}

void AudioPluginAudioProcessor::selectChopAtSample (double samplePosition)
{
    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr)
        return;

    auto nextState = std::make_shared<ChopState> (*currentState);
    const auto* chop = findChopAtSample (currentState.get(), samplePosition);
    nextState->selectedChopId = chop != nullptr ? chop->id : -1;
    std::atomic_store (&chopState, nextState);
    touchTempoUiRevision();
    notifyEditStateChanged();
}

void AudioPluginAudioProcessor::clearSelectedChop()
{
    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr)
        return;

    auto nextState = std::make_shared<ChopState> (*currentState);
    nextState->selectedChopId = -1;
    std::atomic_store (&chopState, nextState);
    voices[activeVoiceIdx].playbackStopSample = -1.0;
    touchTempoUiRevision();
    notifyEditStateChanged();
}

void AudioPluginAudioProcessor::removeSelectedChop()
{
    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr || currentState->selectedChopId < 0)
        return;

    const auto removedChopId = currentState->selectedChopId;

    pushEditUndoSnapshot ({});

    auto nextState = std::make_shared<ChopState> (*currentState);
    nextState->chops.erase (std::remove_if (nextState->chops.begin(), nextState->chops.end(),
                                            [selectedChopId = currentState->selectedChopId] (const ChopDefinition& chop)
                                            {
                                                return chop.id == selectedChopId;
                                            }),
                            nextState->chops.end());
    nextState->selectedChopId = -1;
    std::atomic_store (&chopState, nextState);
    voices[activeVoiceIdx].playbackStopSample = -1.0;
    chopAudioCache.evict (removedChopId);
    touchTempoUiRevision();
    notifyEditStateChanged();
}

void AudioPluginAudioProcessor::setSelectedChopCueNormalized (float normalizedValue)
{
    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr || currentState->selectedChopId < 0)
        return;

    pushEditUndoSnapshot ("cue:" + juce::String (currentState->selectedChopId));

    auto nextState = std::make_shared<ChopState> (*currentState);
    for (auto& chop : nextState->chops)
    {
        if (chop.id != nextState->selectedChopId)
            continue;

        const auto availableLength = juce::jmax (1, chop.endSample - chop.startSample - 1);
        chop.cueOffsetSamples = juce::jlimit (0, availableLength,
                                              (int) std::round (juce::jlimit (0.0f, 1.0f, normalizedValue) * (float) availableLength));
        break;
    }

    std::atomic_store (&chopState, nextState);
    touchTempoUiRevision();
    notifyEditStateChanged();
}

void AudioPluginAudioProcessor::setSelectedChopGainDecibels (float gainDecibels)
{
    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr || currentState->selectedChopId < 0)
        return;

    pushEditUndoSnapshot ("gain:" + juce::String (currentState->selectedChopId));

    auto nextState = std::make_shared<ChopState> (*currentState);
    for (auto& chop : nextState->chops)
    {
        if (chop.id == nextState->selectedChopId)
        {
            chop.gainDecibels = juce::jlimit (-24.0f, 12.0f, gainDecibels);
            break;
        }
    }

    std::atomic_store (&chopState, nextState);
    touchTempoUiRevision();
    notifyEditStateChanged();
}

void AudioPluginAudioProcessor::setSelectedChopPitchSemitones (float newPitchSemitones)
{
    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr || currentState->selectedChopId < 0)
        return;

    pushEditUndoSnapshot ("pitch:" + juce::String (currentState->selectedChopId));

    auto nextState = std::make_shared<ChopState> (*currentState);
    for (auto& chop : nextState->chops)
    {
        if (chop.id == nextState->selectedChopId)
        {
            chop.pitchSemitones = juce::jlimit (-12.0f, 12.0f, newPitchSemitones);
            break;
        }
    }

    std::atomic_store (&chopState, nextState);
    touchTempoUiRevision();
    notifyEditStateChanged();
}

void AudioPluginAudioProcessor::touchTempoUiRevision() noexcept
{
    tempoUiRevision.fetch_add (1, std::memory_order_acq_rel);
}

void AudioPluginAudioProcessor::notifyEditStateChanged()
{
    editChangeBroadcaster.sendChangeMessage();
    requestHostStateSync();
}

void AudioPluginAudioProcessor::requestHostStateSync()
{
    triggerAsyncUpdate();
}

void AudioPluginAudioProcessor::handleAsyncUpdate()
{
    updateHostDisplay (AudioProcessor::ChangeDetails{}.withNonParameterStateChanged (true));
}

void AudioPluginAudioProcessor::pushEditUndoSnapshot (const juce::String& coalesceKey)
{
    const juce::ScopedLock sl (editUndoLock);

    const auto current = std::atomic_load (&chopState);
    if (current == nullptr)
        return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    // A non-empty key that matches the previous edit within the coalesce window
    // means we're mid-gesture (e.g. dragging a gain knob) — the pre-gesture
    // snapshot already sits on the stack, so don't add another.
    const bool coalesce = coalesceKey.isNotEmpty()
                          && ! editUndoStack.empty()
                          && coalesceKey == editUndoCoalesceKey
                          && (nowMs - editUndoLastSnapshotMs) < editUndoCoalesceWindowMs;

    editUndoLastSnapshotMs = nowMs;
    editUndoCoalesceKey = coalesceKey;

    if (coalesce)
        return;

    editUndoStack.push_back ({ current,
                               gridBpmTrim.load (std::memory_order_acquire),
                               gridStartOffset.load (std::memory_order_acquire),
                               chopBarsCount.load (std::memory_order_acquire) });

    if (editUndoStack.size() > maxEditUndoDepth)
        editUndoStack.erase (editUndoStack.begin());
}

void AudioPluginAudioProcessor::clearEditUndoHistory()
{
    const juce::ScopedLock sl (editUndoLock);
    editUndoStack.clear();
    editUndoCoalesceKey.clear();
}

bool AudioPluginAudioProcessor::canUndoEdit() const noexcept
{
    const juce::ScopedLock sl (editUndoLock);
    return ! editUndoStack.empty();
}

void AudioPluginAudioProcessor::undoLastEdit()
{
    EditUndoSnapshot snap;
    {
        const juce::ScopedLock sl (editUndoLock);
        if (editUndoStack.empty())
            return;

        snap = editUndoStack.back();
        editUndoStack.pop_back();
        editUndoCoalesceKey.clear(); // the next edit must begin a fresh undo step
    }

    gridBpmTrim.store (snap.gridBpmTrim, std::memory_order_release);
    gridStartOffset.store (snap.gridStartOffset, std::memory_order_release);
    chopBarsCount.store (snap.chopBarsCount, std::memory_order_release);

    std::atomic_store (&chopState, snap.chopState != nullptr ? snap.chopState
                                                            : std::make_shared<ChopState>());

    // Chop ids / bounds / markers may all differ now — drop the warp cache and
    // re-bake any chops that carry markers, mirroring the restore path.
    warpRenderThreadPool.removeAllJobs (false, 0);
    chopAudioCache.clear();
    if (const auto restored = std::atomic_load (&chopState); restored != nullptr)
        for (const auto& c : restored->chops)
            if (! c.warpMarkers.empty())
                requestChopWarpRender (c.id);

    touchTempoUiRevision();
    notifyEditStateChanged();
    sampleChangeBroadcaster.sendChangeMessage();
}

void AudioPluginAudioProcessor::toggleSelectedChopFavorite()
{
    const auto currentState = std::atomic_load (&chopState);
    if (currentState == nullptr || currentState->selectedChopId < 0)
        return;

    pushEditUndoSnapshot ({});

    auto nextState = std::make_shared<ChopState> (*currentState);
    for (auto& chop : nextState->chops)
    {
        if (chop.id == nextState->selectedChopId)
        {
            chop.favorite = ! chop.favorite;
            break;
        }
    }

    std::atomic_store (&chopState, nextState);
    touchTempoUiRevision();
    notifyEditStateChanged();
}
