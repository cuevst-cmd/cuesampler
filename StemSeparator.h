#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declare ORT types to avoid polluting all translation units (mirrors
// BeatThisAnalyzer.h).
namespace Ort { struct Env; struct SessionOptions; }

/**
 * StemSeparator
 * -------------
 * Runs the base HTDemucs (Meta's hybrid-transformer Demucs v4) through the
 * already-bundled ONNX Runtime to split a loaded sample into drums / bass /
 * vocals stems. This is an OFFLINE pass (tens of seconds), designed to run on a
 * background thread — never a per-block effect. Mirrors BeatThisAnalyzer's shape
 * (Ort::Env/SessionOptions held as members, isReady(), one heavy method run off
 * the message/audio thread).
 *
 * SINGLE-PASS: one Ort::Session running the base htdemucs model, which outputs
 * ALL FOUR stems ([1,4,2,T]) in one forward pass. We slice drums/bass/vocals
 * from that output. This replaced the earlier htdemucs_ft "bag of 4 specialists"
 * design (three full model passes per separation) — that was ~0.8x realtime;
 * one pass is ~3x faster at industry-standard quality. The plugin's "other"
 * stem stays subtraction-based (other = original − drums − bass − vocals) in the
 * processor, so downstream is unchanged.
 *
 * IMPORTANT — the ONNX graph is end-to-end WAVEFORM → WAVEFORM.
 *   The demucs-onnx (StemSplit) export bakes the STFT/iSTFT INTO the graph as
 *   Conv1d/ConvTranspose1d sin/cos kernels (see export_htdemucs.py). So this
 *   class computes NO STFT itself: it feeds raw stereo samples and reads raw
 *   stereo samples back.
 *
 * Pipeline per separate():
 *   buffer (any rate/channels) → resample to 44100 Hz stereo → segment into
 *   7.8 s windows with kOverlap overlap → run the model per segment (slicing the
 *   three stems we keep) → triangular-weighted overlap-add → resample each stem
 *   back to the source rate and conform to the source length/channel count.
 */
class StemSeparator
{
public:
    //==========================================================================
    struct StemResult
    {
        bool valid = false;
        // At the SOURCE sample rate, with the source length and channel count.
        juce::AudioBuffer<float> drums, bass, vocals;
    };

    /** Absolute path to the single base-htdemucs ONNX model. An empty path or a
     *  model that fails to load leaves the separator not-ready (separate()
     *  returns an invalid result and the caller falls back to the original
     *  buffer). */
    struct ModelPaths
    {
        juce::String model;
    };

    //==========================================================================
    explicit StemSeparator (const ModelPaths& paths);
    ~StemSeparator();

    /** True only if the model session loaded successfully. */
    bool isReady() const noexcept { return sessionReady.load(); }

    /** Separate a buffer into drums/bass/vocals stems.
     *  @param buffer      source audio (any sample rate, 1+ channels)
     *  @param sampleRate  sample rate of 'buffer'
     *  @param progress    optional, called with [0,1] as work proceeds
     *  @param shouldAbort optional, polled between segments; if it returns true
     *                     the run bails out early and returns an invalid result.
     */
    StemResult separate (const juce::AudioBuffer<float>& buffer,
                         double sampleRate,
                         std::function<void(float)> progress = {},
                         std::function<bool()>       shouldAbort = {}) const;

    //==========================================================================
    // Model / preprocessing constants (from the export spec).
    static constexpr double kModelSampleRate = 44100.0;
    static constexpr int    kModelChannels   = 2;
    static constexpr int    kSegmentSamples  = 343980;  // 7.8 s @ 44.1 kHz
    // Demucs default is 0.25; we ran 0.10, now default 0.0 (no overlap) for the
    // fewest inferences. This is the compile-time fallback only — the live value
    // is resolved from CUE_STEM_OVERLAP at runtime (clamped [0, 0.5]) so the
    // speed/boundary-artifact trade can be dialled in by ear. Bump to ~0.05 if
    // segment seams become audible.
    static constexpr double kOverlap         = 0.0;

    // Standard Demucs source order in the model's [1,4,2,T] output. We keep
    // drums/bass/vocals; "other" (index 2) is recomputed by subtraction downstream.
    enum StemIndex { Drums = 0, Bass = 1, Other = 2, Vocals = 3 };

    // The three stems we slice out of the all-stems output, in StemResult order.
    static constexpr int kKeptStemIndices[3] = { Drums, Bass, Vocals };

private:
    //==========================================================================
    // The loaded model: its Ort::Session plus cached I/O names. Defined in the
    // .cpp so the ORT C++ headers stay out of this header.
    struct Model;

    // Opens the model session, preferring accelOpts (CoreML) and retrying with
    // cpuOpts if CoreML session creation fails. Returns a Model with
    // ready==false (logged) if the path is empty/missing or both attempts fail.
    static std::unique_ptr<Model> loadModel (Ort::Env& env,
                                             Ort::SessionOptions* accelOpts,
                                             Ort::SessionOptions& cpuOpts,
                                             const juce::String& path,
                                             const char* label);

    std::unique_ptr<Ort::Env>            ortEnv;
    std::unique_ptr<Ort::SessionOptions> ortOptions;        // CPU-only base
    std::unique_ptr<Ort::SessionOptions> ortOptionsAccel;   // GPU/ANE accelerator EP options
                                                            // (DirectML on Windows, CoreML on macOS);
                                                            // null if unavailable/disabled
    std::unique_ptr<Model>               stemModel;
    std::atomic<bool>                    sessionReady { false };

    //==========================================================================
    // The three 44.1 kHz stereo stems produced by one segmented model pass.
    struct Stems44 { juce::AudioBuffer<float> drums, bass, vocals; };

    // Resample a buffer between rates (per-channel Lagrange). Returns a copy at
    // dstRate; returns a plain copy when the rates already match.
    static juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& src,
                                              double srcRate, double dstRate);

    // Force 'src' to stereo (duplicate mono; take first two channels otherwise).
    static juce::AudioBuffer<float> toStereo (const juce::AudioBuffer<float>& src);

    // Crop/pad 'src' to exactly dstLen samples and dstChannels channels
    // (downmixing to mono when dstChannels == 1).
    static juce::AudioBuffer<float> conform (const juce::AudioBuffer<float>& src,
                                             int dstChannels, int dstLen);

    // Run the model over a full 44.1 kHz stereo mix with segmentation +
    // triangular overlap-add, slicing drums/bass/vocals from each segment's
    // all-stems output. Returns three [2, L] buffers at 44.1 kHz, or empty
    // buffers if aborted/failed.
    Stems44 runModel (const Model& model,
                      const juce::AudioBuffer<float>& mix44,
                      const std::function<void(float)>& progress,
                      const std::function<bool()>& shouldAbort) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparator)
};
