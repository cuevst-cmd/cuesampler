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
 * Runs HTDemucs-FT (Meta's fine-tuned hybrid-transformer Demucs v4) through the
 * already-bundled ONNX Runtime to split a loaded sample into drums / bass /
 * vocals stems. This is an OFFLINE pass (tens of seconds), designed to run on a
 * background thread — never a per-block effect. Mirrors BeatThisAnalyzer's shape
 * (Ort::Env/SessionOptions held as members, isReady(), one heavy method run off
 * the message/audio thread).
 *
 * IMPORTANT — the ONNX graph is end-to-end WAVEFORM → WAVEFORM.
 *   The demucs-onnx (StemSplit) export bakes the STFT/iSTFT INTO the graph as
 *   Conv1d/ConvTranspose1d sin/cos kernels (see export_htdemucs.py). So this
 *   class computes NO STFT itself: it feeds raw stereo samples and reads raw
 *   stereo samples back. (Phase 1 verified this against the package; it is the
 *   opposite of the sevagh/demucs.onnx "STFT in host code" approach.)
 *
 * One Ort::Session per FT specialist (drums/bass/vocals). We don't run the
 * "other" specialist — in the plugin's subtraction model, other = original −
 * (drums + bass + vocals), computed later in the processor.
 *
 * Pipeline per separate():
 *   buffer (any rate/channels) → resample to 44100 Hz stereo → segment into
 *   7.8 s windows with 0.25 overlap → run each specialist per segment →
 *   triangular-weighted overlap-add → resample each stem back to the source
 *   rate and conform to the source length/channel count.
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

    /** Absolute paths to the three FT specialists we need. A path that is empty
     *  or does not load leaves the separator not-ready (separate() returns an
     *  invalid result and the caller falls back to the original buffer). */
    struct ModelPaths
    {
        juce::String drums, bass, vocals;
    };

    //==========================================================================
    explicit StemSeparator (const ModelPaths& paths);
    ~StemSeparator();

    /** True only if all three specialist sessions loaded successfully. */
    bool isReady() const noexcept { return sessionReady.load(); }

    /** Separate a buffer into drums/bass/vocals stems.
     *  @param buffer      source audio (any sample rate, 1+ channels)
     *  @param sampleRate  sample rate of 'buffer'
     *  @param progress    optional, called with [0,1] as work proceeds
     *  @param shouldAbort optional, polled between segments/models; if it returns
     *                     true the run bails out early and returns an invalid
     *                     result. (Minimal extension over the Phase 2 signature
     *                     so the Phase 3 ThreadPoolJob can cancel a stale run via
     *                     shouldExit(); the 3-arg form still compiles unchanged.)
     */
    StemResult separate (const juce::AudioBuffer<float>& buffer,
                         double sampleRate,
                         std::function<void(float)> progress = {},
                         std::function<bool()>       shouldAbort = {}) const;

    //==========================================================================
    // Model / preprocessing constants (from Phase 1's export spec).
    static constexpr double kModelSampleRate = 44100.0;
    static constexpr int    kModelChannels   = 2;
    static constexpr int    kSegmentSamples  = 343980;  // 7.8 s @ 44.1 kHz
    static constexpr double kOverlap         = 0.25;     // Demucs default

    // Standard Demucs source order. Used to slice the specialty stem when an
    // exported graph returns all four stems ([1,4,2,T]); harmless when a graph
    // returns a single stem ([1,2,T]).
    enum StemIndex { Drums = 0, Bass = 1, Other = 2, Vocals = 3 };

private:
    //==========================================================================
    // One loaded specialist: its Ort::Session plus cached I/O names. Defined in
    // the .cpp so the ORT C++ headers stay out of this header.
    struct Model;

    // Opens one specialist session, preferring accelOpts (CoreML) and retrying
    // with cpuOpts if CoreML session creation fails. Returns a Model with
    // ready==false (logged) if the path is empty/missing or both attempts fail.
    static std::unique_ptr<Model> loadModel (Ort::Env& env,
                                             Ort::SessionOptions* accelOpts,
                                             Ort::SessionOptions& cpuOpts,
                                             const juce::String& path,
                                             const char* label);

    std::unique_ptr<Ort::Env>            ortEnv;
    std::unique_ptr<Ort::SessionOptions> ortOptions;        // CPU-only base
    std::unique_ptr<Ort::SessionOptions> ortOptionsCoreML;  // CoreML (macOS); null if unavailable
    std::unique_ptr<Model>               drumsModel;
    std::unique_ptr<Model>               bassModel;
    std::unique_ptr<Model>               vocalsModel;
    std::atomic<bool>                    sessionReady { false };

    //==========================================================================
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

    // Run one specialist over a full 44.1 kHz stereo mix with segmentation +
    // triangular overlap-add. Returns the specialty stem as [2, L] at 44.1 kHz,
    // or an empty buffer if aborted/failed. 'progress' is already mapped to this
    // model's slice of the global [0,1].
    juce::AudioBuffer<float> runModel (const Model& model,
                                       const juce::AudioBuffer<float>& mix44,
                                       int specialtyStemIndex,
                                       const std::function<void(float)>& progress,
                                       const std::function<bool()>& shouldAbort) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparator)
};
