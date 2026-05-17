#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declare ORT types to avoid polluting all translation units
namespace Ort { struct Env; struct Session; struct SessionOptions; }

/**
 * BeatThisAnalyzer
 * ----------------
 * Runs the beat_this (ISMIR 2024) neural beat tracker on a loaded audio buffer
 * and returns beat / downbeat positions with high accuracy (~92-95% F-measure).
 *
 * The analysis is designed to run entirely on a background thread.
 * All public methods are thread-safe when called from a single analysis thread.
 *
 * Pipeline:
 *   AudioBuffer → resample to 22050 Hz mono → Mel spectrogram (128 bands, 50 fps)
 *   → ONNX Runtime inference → sigmoid → peak-pick → tempo calc
 */
class BeatThisAnalyzer
{
public:
    //==========================================================================
    struct Result
    {
        bool    valid            = false;
        double  estimatedBpm     = 0.0;
        double  beatPeriodSecs   = 0.0;
        double  firstBeatSecs    = 0.0;
        float   confidence       = 0.0f;
        bool    likelyDrifting   = false;
        int     downbeatPhase    = 0;  // 0-3
        std::vector<double> beatPositionsSecs;
        std::vector<double> barPositionsSecs;
    };

    //==========================================================================
    /** Constructs the analyzer.
     *  @param onnxModelPath  Absolute path to beat_this.onnx
     */
    explicit BeatThisAnalyzer (const juce::String& onnxModelPath);

    ~BeatThisAnalyzer();

    /** Returns true if the ONNX session loaded successfully. */
    bool isReady() const noexcept { return sessionReady.load(); }

    /** Run inference on a loaded audio buffer.
     *  @param buffer      The source audio (any sample rate, any channel count)
     *  @param sampleRate  Sample rate of 'buffer'
     *  @param startSample Analysis start (0 = full file)
     *  @param endSample   Analysis end (-1 = full file)
     *  @return            Beat analysis result
     */
    Result analyze (const juce::AudioBuffer<float>& buffer,
                    double sampleRate,
                    int startSample = 0,
                    int endSample   = -1) const;

private:
    //==========================================================================
    // Constants matching beat_this's training configuration
    static constexpr double  kTargetSampleRate = 22050.0;
    static constexpr int     kFftSize          = 1024;
    static constexpr int     kHopSize          = 441;    // 50 fps
    static constexpr int     kMelBins          = 128;
    static constexpr double  kFps              = 50.0;   // kTargetSampleRate / kHopSize
    static constexpr double  kFreqMin          = 30.0;
    static constexpr double  kFreqMax          = 10000.0;
    static constexpr float   kLogScale         = 1000.0f; // ln(1 + 1000*x)
    static constexpr float   kBeatThreshold    = 0.5f;   // sigmoid threshold
    static constexpr float   kMinBeatGapSecs   = 0.25f;  // 240 BPM max

    //==========================================================================
    // Mel filter bank: sparse triangular filters, one per band
    struct MelFilter { int startBin = 0; std::vector<float> weights; };
    std::vector<MelFilter> melFilters;  // size kMelBins

    // ONNX Runtime objects
    std::unique_ptr<Ort::Env>            ortEnv;
    std::unique_ptr<Ort::SessionOptions> ortOptions;
    std::unique_ptr<Ort::Session>        ortSession;
    std::atomic<bool>                    sessionReady { false };

    //==========================================================================
    void buildMelFilters();

    std::vector<float> resampleToMono (const juce::AudioBuffer<float>& buffer,
                                       double sourceSampleRate,
                                       int startSample,
                                       int endSample) const;

    // Returns flat buffer [numFrames * kMelBins]; numFrames = result.size() / kMelBins
    std::vector<float> computeMelSpectrogram (const std::vector<float>& mono) const;

    struct BeatDownbeatProbs { std::vector<float> beat; std::vector<float> downbeat; };

    BeatDownbeatProbs runOnnxInference (const std::vector<float>& flatMel, int numFrames) const;
    BeatDownbeatProbs runOnnxInferenceChunk (const std::vector<float>& flatMel,
                                             int totalFrames,
                                             int startFrame,
                                             int endFrame) const;

    // beat_this positional encoding is fixed to 8192 frames (~163 s at 50 fps)
    static constexpr int kMaxChunkFrames   = 8192;
    static constexpr int kChunkOverlap     = 512;  // frames of overlap between chunks

    static std::vector<int> peakPick (const std::vector<float>& activations,
                                      float threshold,
                                      int minGapFrames);

    static double estimateGlobalTempoACF (const std::vector<float>& activations);
    
    static std::vector<int> dpBeatTrack (const std::vector<float>& activations, 
                                         double targetIntervalFrames);

    static float sigmoid (float x) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatThisAnalyzer)
};
