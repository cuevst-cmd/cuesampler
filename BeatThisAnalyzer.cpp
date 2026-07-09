#include "BeatThisAnalyzer.h"

// Include ONNX Runtime C++ API
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <numeric>

//==============================================================================
// Mel scale conversion helpers
static double hzToMel (double hz) noexcept
{
    return 2595.0 * std::log10 (1.0 + hz / 700.0);
}

static double melToHz (double mel) noexcept
{
    return 700.0 * (std::pow (10.0, mel / 2595.0) - 1.0);
}

//==============================================================================
BeatThisAnalyzer::BeatThisAnalyzer (const juce::String& onnxModelPath)
{
    buildMelFilters();

    try
    {
        ortEnv     = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "BeatThis");
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

    // Convert back to Hz then to FFT bin index
    std::vector<int> binPoints (numPoints);
    for (int i = 0; i < numPoints; ++i)
    {
        const double hz  = melToHz (melPoints[i]);
        const double bin = hz * (double) kFftSize / kTargetSampleRate;
        binPoints[i]     = juce::jlimit (0, numFftBins - 1, (int) std::round (bin));
    }

    // Build sparse triangular filters — only store the non-zero bin range per band
    melFilters.resize ((size_t) kMelBins);

    for (int m = 0; m < kMelBins; ++m)
    {
        const int left   = binPoints[m];
        const int centre = binPoints[m + 1];
        const int right  = binPoints[m + 2];

        melFilters[(size_t) m].startBin = left;
        melFilters[(size_t) m].weights.assign ((size_t) (right - left + 1), 0.0f);
        auto& w = melFilters[(size_t) m].weights;

        for (int bin = left; bin < centre; ++bin)
            if (centre > left)
                w[(size_t) (bin - left)] = (float) (bin - left) / (float) (centre - left);

        for (int bin = centre; bin <= right; ++bin)
            if (right > centre)
                w[(size_t) (bin - left)] = (float) (right - bin) / (float) (right - centre);
    }
}

//==============================================================================
std::vector<float> BeatThisAnalyzer::resampleToMono (const juce::AudioBuffer<float>& buffer,
                                                      double sourceSampleRate,
                                                      int startSample,
                                                      int endSample) const
{
    const int numChannels = buffer.getNumChannels();
    const int numSrcSamples = endSample - startSample;

    if (numSrcSamples <= 0 || numChannels <= 0 || sourceSampleRate <= 0.0)
        return {};

    // Target number of samples at 22050 Hz
    const double ratio   = kTargetSampleRate / sourceSampleRate;
    const int numDstSamples = juce::jmax (1, (int) std::round ((double) numSrcSamples * ratio));

    std::vector<float> mono (numDstSamples, 0.0f);

    for (int dstIdx = 0; dstIdx < numDstSamples; ++dstIdx)
    {
        const double srcFloat = (double) dstIdx / ratio;
        const int    srcLo    = (int) srcFloat;
        const float  frac     = (float) (srcFloat - (double) srcLo);
        const int    srcHi    = juce::jmin (srcLo + 1, numSrcSamples - 1);

        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = buffer.getReadPointer (ch, startSample);
            sum += data[srcLo] * (1.0f - frac) + data[srcHi] * frac;
        }
        mono[(size_t) dstIdx] = sum / (float) numChannels;
    }

    return mono;
}

//==============================================================================
std::vector<float>
BeatThisAnalyzer::computeMelSpectrogram (const std::vector<float>& mono) const
{
    if (mono.empty())
        return {};

    // Hann window
    std::vector<float> window ((size_t) kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[(size_t) i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (kFftSize - 1)));

    juce::dsp::FFT fft (static_cast<int> (std::log2 (kFftSize)));

    const int halfWin     = kFftSize / 2;
    const int totalSamples = (int) mono.size();
    const int estFrames   = juce::jmax (1, (int) std::ceil ((double) totalSamples / kHopSize));

    // Flat output: [numFrames * kMelBins] — avoids per-frame heap allocation
    std::vector<float> flat;
    flat.reserve ((size_t) (estFrames * kMelBins));

    std::vector<float> fftBuf ((size_t) kFftSize * 2);

    for (int centre = 0; centre < totalSamples; centre += kHopSize)
    {
        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);

        for (int i = 0; i < kFftSize; ++i)
        {
            const int srcIdx = centre - halfWin + i;
            if (srcIdx >= 0 && srcIdx < totalSamples)
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

            flat.push_back (std::log (1.0f + kLogScale * juce::jmax (0.0f, energy)));
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
        const float* raw = outputTensors[(size_t) idx].GetTensorData<float>();
        const int64_t n  = outputTensors[(size_t) idx].GetTensorTypeAndShapeInfo().GetShape()[1];
        std::vector<float> probs ((size_t) n);
        for (int64_t i = 0; i < n; ++i)
            probs[(size_t) i] = sigmoid (raw[i]);
        return probs;
    };

    return { extractProbs (0), extractProbs (1) };
}

//==============================================================================
BeatThisAnalyzer::BeatDownbeatProbs
BeatThisAnalyzer::runOnnxInference (const std::vector<float>& flatMel, int numFrames) const
{
    if (! sessionReady.load() || flatMel.empty() || numFrames <= 0)
        return {};

    if (numFrames <= kMaxChunkFrames)
        return runOnnxInferenceChunk (flatMel, numFrames, 0, numFrames);

    // Chunk with overlap; stitch both beat and downbeat arrays together.
    BeatDownbeatProbs merged;
    merged.beat.reserve ((size_t) numFrames);
    merged.downbeat.reserve ((size_t) numFrames);

    const int halfOverlap = kChunkOverlap / 2;
    const int step        = kMaxChunkFrames - kChunkOverlap;

    for (int chunkStart = 0; chunkStart < numFrames; chunkStart += step)
    {
        const int chunkEnd = juce::jmin (chunkStart + kMaxChunkFrames, numFrames);
        auto chunk = runOnnxInferenceChunk (flatMel, numFrames, chunkStart, chunkEnd);

        if (chunk.beat.empty())
            return {};

        const int keepFrom = (chunkStart == 0) ? 0 : juce::jmin (halfOverlap, (int) chunk.beat.size());
        for (int i = keepFrom; i < (int) chunk.beat.size(); ++i)
        {
            merged.beat.push_back (chunk.beat[(size_t) i]);
            merged.downbeat.push_back (chunk.downbeat[(size_t) i]);
        }
    }

    return merged;
}

//==============================================================================
double BeatThisAnalyzer::estimateGlobalTempoACF (const std::vector<float>& activations)
{
    if (activations.empty())
        return 0.0;
        
    // Search between 70 BPM and 160 BPM
    const int maxLagFrames = (int) std::round (kFps * (60.0 / 70.0));
    const int minLagFrames = (int) std::round (kFps * (60.0 / 160.0));
    
    if ((int)activations.size() <= maxLagFrames)
        return 0.0;
        
    double bestScore = -1.0;
    int bestLag = 0;
    
    for (int lag = minLagFrames; lag <= maxLagFrames; ++lag)
    {
        double acf = 0.0;
        for (size_t i = 0; i < activations.size() - lag; ++i)
            acf += activations[i] * activations[i + lag];
            
        // Tempo prior: Gaussian centered at 110 BPM
        double currentBpm = 60.0 / ((double)lag / kFps);
        double diff = currentBpm - 110.0;
        double prior = std::exp (-(diff * diff) / (2.0 * 30.0 * 30.0));
        
        double score = acf * prior;
        if (score > bestScore)
        {
            bestScore = score;
            bestLag = lag;
        }
    }
    
    return (double)bestLag;
}

//==============================================================================
std::vector<int> BeatThisAnalyzer::dpBeatTrack (const std::vector<float>& activations, double targetIntervalFrames)
{
    if (activations.empty() || targetIntervalFrames <= 0.0)
        return {};
        
    const int n = (int) activations.size();
    std::vector<double> dpScore (n, -1e9);
    std::vector<int> dpBacklink (n, -1);
    
    const double txWt = 15.0; // Strong penalty for deviating from target interval
    
    // Evaluate local maxima to speed up DP and avoid picking the slope of a peak.
    std::vector<int> candidates;
    candidates.reserve (n / 10);
    for (int i = 1; i < n - 1; ++i)
    {
        if (activations[(size_t) i] >= 0.1f && 
            activations[(size_t) i] >= activations[(size_t) (i-1)] && 
            activations[(size_t) i] >= activations[(size_t) (i+1)])
        {
            candidates.push_back(i);
        }
    }
    
    if (candidates.empty())
        return {};

    for (size_t c = 0; c < candidates.size(); ++c)
    {
        int i = candidates[c];
        double bestPrevScore = 0.0; // Base score for starting a new sequence
        int bestPrevIdx = -1;
        
        // Search backwards among previous candidates
        for (int c_prev = (int)c - 1; c_prev >= 0; --c_prev)
        {
            int j = candidates[(size_t)c_prev];
            double interval = (double)(i - j);
            
            if (interval > targetIntervalFrames * 3.5)
                break; // Don't look too far back
                
            // Quantize interval to the nearest multiple of targetIntervalFrames
            double ratio = interval / targetIntervalFrames;
            int numBeats = (int)std::round (ratio);
            if (numBeats < 1) numBeats = 1;
            if (numBeats > 3) numBeats = 3;
            
            double adjustedRatio = ratio / (double)numBeats;
            double penalty = -txWt * std::pow (std::log2 (adjustedRatio), 2.0);
            
            // Penalize skipped beats to favor continuous tracking
            double skipPenalty = (numBeats - 1) * 2.0; 
            
            double score = dpScore[(size_t) j] + penalty - skipPenalty;
            if (score > bestPrevScore)
            {
                bestPrevScore = score;
                bestPrevIdx = j;
            }
        }
        
        dpScore[(size_t) i] = bestPrevScore + (double)activations[(size_t) i];
        dpBacklink[(size_t) i] = bestPrevIdx;
    }
    
    // Find the best ending beat
    double maxScore = -1e9;
    int bestEndIdx = -1;
    for (int i : candidates)
    {
        if (dpScore[(size_t) i] > maxScore)
        {
            maxScore = dpScore[(size_t) i];
            bestEndIdx = i;
        }
    }
    
    std::vector<int> path;
    int curr = bestEndIdx;
    while (curr != -1)
    {
        path.push_back (curr);
        curr = dpBacklink[(size_t) curr];
    }
    std::reverse (path.begin(), path.end());
    
    // Interpolate any skipped beats for a solid grid
    std::vector<int> filledPath;
    if (!path.empty())
    {
        filledPath.push_back (path[0]);
        for (size_t k = 1; k < path.size(); ++k)
        {
            int prev = path[k-1];
            int currBeat = path[k];
            double ratio = (double)(currBeat - prev) / targetIntervalFrames;
            int numBeats = (int)std::round (ratio);
            
            if (numBeats > 1 && numBeats <= 4)
            {
                for (int b = 1; b < numBeats; ++b)
                {
                    int interpolated = prev + (int)std::round ((double)(currBeat - prev) * b / numBeats);
                    filledPath.push_back (interpolated);
                }
            }
            filledPath.push_back (currBeat);
        }
    }
    
    return filledPath;
}

//==============================================================================
std::vector<int> BeatThisAnalyzer::peakPick (const std::vector<float>& activations,
                                              float threshold,
                                              int minGapFrames)
{
    std::vector<int> peaks;
    if (activations.empty())
        return peaks;

    // Simple peak-picking: local max above threshold with minimum gap
    for (int i = 1; i < (int) activations.size() - 1; ++i)
    {
        if (activations[(size_t) i] < threshold)
            continue;

        const bool isLocalMax = (activations[(size_t) i] >= activations[(size_t) i - 1])
                             && (activations[(size_t) i] >= activations[(size_t) i + 1]);
        if (! isLocalMax)
            continue;

        if (! peaks.empty() && (i - peaks.back()) < minGapFrames)
        {
            // Keep the taller peak
            if (activations[(size_t) i] > activations[(size_t) peaks.back()])
                peaks.back() = i;
        }
        else
        {
            peaks.push_back (i);
        }
    }

    return peaks;
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

    if (probs.beat.empty())
        return result;

    const auto& beatProbs     = probs.beat;
    const auto& downbeatProbs = probs.downbeat;

    // -- 4. Global Tempo Estimation via Autocorrelation
    double acfIntervalFrames = estimateGlobalTempoACF (beatProbs);
    
    // Check downbeats to anchor the global tempo
    // Min gap for downbeats is roughly 3 beats
    const int minDbGapFrames = juce::jmax (1, (int) std::round (kFps * (double) kMinBeatGapSecs * 3.0));
    const auto downbeatIdxs = peakPick (downbeatProbs, 0.4f, minDbGapFrames);
    
    double targetIntervalFrames = acfIntervalFrames;
    
    if (downbeatIdxs.size() >= 2)
    {
        std::vector<double> dbIntervals;
        dbIntervals.reserve (downbeatIdxs.size() - 1);
        for (size_t i = 1; i < downbeatIdxs.size(); ++i)
            dbIntervals.push_back ((double)(downbeatIdxs[i] - downbeatIdxs[i-1]));
            
        std::sort (dbIntervals.begin(), dbIntervals.end());
        double medianDb = dbIntervals[dbIntervals.size() / 2];
        double derivedBeatInterval = medianDb / 4.0;
        
        // If the downbeat-derived beat interval is reasonable (70 - 160 BPM)
        // we heavily trust it because soul/funk downbeats are very clear.
        if (derivedBeatInterval >= (kFps * 60.0 / 160.0) && derivedBeatInterval <= (kFps * 60.0 / 70.0))
        {
            targetIntervalFrames = derivedBeatInterval;
        }
    }
    
    if (targetIntervalFrames <= 0.0)
    {
        // Fallback to naive estimation or default 120 bpm
        targetIntervalFrames = kFps * 0.5;
    }

    // -- 5. Dynamic Programming Beat Tracking
    const auto beatFrameIdxs = dpBeatTrack (beatProbs, targetIntervalFrames);

    if (beatFrameIdxs.size() < 2)
        return result;

    // -- 6. Convert frame indices to seconds with parabolic sub-frame interpolation.
    const double analysisStartSecs = (double) start / sampleRate;
    std::vector<double> beatPosSecs;
    beatPosSecs.reserve (beatFrameIdxs.size());
    for (int idx : beatFrameIdxs)
    {
        double refinedIdx = idx;
        if (idx > 0 && idx < (int) beatProbs.size() - 1)
        {
            const float a     = beatProbs[(size_t) (idx - 1)];
            const float b     = beatProbs[(size_t) idx];
            const float c     = beatProbs[(size_t) (idx + 1)];
            const float denom = a - 2.0f * b + c;
            if (std::abs (denom) > 1e-6f)
                refinedIdx = (double) idx - 0.5 * (double) (c - a) / (double) denom;
        }
        beatPosSecs.push_back (analysisStartSecs + refinedIdx / kFps);
    }

    // -- 7. Compute BPM from beat intervals
    std::vector<double> intervals;
    intervals.reserve (beatPosSecs.size() - 1);
    for (size_t i = 1; i < beatPosSecs.size(); ++i)
    {
        const double iv = beatPosSecs[i] - beatPosSecs[i - 1];
        if (iv > 0.1 && iv < 2.0)
            intervals.push_back (iv);
    }

    if (intervals.empty())
        return result;

    std::vector<double> sortedIntervals = intervals;
    std::sort (sortedIntervals.begin(), sortedIntervals.end());
    const size_t nIv = sortedIntervals.size();
    const double medianInterval = (nIv % 2 == 0)
        ? 0.5 * (sortedIntervals[nIv / 2 - 1] + sortedIntervals[nIv / 2])
        : sortedIntervals[nIv / 2];

    if (medianInterval <= 0.0)
        return result;

    double intervalSum   = 0.0;
    int    intervalCount = 0;
    for (double iv : intervals)
    {
        if (std::abs (iv - medianInterval) / medianInterval <= 0.20)
        {
            intervalSum += iv;
            ++intervalCount;
        }
    }
    const double refinedInterval = (intervalCount >= 3)
        ? intervalSum / (double) intervalCount
        : medianInterval;

    double variance = 0.0;
    for (double iv : intervals)
    {
        const double d = iv - medianInterval;
        variance += d * d;
    }
    variance /= (double) intervals.size();
    const double drift = std::sqrt (variance) / medianInterval;

    // Confidence: mean beat activation at detected peaks
    float meanBeatStrength = 0.0f;
    for (int idx : beatFrameIdxs)
        meanBeatStrength += beatProbs[(size_t) idx];
    meanBeatStrength /= (float) beatFrameIdxs.size();

    // -- 8. Downbeat phase (0–3)
    int downbeatPhase = 0;
    if (! downbeatProbs.empty())
    {
        double bestPhaseScore = -1.0;
        for (int phase = 0; phase < 4 && phase < (int) beatFrameIdxs.size(); ++phase)
        {
            double phaseScore = 0.0;
            for (int b = phase; b < (int) beatFrameIdxs.size(); b += 4)
            {
                const int frameIdx = beatFrameIdxs[(size_t) b];
                if (frameIdx < (int) downbeatProbs.size())
                    phaseScore += (double) downbeatProbs[(size_t) frameIdx];
            }
            if (phaseScore > bestPhaseScore)
            {
                bestPhaseScore = phaseScore;
                downbeatPhase  = phase;
            }
        }
    }

    // -- 9. Bar positions
    std::vector<double> barPosSecs;
    for (int b = downbeatPhase; b < (int) beatPosSecs.size(); b += 4)
        barPosSecs.push_back (beatPosSecs[(size_t) b]);

    // -- 10. Fill result
    result.valid            = true;
    result.estimatedBpm     = 60.0 / refinedInterval;
    result.beatPeriodSecs   = refinedInterval;
    result.firstBeatSecs    = beatPosSecs.front();
    result.confidence       = juce::jlimit (0.0f, 1.0f, meanBeatStrength);
    result.likelyDrifting   = drift > 0.03;
    result.downbeatPhase    = downbeatPhase;
    result.beatPositionsSecs = std::move (beatPosSecs);
    result.barPositionsSecs  = std::move (barPosSecs);

    return result;
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
