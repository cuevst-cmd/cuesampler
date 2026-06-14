#include "KeyDetector.h"

#include <algorithm>
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

// Log-magnitude compression strength. The spectrum is normalized to its global
// peak (so this is loudness-independent), then each bin contributes
// log1p(gamma * mag/peak). This whitens the chroma so loud partials and bass
// notes no longer dominate the linear sum. Larger gamma => more compression.
constexpr double logCompressionGamma = 1000.0;

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

// EDMA key profiles (Faraldo et al., "Key Estimation in Electronic Dance
// Music", 2016). These are corpus-derived from a large Beatport set and
// outperform the classical Krumhansl/Temperley profiles on electronic and
// modern produced material — the right fit for a sampler aimed at beatmakers.
// They are published research values (no GPL/AGPL library code involved), like
// the Temperley profiles used previously. Index 0 is the tonic; the correlate()
// step is mean/scale-invariant, so only the shape matters.
const std::array<double, 12> KeyDetector::majorProfile
{
    1.0000, 0.2875, 0.5020, 0.4048, 0.6050, 0.5614, 0.3205, 0.7966, 0.3159, 0.4506, 0.4202, 0.3889
};

const std::array<double, 12> KeyDetector::minorProfile
{
    1.0000, 0.3096, 0.4415, 0.5827, 0.3262, 0.4948, 0.2889, 0.7804, 0.4328, 0.2903, 0.5331, 0.3217
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

    // Confidence combines two factors, each clamped to [0, 1]:
    //   tonality  — how well the winning key actually fits (a percussive or
    //               atonal sample correlates poorly with every profile);
    //   separation — how far the winner leads the runner-up (a ~0.10 Pearson
    //               gap is treated as decisive). Note that genuine relative
    //               major/minor and dominant ambiguities have small gaps, so
    //               this honestly reports lower confidence for those cases.
    auto confidence = 0.0;
    if (std::isfinite (bestScore) && std::isfinite (secondBestScore))
    {
        const auto tonality   = std::clamp (bestScore, 0.0, 1.0);
        const auto separation = std::clamp ((bestScore - secondBestScore) / 0.10, 0.0, 1.0);
        confidence = tonality * separation;
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

// Builds a normalized, tuning-corrected, spectrally-whitened chromagram.
//
// Two passes over the analysis span (FFT is cheap and this runs once per file
// on a background thread):
//   Pass 1 — find the global magnitude peak (for loudness-independent log
//            compression) and estimate the global tuning offset, so material
//            that isn't at A440 (pitched samples, off-tune recordings) doesn't
//            smear energy across neighbouring pitch classes.
//   Pass 2 — accumulate log-compressed, tuning-corrected magnitudes into the
//            12 pitch classes.
std::array<double, 12> KeyDetector::buildChromagram (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    std::array<double, 12> chroma {};

    if (buffer.getNumChannels() <= 0 || buffer.getNumSamples() < fftSize || sampleRate <= 0.0)
        return chroma;

    juce::dsp::FFT fft (fftOrder);
    std::array<float, fftSize> window {};

    for (int i = 0; i < fftSize; ++i)
    {
        const auto phase = juce::MathConstants<double>::twoPi * static_cast<double> (i) / static_cast<double> (fftSize - 1);
        window[static_cast<size_t> (i)] = static_cast<float> (0.5 * (1.0 - std::cos (phase)));
    }

    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();
    const auto frameCap = static_cast<int> ((analysisSeconds * sampleRate) / static_cast<double> (hopSize));

    // Runs the windowed-FFT frame loop, invoking binFn(frequency, magnitude)
    // for every in-band bin of every frame.
    const auto forEachBin = [&] (auto&& binFn)
    {
        std::array<float, fftSize * 2> fftData {};
        auto frameCount = 0;

        for (int start = 0; start + fftSize <= numSamples && frameCount < frameCap; start += hopSize, ++frameCount)
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

                binFn (frequency, static_cast<double> (fftData[static_cast<size_t> (bin)]));
            }
        }
    };

    constexpr auto twoPi = juce::MathConstants<double>::twoPi;

    // Pass 1: peak magnitude + tuning offset (magnitude-weighted circular mean
    // of each bin's deviation from the nearest equal-tempered semitone).
    double globalMaxMag = 0.0;
    double tuningCos = 0.0;
    double tuningSin = 0.0;

    forEachBin ([&] (double frequency, double magnitude)
    {
        globalMaxMag = std::max (globalMaxMag, magnitude);

        const auto midi = (12.0 * std::log2 (frequency / 440.0)) + 69.0;
        const auto deviation = midi - std::round (midi); // semitones in [-0.5, 0.5]
        tuningCos += magnitude * std::cos (twoPi * deviation);
        tuningSin += magnitude * std::sin (twoPi * deviation);
    });

    if (globalMaxMag <= 0.0)
        return chroma;

    const auto tuningOffset = (tuningCos != 0.0 || tuningSin != 0.0)
                                ? std::atan2 (tuningSin, tuningCos) / twoPi // semitones in [-0.5, 0.5]
                                : 0.0;

    // Pass 2: spectrally-whitened, tuning-corrected accumulation.
    forEachBin ([&] (double frequency, double magnitude)
    {
        const auto weight = std::log1p (logCompressionGamma * magnitude / globalMaxMag);
        const auto midi = (12.0 * std::log2 (frequency / 440.0)) + 69.0 - tuningOffset;
        const auto pitchClass = wrapPitchClass (static_cast<int> (std::lround (midi)));

        chroma[static_cast<size_t> (pitchClass)] += weight;
    });

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
