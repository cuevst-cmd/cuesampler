#include "KeyDetector.h"

#include <cmath>
#include <limits>

namespace
{
constexpr int fftOrder = 13;
constexpr int fftSize = 1 << fftOrder;
constexpr int hopSize = fftSize / 4;
constexpr double minFrequencyHz = 65.0;
constexpr double maxFrequencyHz = 2093.0;
constexpr double analysisSeconds = 30.0; // analyze a longer span; 6s often catches only an ambiguous intro
constexpr double minAudioSeconds = 0.5;

constexpr std::array<const char*, 12> noteNames
{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

constexpr std::array<const char*, 12> majorCamelot
{
    "8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B"
};

constexpr std::array<const char*, 12> minorCamelot
{
    "5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A"
};

// Returns the positive modulo value for pitch-class indexing.
int wrapPitchClass (int value)
{
    auto wrapped = value % 12;

    if (wrapped < 0)
        wrapped += 12;

    return wrapped;
}
}

// Temperley (2001) key profiles. These outperform the original Krumhansl-Kessler
// profiles on real recordings and are published research values (no GPL/AGPL
// library code involved). The correlate() step is scale-invariant, so only the
// shape of these profiles matters.
const std::array<double, 12> KeyDetector::majorProfile
{
    0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400
};

const std::array<double, 12> KeyDetector::minorProfile
{
    0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330
};

// Detects the most likely musical key from the supplied audio buffer.
KeyDetector::Result KeyDetector::detect (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    Result result;

    if (buffer.getNumChannels() <= 0 || sampleRate <= 0.0)
        return result;

    if (static_cast<double> (buffer.getNumSamples()) < minAudioSeconds * sampleRate)
        return result;

    const auto chroma = buildChromagram (buffer, sampleRate);

    double chromaSum = 0.0;
    for (auto value : chroma)
        chromaSum += value;

    if (chromaSum <= 0.0)
        return result;

    double bestScore = -std::numeric_limits<double>::infinity();
    double secondBestScore = -std::numeric_limits<double>::infinity();
    int bestRoot = 0;
    bool bestIsMajor = true;

    for (int shift = 0; shift < 12; ++shift)
    {
        const auto majorScore = correlate (chroma, majorProfile, shift);
        if (majorScore > bestScore)
        {
            secondBestScore = bestScore;
            bestScore = majorScore;
            bestRoot = shift;
            bestIsMajor = true;
        }
        else if (majorScore > secondBestScore)
        {
            secondBestScore = majorScore;
        }

        const auto minorScore = correlate (chroma, minorProfile, shift);
        if (minorScore > bestScore)
        {
            secondBestScore = bestScore;
            bestScore = minorScore;
            bestRoot = shift;
            bestIsMajor = false;
        }
        else if (minorScore > secondBestScore)
        {
            secondBestScore = minorScore;
        }
    }

    auto confidence = 0.0;
    if (std::isfinite (bestScore) && std::isfinite (secondBestScore))
    {
        confidence = (bestScore - secondBestScore) * 0.5;

        if (confidence < 0.0)
            confidence = 0.0;

        if (confidence > 1.0)
            confidence = 1.0;
    }

    result.valid = true;
    result.rootIndex = bestRoot;
    result.rootNote = noteNames[static_cast<size_t> (bestRoot)];
    result.isMajor = bestIsMajor;
    result.key = result.rootNote + (bestIsMajor ? std::string() : std::string ("m"));
    result.confidence = static_cast<float> (confidence);
    result.camelot = bestIsMajor ? majorCamelot[static_cast<size_t> (bestRoot)]
                                 : minorCamelot[static_cast<size_t> (bestRoot)];

    return result;
}

// Builds a fully-populated Result for an explicit key (user override).
KeyDetector::Result KeyDetector::makeResult (int rootIndex, bool isMajor)
{
    Result result;

    const auto root = wrapPitchClass (rootIndex);
    result.valid      = true;
    result.rootIndex  = root;
    result.rootNote   = noteNames[static_cast<size_t> (root)];
    result.isMajor    = isMajor;
    result.key        = result.rootNote + (isMajor ? std::string() : std::string ("m"));
    result.confidence = 1.0f;
    result.camelot    = isMajor ? majorCamelot[static_cast<size_t> (root)]
                                : minorCamelot[static_cast<size_t> (root)];
    return result;
}

// Builds a normalized chromagram from windowed FFT magnitudes.
std::array<double, 12> KeyDetector::buildChromagram (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    std::array<double, 12> chroma {};

    if (buffer.getNumChannels() <= 0 || buffer.getNumSamples() < fftSize || sampleRate <= 0.0)
        return chroma;

    juce::dsp::FFT fft (fftOrder);
    std::array<float, fftSize * 2> fftData {};
    std::array<float, fftSize> window {};

    for (int i = 0; i < fftSize; ++i)
    {
        const auto phase = juce::MathConstants<double>::twoPi * static_cast<double> (i) / static_cast<double> (fftSize - 1);
        window[static_cast<size_t> (i)] = static_cast<float> (0.5 * (1.0 - std::cos (phase)));
    }

    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();
    const auto frameCap = static_cast<int> ((analysisSeconds * sampleRate) / static_cast<double> (hopSize));
    auto frameCount = 0;

    for (int start = 0; start + fftSize <= numSamples && frameCount < frameCap; start += hopSize)
    {
        fftData.fill (0.0f);

        for (int i = 0; i < fftSize; ++i)
        {
            double monoSample = 0.0;

            for (int channel = 0; channel < numChannels; ++channel)
                monoSample += static_cast<double> (buffer.getSample (channel, start + i));

            monoSample /= static_cast<double> (numChannels);
            fftData[static_cast<size_t> (i)] = static_cast<float> (monoSample) * window[static_cast<size_t> (i)];
        }

        fft.performFrequencyOnlyForwardTransform (fftData.data());

        for (int bin = 1; bin < fftSize / 2; ++bin)
        {
            const auto frequency = static_cast<double> (bin) * sampleRate / static_cast<double> (fftSize);

            if (frequency < minFrequencyHz || frequency > maxFrequencyHz)
                continue;

            const auto midi = (12.0 * std::log2 (frequency / 440.0)) + 69.0;
            const auto pitchClass = wrapPitchClass (static_cast<int> (std::lround (midi)));
            const auto magnitude = static_cast<double> (fftData[static_cast<size_t> (bin)]);

            chroma[static_cast<size_t> (pitchClass)] += magnitude;
        }

        ++frameCount;
    }

    double chromaSum = 0.0;
    for (auto value : chroma)
        chromaSum += value;

    if (chromaSum <= 0.0)
        return chroma;

    for (auto& value : chroma)
        value /= chromaSum;

    return chroma;
}

// Computes Pearson correlation between chroma values and a rotated profile.
double KeyDetector::correlate (const std::array<double, 12>& chroma, const std::array<double, 12>& profile, int shift)
{
    double chromaMean = 0.0;
    double profileMean = 0.0;

    for (int i = 0; i < 12; ++i)
    {
        chromaMean += chroma[static_cast<size_t> (i)];
        profileMean += profile[static_cast<size_t> (i)];
    }

    chromaMean /= 12.0;
    profileMean /= 12.0;

    double numerator = 0.0;
    double chromaVariance = 0.0;
    double profileVariance = 0.0;

    for (int i = 0; i < 12; ++i)
    {
        const auto chromaValue = chroma[static_cast<size_t> (i)] - chromaMean;
        const auto profileValue = profile[static_cast<size_t> (wrapPitchClass (i - shift))] - profileMean;

        numerator += chromaValue * profileValue;
        chromaVariance += chromaValue * chromaValue;
        profileVariance += profileValue * profileValue;
    }

    const auto denominator = std::sqrt (chromaVariance * profileVariance);
    return denominator > 0.0 ? numerator / denominator : 0.0;
}
