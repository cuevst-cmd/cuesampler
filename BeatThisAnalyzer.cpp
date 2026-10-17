#include "BeatThisAnalyzer.h"

// Include ONNX Runtime C++ API
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

//==============================================================================
// Slaney's piecewise-linear/log mel scale, as used by torchaudio's
// melscale_fbanks(mel_scale="slaney", norm=None) in beat_this/preprocessing.py.
static double hzToMel (double hz) noexcept
{
    constexpr double linearStep = 200.0 / 3.0;
    const double logStep = std::log (6.4) / 27.0;
    return hz < 1000.0 ? hz / linearStep
                       : 15.0 + std::log (hz / 1000.0) / logStep;
}

static double melToHz (double mel) noexcept
{
    constexpr double linearStep = 200.0 / 3.0;
    const double logStep = std::log (6.4) / 27.0;
    return mel < 15.0 ? mel * linearStep
                      : 1000.0 * std::exp ((mel - 15.0) * logStep);
}

// PyTorch's reflect padding excludes the endpoint. Repeating that reflection
// also handles clips shorter than the 512-sample STFT padding (which PyTorch
// itself rejects). A single-sample clip has no neighbouring sample to reflect.
static int reflectedIndex (int64_t index, int numSamples) noexcept
{
    if (numSamples <= 1)
        return 0;

    const int64_t period = 2 * (int64_t) (numSamples - 1);
    index %= period;
    if (index < 0)
        index += period;
    return (int) (index < numSamples ? index : period - index);
}

//==============================================================================
BeatThisAnalyzer::BeatThisAnalyzer (const juce::String& onnxModelPath)
{
    buildMelFilters();

    try
    {
        ortEnv     = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "BeatThis");
        // Configure diagnostics before any model/session work, off the audio thread.
        ortEnv->DisableTelemetryEvents();
        ortOptions = std::make_unique<Ort::SessionOptions>();
        ortOptions->SetIntraOpNumThreads (1);
        ortOptions->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Load from file path. ONNX takes the path as ORTCHAR_T*: that is
        // wchar_t* on Windows but char* elsewhere, so build the matching owned
        // string. path.c_str() then has the correct type on each platform.
       #ifdef _WIN32
        const std::wstring path (onnxModelPath.toWideCharPointer());
       #else
        const std::string path = onnxModelPath.toStdString();
       #endif
        ortSession = std::make_unique<Ort::Session> (*ortEnv, path.c_str(), *ortOptions);
        ortRunOptions = std::make_unique<Ort::RunOptions>();
        sessionReady.store (true);
    }
    catch (const Ort::Exception& e)
    {
        juce::Logger::writeToLog ("BeatThisAnalyzer: ONNX load failed: " + juce::String::fromUTF8 (e.what()));
        sessionReady.store (false);
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog ("BeatThisAnalyzer: init error: " + juce::String::fromUTF8 (e.what()));
        sessionReady.store (false);
    }
}

BeatThisAnalyzer::~BeatThisAnalyzer() = default;

//==============================================================================
void BeatThisAnalyzer::buildMelFilters()
{
    const int numFftBins = kFftSize / 2 + 1;

    // Create numMel+2 points linearly spaced in Mel space between fMin and fMax
    const int numPoints = kMelBins + 2;
    const double melMin = hzToMel (kFreqMin);
    const double melMax = hzToMel (kFreqMax);

    std::vector<double> melPoints (numPoints);
    for (int i = 0; i < numPoints; ++i)
        melPoints[i] = melMin + (melMax - melMin) * (double) i / (double) (numPoints - 1);

    // Preserve the continuous frequency coordinates: rounding triangle edges
    // onto FFT bins changes the lowest mel bands particularly strongly.
    std::vector<double> hzPoints (numPoints);
    for (int i = 0; i < numPoints; ++i)
        hzPoints[(size_t) i] = melToHz (melPoints[(size_t) i]);

    // Build sparse triangular filters — only store the non-zero bin range per band
    melFilters.resize ((size_t) kMelBins);

    for (int m = 0; m < kMelBins; ++m)
    {
        const double left   = hzPoints[(size_t) m];
        const double centre = hzPoints[(size_t) m + 1];
        const double right  = hzPoints[(size_t) m + 2];
        const double binHz  = kTargetSampleRate / kFftSize;
        const int firstBin  = juce::jlimit (0, numFftBins - 1, (int) std::ceil (left / binHz));
        const int lastBin   = juce::jlimit (firstBin, numFftBins - 1, (int) std::floor (right / binHz));

        melFilters[(size_t) m].startBin = firstBin;
        melFilters[(size_t) m].weights.assign ((size_t) (lastBin - firstBin + 1), 0.0f);
        auto& w = melFilters[(size_t) m].weights;

        for (int bin = firstBin; bin <= lastBin; ++bin)
        {
            const double hz = bin * binHz;
            w[(size_t) (bin - firstBin)] = (float) std::max (0.0, std::min (
                (hz - left) / (centre - left), (right - hz) / (right - centre)));
        }
    }
}

//==============================================================================
std::vector<float> BeatThisAnalyzer::resampleToMono (const juce::AudioBuffer<float>& buffer,
                                                      double sourceSampleRate,
                                                      int startSample,
                                                      int endSample) const
{
    const int numChannels = buffer.getNumChannels();
    startSample = juce::jlimit (0, buffer.getNumSamples(), startSample);
    endSample   = juce::jlimit (startSample, buffer.getNumSamples(), endSample);
    const int numSrcSamples = endSample - startSample;

    if (numSrcSamples <= 0 || numChannels <= 0 || sourceSampleRate <= 0.0
        || ! std::isfinite (sourceSampleRate))
        return {};

    const double ratio = kTargetSampleRate / sourceSampleRate;
    const double outputLength = std::round ((double) numSrcSamples * ratio);
    if (! std::isfinite (outputLength) || outputLength > std::numeric_limits<int>::max())
        return {};

    // Avoid filtering or changing samples when no conversion is required.
    if (sourceSampleRate == kTargetSampleRate)
    {
        std::vector<float> mono ((size_t) numSrcSamples, 0.0f);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* channel = buffer.getReadPointer (ch, startSample);
            for (int i = 0; i < numSrcSamples; ++i)
                mono[(size_t) i] += channel[i] / (float) numChannels;
        }
        return mono;
    }

    // Offline band-limited resampling. A cutoff below the lower Nyquist leaves
    // room for the Blackman transition band; scaling its width when downsampling
    // prevents high-frequency content aliasing into the model's beat features.
    // The phase table avoids evaluating trigonometry for every output sample.
    // This is deliberately independent of any host/audio-thread resampler.
    constexpr int numPhases = 512;
    const double cutoff = 0.95 * std::min (1.0, ratio);
    const double requiredHalfWidth = std::ceil (64.0 / cutoff);
    if (requiredHalfWidth > 8192.0) // Reject impractical/invalid source rates.
        return {};
    const int halfWidth = (int) requiredHalfWidth;
    const int numTaps = 2 * halfWidth + 1;
    std::vector<float> kernels ((size_t) (numPhases + 1) * (size_t) numTaps);

    for (int phase = 0; phase <= numPhases; ++phase)
    {
        auto* kernel = kernels.data() + (size_t) phase * (size_t) numTaps;
        const double fraction = (double) phase / numPhases;
        double sum = 0.0;
        for (int tap = 0; tap < numTaps; ++tap)
        {
            const double distance = (double) (tap - halfWidth) - fraction;
            const double x = distance / halfWidth;
            const double angle = juce::MathConstants<double>::pi * cutoff * distance;
            const double sinc = std::abs (angle) < 1.0e-12 ? 1.0 : std::sin (angle) / angle;
            const double window = std::abs (x) >= 1.0 ? 0.0
                : 0.42 + 0.5 * std::cos (juce::MathConstants<double>::pi * x)
                       + 0.08 * std::cos (juce::MathConstants<double>::twoPi * x);
            kernel[tap] = (float) (cutoff * sinc * window);
            sum += kernel[tap];
        }
        for (int tap = 0; tap < numTaps; ++tap)
            kernel[tap] = (float) (kernel[tap] / sum);
    }

    // Downmix once, rather than averaging channels inside every filter tap.
    // Reflection supplies complete filter support without delaying the result.
    std::vector<float> source ((size_t) numSrcSamples + 2 * (size_t) halfWidth, 0.0f);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto* channel = buffer.getReadPointer (ch, startSample);
        for (int i = 0; i < numSrcSamples; ++i)
            source[(size_t) halfWidth + (size_t) i] += channel[i] / (float) numChannels;
    }
    for (int i = 0; i < halfWidth; ++i)
    {
        source[(size_t) i] = source[(size_t) halfWidth
            + (size_t) reflectedIndex ((int64_t) i - halfWidth, numSrcSamples)];
        source[(size_t) halfWidth + (size_t) numSrcSamples + (size_t) i]
            = source[(size_t) halfWidth
                + (size_t) reflectedIndex ((int64_t) numSrcSamples + i, numSrcSamples)];
    }

    const int numDstSamples = juce::jmax (1, (int) outputLength);
    std::vector<float> mono ((size_t) numDstSamples);
    for (int dstIdx = 0; dstIdx < numDstSamples; ++dstIdx)
    {
        const double position = (double) dstIdx / ratio;
        const int sourceIndex = std::min ((int) position, numSrcSamples - 1);
        const double phasePosition = (position - sourceIndex) * numPhases;
        const int phase = std::min ((int) phasePosition, numPhases - 1);
        const float blend = (float) (phasePosition - phase);
        const auto* kernel = kernels.data() + (size_t) phase * (size_t) numTaps;
        const auto* nextKernel = kernel + numTaps;
        const auto* samples = source.data() + sourceIndex;
        double sum = 0.0;
        for (int tap = 0; tap < numTaps; ++tap)
        {
            const float weight = kernel[tap] + blend * (nextKernel[tap] - kernel[tap]);
            sum += samples[tap] * weight;
        }
        mono[(size_t) dstIdx] = (float) sum;
    }

    return mono;
}

//==============================================================================
std::vector<float>
BeatThisAnalyzer::computeMelSpectrogram (const std::vector<float>& mono) const
{
    if (mono.empty())
        return {};

    // torch.hann_window defaults to periodic=True. Its denominator is n_fft,
    // rather than n_fft - 1 (the latter produces a symmetric analysis window).
    std::vector<float> window ((size_t) kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[(size_t) i] = (float) (0.5 * (1.0 - std::cos (
            juce::MathConstants<double>::twoPi * (double) i / kFftSize)));

    juce::dsp::FFT fft (static_cast<int> (std::log2 (kFftSize)));

    const int halfWin     = kFftSize / 2;
    const int totalSamples = (int) mono.size();
    const int numFrames = 1 + totalSamples / kHopSize;
    const float fftNormalisation = 1.0f / std::sqrt ((float) kFftSize);

    // Flat output: [numFrames * kMelBins] — avoids per-frame heap allocation
    std::vector<float> flat;
    flat.reserve ((size_t) numFrames * (size_t) kMelBins);

    std::vector<float> fftBuf ((size_t) kFftSize * 2);

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int centre = frame * kHopSize;
        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);

        for (int i = 0; i < kFftSize; ++i)
        {
            const int srcIdx = reflectedIndex ((int64_t) centre - halfWin + i, totalSamples);
            fftBuf[(size_t) i] = mono[(size_t) srcIdx] * window[(size_t) i];
        }

        fft.performFrequencyOnlyForwardTransform (fftBuf.data());

        // Apply sparse triangular filters — only iterate the non-zero bin range
        for (int m = 0; m < kMelBins; ++m)
        {
            const auto& filt = melFilters[(size_t) m];
            float energy = 0.0f;
            const int numWeights = (int) filt.weights.size();
            for (int k = 0; k < numWeights; ++k)
                energy += filt.weights[(size_t) k] * fftBuf[(size_t) (filt.startBin + k)];

            // normalized="frame_length", power=1: normalize FFT magnitudes by
            // sqrt(n_fft), and apply no Slaney area normalization to the bank.
            flat.push_back (std::log1p (kLogScale * fftNormalisation * juce::jmax (0.0f, energy)));
        }
    }

    return flat;
}

//==============================================================================
BeatThisAnalyzer::BeatDownbeatProbs
BeatThisAnalyzer::runOnnxInferenceChunk (const std::vector<float>& flatMel,
                                         int totalFrames,
                                         int startFrame,
                                         int endFrame) const
{
    if (startFrame < 0 || endFrame <= startFrame || endFrame > totalFrames
        || flatMel.size() != (size_t) totalFrames * kMelBins)
        return {};

    const int64_t batchSize = 1;
    const int64_t numFrames = (int64_t) (endFrame - startFrame);
    const int64_t melBins   = kMelBins;

    // Slice directly from the flat buffer — no copy if whole file, subspan otherwise
    const float* srcPtr = flatMel.data() + (size_t) startFrame * (size_t) kMelBins;
    const size_t srcLen = (size_t) (numFrames * melBins);

    const std::array<int64_t, 3> inputShape { batchSize, numFrames, melBins };
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor  = Ort::Value::CreateTensor<float> (
        memInfo, const_cast<float*> (srcPtr), srcLen, inputShape.data(), inputShape.size());

    const char* inputName    = "mel_spectrogram";
    const char* beatName     = "beat";
    const char* downbeatName = "downbeat";

    std::vector<Ort::Value> outputTensors;
    try
    {
        const std::array<const char*, 2> outputNames { beatName, downbeatName };
        outputTensors = ortSession->Run (
            *ortRunOptions,
            &inputName, &inputTensor, 1,
            outputNames.data(), outputNames.size());
    }
    catch (const Ort::Exception& e)
    {
        juce::Logger::writeToLog ("BeatThisAnalyzer: inference error: " + juce::String::fromUTF8 (e.what()));
        return {};
    }

    auto extractProbs = [&] (int idx) -> std::vector<float>
    {
        const auto info = outputTensors[(size_t) idx].GetTensorTypeAndShapeInfo();
        const auto shape = info.GetShape();
        if (shape.size() != 2 || shape[0] != 1 || shape[1] != numFrames
            || info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            return {};
        const float* raw = outputTensors[(size_t) idx].GetTensorData<float>();
        std::vector<float> probs ((size_t) numFrames);
        for (int64_t i = 0; i < numFrames; ++i)
            probs[(size_t) i] = sigmoid (raw[i]);
        return probs;
    };

    return { extractProbs (0), extractProbs (1) };
}

//==============================================================================
BeatThisAnalyzer::BeatDownbeatProbs
BeatThisAnalyzer::runOnnxInference (const std::vector<float>& flatMel, int numFrames) const
{
    if (! sessionReady.load() || numFrames <= 0
        || flatMel.size() != (size_t) numFrames * kMelBins)
        return {};

    // Match Spect2Frames: 30-second chunks, six context frames on each side,
    // and keep the first prediction in overlaps. Discarding only one side of
    // an overlap would insert frames and shift every later beat timestamp.
    constexpr int chunkFrames = 1500;
    constexpr int borderFrames = 6;
    constexpr int stepFrames = chunkFrames - 2 * borderFrames;
    std::vector<int> starts;
    for (int start = -borderFrames; start < numFrames - borderFrames; start += stepFrames)
        starts.push_back (start);
    if (numFrames > stepFrames)
        starts.back() = numFrames - (chunkFrames - borderFrames);

    BeatDownbeatProbs merged;
    merged.beat.resize ((size_t) numFrames);
    merged.downbeat.resize ((size_t) numFrames);
    int writtenUntil = 0;
    for (const int start : starts)
    {
        const int sourceStart = juce::jmax (0, start);
        const int sourceEnd = juce::jmin (numFrames, start + chunkFrames);
        const int leftPad = juce::jmax (0, -start);
        const int rightPad = juce::jlimit (0, borderFrames, start + chunkFrames - numFrames);
        const int inputFrames = leftPad + sourceEnd - sourceStart + rightPad;
        std::vector<float> padded ((size_t) inputFrames * kMelBins, 0.0f);
        std::copy (flatMel.begin() + (size_t) sourceStart * kMelBins,
                   flatMel.begin() + (size_t) sourceEnd * kMelBins,
                   padded.begin() + (size_t) leftPad * kMelBins);
        const auto chunk = runOnnxInferenceChunk (padded, inputFrames, 0, inputFrames);
        if (chunk.beat.size() != (size_t) inputFrames
            || chunk.downbeat.size() != (size_t) inputFrames)
            return {};

        const int keepStart = juce::jmax (writtenUntil, start + borderFrames);
        const int keepEnd = juce::jmin (numFrames, start + inputFrames - borderFrames);
        for (int frame = keepStart; frame < keepEnd; ++frame)
        {
            merged.beat[(size_t) frame] = chunk.beat[(size_t) (frame - start)];
            merged.downbeat[(size_t) frame] = chunk.downbeat[(size_t) (frame - start)];
        }
        writtenUntil = keepEnd;
    }
    return writtenUntil == numFrames ? merged : BeatDownbeatProbs {};
}

//==============================================================================
std::vector<int> BeatThisAnalyzer::peakPick (const std::vector<float>& activations,
                                           float threshold,
                                           int radiusFrames)
{
    std::vector<int> peaks;
    for (int i = 0; i < (int) activations.size(); ++i)
    {
        const auto value = activations[(size_t) i];
        if (! std::isfinite (value) || value <= threshold)
            continue;

        bool isMaximum = true;
        for (int j = juce::jmax (0, i - radiusFrames);
             j <= juce::jmin ((int) activations.size() - 1, i + radiusFrames); ++j)
            if (activations[(size_t) j] > value)
                isMaximum = false;
        if (! isMaximum)
            continue;

        // Collapse a flat-topped peak to its centre, as in the reference
        // minimal postprocessor, rather than treating each frame as a beat.
        int plateauEnd = i;
        while (plateauEnd + 1 < (int) activations.size()
               && activations[(size_t) plateauEnd + 1] == value)
            ++plateauEnd;
        peaks.push_back ((i + plateauEnd) / 2);
        i = plateauEnd;
    }
    return peaks;
}

BeatThisAnalyzer::Result BeatThisAnalyzer::decodeBeatProbabilities (
    const std::vector<float>& beatProbs,
    const std::vector<float>& downbeatProbs,
    double analysisStartSecs)
{
    Result result;
    if (! std::isfinite (analysisStartSecs)
        || std::any_of (beatProbs.begin(), beatProbs.end(),
                        [] (float p) { return ! std::isfinite (p); }))
        return result;

    // Beat This was trained to work with minimal peak picking. Do not force
    // its predictions through a preferred tempo or infer beats by dividing
    // downbeat spacing by four: both change the metrical interpretation.
    const auto beatFrames = peakPick (beatProbs, kBeatThreshold, 3);
    if (beatFrames.size() < 4)
        return result;

    result.beatPositionsSecs.reserve (beatFrames.size());
    double strength = 0.0;
    for (const int frame : beatFrames)
    {
        double position = (double) frame;
        if (frame > 0 && frame + 1 < (int) beatProbs.size())
        {
            const double left = beatProbs[(size_t) frame - 1];
            const double centre = beatProbs[(size_t) frame];
            const double right = beatProbs[(size_t) frame + 1];
            const double curvature = left - 2.0 * centre + right;
            if (curvature < -1.0e-6)
                position += juce::jlimit (-0.5, 0.5, 0.5 * (left - right) / curvature);
        }
        result.beatPositionsSecs.push_back (analysisStartSecs + position / kFps);
        strength += (double) beatProbs[(size_t) frame];
    }

    std::vector<double> intervals;
    intervals.reserve (beatFrames.size() - 1);
    for (size_t i = 1; i < result.beatPositionsSecs.size(); ++i)
    {
        const auto interval = result.beatPositionsSecs[i] - result.beatPositionsSecs[i - 1];
        if (interval >= 0.15 && interval <= 3.0)
            intervals.push_back (interval);
    }
    if (intervals.size() < 3)
        return Result {};

    auto sorted = intervals;
    std::sort (sorted.begin(), sorted.end());
    const size_t middle = sorted.size() / 2;
    // Pick an observed median. Averaging two separated tempo modes can land
    // in a gap containing no actual intervals and invalidate a tempo change.
    const double median = sorted[middle];
    double sum = 0.0;
    int inlierCount = 0;
    for (const auto interval : intervals)
        if (std::abs (interval - median) <= median * 0.2)
        {
            sum += interval;
            ++inlierCount;
        }
    if (inlierCount < 3)
        return Result {};

    result.beatPeriodSecs = sum / (double) inlierCount;
    result.estimatedBpm = 60.0 / result.beatPeriodSecs;
    result.firstBeatSecs = result.beatPositionsSecs.front();
    const double evidence = juce::jmin (1.0, (double) intervals.size() / 8.0);
    const double continuity = (double) inlierCount / (double) (beatFrames.size() - 1);
    const double intervalCoverage = (double) intervals.size() / (double) (beatFrames.size() - 1);
    result.confidence = (float) juce::jlimit (0.0, 1.0,
        strength / (double) beatFrames.size() * evidence * intervalCoverage * (0.75 + 0.25 * continuity));

    // Four-interval averages distinguish sustained tempo movement from
    // frame quantisation and alternating early/late expressive beat timing.
    double localVariance = 0.0;
    int localCount = 0;
    for (size_t i = 4; i < result.beatPositionsSecs.size(); ++i)
    {
        const auto localPeriod = (result.beatPositionsSecs[i] - result.beatPositionsSecs[i - 4]) / 4.0;
        localVariance += std::pow (localPeriod / result.beatPeriodSecs - 1.0, 2.0);
        ++localCount;
    }
    result.likelyDrifting = localCount > 0 && std::sqrt (localVariance / (double) localCount) > 0.03;

    // Preserve observed downbeats, including triple/odd meters. The legacy
    // phase remains modulo four for the existing fixed four-beat chop UI;
    // it is never used to infer BPM or synthesize additional bar markers.
    for (const int downbeat : peakPick (downbeatProbs, kBeatThreshold, 3))
    {
        const auto nearest = std::min_element (beatFrames.begin(), beatFrames.end(),
            [downbeat] (int a, int b) { return std::abs (a - downbeat) < std::abs (b - downbeat); });
        if (std::abs (*nearest - downbeat) > 5)
            continue;
        const auto beatIndex = (size_t) std::distance (beatFrames.begin(), nearest);
        const auto time = result.beatPositionsSecs[beatIndex];
        if (result.barPositionsSecs.empty())
            result.downbeatPhase = (int) (beatIndex % 4);
        if (result.barPositionsSecs.empty() || time > result.barPositionsSecs.back())
            result.barPositionsSecs.push_back (time);
    }
    result.valid = true;
    return result;
}

//==============================================================================
float BeatThisAnalyzer::sigmoid (float x) noexcept
{
    return 1.0f / (1.0f + std::exp (-x));
}

//==============================================================================
BeatThisAnalyzer::Result
BeatThisAnalyzer::analyze (const juce::AudioBuffer<float>& buffer,
                            double sampleRate,
                            int startSample,
                            int endSample) const
{
    Result result;

    if (! sessionReady.load())
        return result;

    const int totalSamples = buffer.getNumSamples();
    if (totalSamples <= 0 || sampleRate <= 0.0)
        return result;

    const int start = juce::jmax (0, startSample);
    const int end   = (endSample < 0) ? totalSamples : juce::jmin (endSample, totalSamples);

    if (end <= start)
        return result;

    BeatDownbeatProbs probs;
    {
        // Keep the large resampled waveform and mel tensor scoped tightly so they
        // do not overlap with later peak-picking allocations.
        const auto mono = resampleToMono (buffer, sampleRate, start, end);
        if (mono.empty())
            return result;

        const auto flatMel = computeMelSpectrogram (mono);
        const int numMelFrames = (int) flatMel.size() / kMelBins;
        if (numMelFrames < 10)
            return result;

        probs = runOnnxInference (flatMel, numMelFrames);
    }

    return decodeBeatProbabilities (probs.beat, probs.downbeat, (double) start / sampleRate);
}

//==============================================================================
void BeatThisAnalyzer::terminate() const
{
    if (sessionReady.load() && ortRunOptions)
    {
        try {
            ortRunOptions->SetTerminate();
        } catch (...) {}
    }
}
