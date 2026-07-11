#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "AudioFingerprinter.h"
#include "BeatThisAnalyzer.h"
#include "StemSeparator.h"
#include "EditTelemetry.h"
#include "UpdateChecker.h"
#include "KeyDetector.h"
#include "ChopAudioCache.h"
#include "SSLBusCompressor.h"
#include "BitCrusher.h"
#include "WarpMap.h"
#include "CueFxRack/FxEngine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

//==============================================================================
class AudioPluginAudioProcessor final : public juce::AudioProcessor,
                                        private juce::AsyncUpdater
{
public:
    struct LoadedSampleData
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0;
        juce::File sourceFile;
        juce::String filePath;
        juce::String fileName;
        int leadingContentStartSample = 0;
        juce::MemoryBlock serializedStateData;
    };

    // Result of HTDemucs-FT separation for the current sample. Holds the three
    // stems we mute against plus a pointer to the PRISTINE source sample they
    // were derived from. Muting is subtraction: played buffer = source.buffer −
    // Σ(muted stems); "other" is implicit and always plays. Keeping the source
    // sample (not a copy of its buffer) lets "nothing muted" restore the exact
    // original object bit-for-bit with zero copy. Swapped via atomic_load/store.
    struct StemSet
    {
        std::shared_ptr<LoadedSampleData> source;   // pristine original to subtract from
        juce::AudioBuffer<float> drums, bass, vocals; // source rate/length/channels
    };

    struct TempoAnalysisData
    {
        double estimatedBpm = 0.0;
        float confidence = 0.0f;
        bool likelyDrifting = false;
        double beatPeriodSeconds = 0.0;
        double firstBeatSeconds = 0.0;
        std::vector<double> beatPositionsSeconds;
        std::vector<double> barPositionsSeconds;
        int downbeatPhase = 0;
        double analysisStartSeconds = 0.0;
        double analysisEndSeconds = 0.0;
    };

    struct TempoEditState
    {
        int regionStartSample = -1;
        int regionEndSample = -1;
    };

    using ChopWarpMarker = cuesampler::ChopWarpMarker;

    struct ChopDefinition
    {
        int id = 0;
        int startSample = 0;
        int endSample = 0;
        int cueOffsetSamples = 0;
        float gainDecibels = 0.0f;
        float pitchSemitones = 0.0f;
        bool favorite = false;
        std::vector<ChopWarpMarker> warpMarkers; // sorted by sourceSample; empty = no warp
    };

    struct ChopState
    {
        int selectedChopId = -1;
        int nextChopId = 1;
        std::vector<ChopDefinition> chops;
    };

    //==============================================================================
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Sample loading
    void loadAudioFile (const juce::File& file);
    std::shared_ptr<const LoadedSampleData> getLoadedSample() const;

    void startPlayback() noexcept;
    void pausePlayback() noexcept;
    void stopPlayback() noexcept;
    void setPlaybackSamplePosition (double newPosition) noexcept;
    void setTimeStretchRatio (float newRatio) noexcept;
    void setPitchSemitones (float newSemitones) noexcept;
    void setSyncToHost (bool shouldSync) noexcept;
    void setHalfTimeEnabled (bool shouldEnable) noexcept;

    // MIDI octave shift, in octaves, applied to the chop note mapping so users
    // whose keyboard lacks octave buttons can reach all chops. 0 = default
    // (C2 -> chop 1). Clamped to [midiOctaveOffsetMin, midiOctaveOffsetMax].
    void setMidiOctaveOffset (int octaves) noexcept;
    int  getMidiOctaveOffset() const noexcept;

    // Lowest MIDI note that triggers chop 1 with the current octave shift.
    int  getMidiRootNote() const noexcept;

    // MIDI note that triggers the given chop id (root + its list index) under
    // the current octave shift, or -1 if no such chop. Lets the editor light
    // the matching key on the on-screen keyboard when a chop is previewed.
    int  getMidiNoteForChopId (int chopId) const noexcept;
    int  getSelectedChopMidiNote() const noexcept;

    // Mirrors the incoming MIDI stream for the editor's on-screen keyboard,
    // and injects notes played by clicking it back into processBlock.
    juce::MidiKeyboardState keyboardState;

    static constexpr int midiOctaveOffsetMin = -3;
    static constexpr int midiOctaveOffsetMax = 4;
    void setGridBpmTrim (float trimBpm);
    void setGridStartOffset (float offsetSeconds);
    void setWaveformZoom (float zoomValue) noexcept;
    void setWaveformScroll (float scrollValue) noexcept;

    // Compressor (SSL-style bus compressor on the master output).
    void  setCompressorEnabled    (bool shouldEnable) noexcept;
    void  setCompressorThresholdDb (float dB)         noexcept;
    void  setCompressorMakeupDb   (float dB)          noexcept;
    bool  isCompressorEnabled() const noexcept;
    float getCompressorThresholdDb() const noexcept;
    float getCompressorMakeupDb() const noexcept;
    float getCompressorGainReductionDb() const noexcept;

    // Bit Crusher (bit-depth quantization + sample-rate reduction).
    void  setBitCrusherEnabled (bool shouldEnable) noexcept;
    void  setBitCrusherBits    (float bits)        noexcept;
    void  setBitCrusherCrush   (float percent)     noexcept;

    // Resize a chop by moving its left or right edge to a new sample position.
    // The implied chop length sets the new BPM trim, and the grid offset is
    // adjusted so the dragged edge lands on a fresh chop boundary. All other
    // chops are then rebuilt against this new grid.
    void  resizeChopBoundary (int chopId, int newStartSample, int newEndSample);
    void  setChopBounds (int chopId, int newStartSample, int newEndSample);
    bool  isBitCrusherEnabled() const noexcept;
    float getBitCrusherBits()   const noexcept;
    float getBitCrusherCrush()  const noexcept;

    // Live post-bit-crusher output snapshot for the on-module scope visualizer:
    // a small rolling window of mono output samples in chronological order,
    // published lock-free from the audio thread and read on the editor timer.
    static constexpr int kBitCrusherScopeSize = 64;
    void  readBitCrusherScope (float* dest, int maxSamples) const noexcept;

    //==============================================================================
    // CUE FX rack — CUERACK's master chain, ported (see CueFxRack/FxEngine.h).
    // Parameters live in an APVTS whose state nests inside the sampler's
    // session state; the editor's rack strip attaches its panels directly.
    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "PARAMS", cue::createParameterLayout() };

    float getFxCompGainReductionDb() const        { return fxEngine.getCompGainReductionDb(); }
    int   getFxLimiterNumBands() const            { return fxEngine.getLimiterNumBands(); }
    float getFxLimiterBandGRDb (int band) const   { return fxEngine.getLimiterBandGRDb (band); }
    juce::Point<float> getFxImagerMidSide() const { return { fxEngine.getMidLevel(), fxEngine.getSideLevel() }; }

    bool isPlaying() const noexcept;
    double getPlaybackSamplePosition() const noexcept;
    int getLastTriggeredChopId() const noexcept;
    uint64_t getChopTriggerRevision() const noexcept;
    float getOutputMeterLevel() const noexcept;
    float getTimeStretchRatio() const noexcept;
    float getPitchSemitones() const noexcept;
    bool getSyncToHost() const noexcept;
    bool getHalfTimeEnabled() const noexcept;
    double getHostBpm() const noexcept;
    float getGridBpmTrim() const noexcept;
    float getGridStartOffset() const noexcept;
    float getWaveformZoom() const noexcept;
    float getWaveformScroll() const noexcept;
    double getResolvedGridAnchorSeconds() const noexcept;
    int getChopBarsCount() const noexcept;
    bool isTempoAnalysisInProgress() const noexcept;
    bool isKeyDetectionInProgress() const noexcept;
    KeyDetector::Result getDetectedKey() const;

    // ---- Stem separation (HTDemucs-FT) ------------------------------------
    // Each setter flips the flag and schedules a background remix of the played
    // buffer (original − Σ muted stems), atomic-swapped into loadedSample. No-op
    // (original keeps playing) until stems are ready. Message-thread facing.
    void setMuteDrums  (bool shouldMute) noexcept;
    void setMuteBass   (bool shouldMute) noexcept;
    void setMuteVocals (bool shouldMute) noexcept;
    bool getMuteDrums()  const noexcept { return muteDrums.load (std::memory_order_acquire); }
    bool getMuteBass()   const noexcept { return muteBass.load (std::memory_order_acquire); }
    bool getMuteVocals() const noexcept { return muteVocals.load (std::memory_order_acquire); }

    // Manually start a stem-separation pass on the currently-loaded sample. The
    // STEMS button in the editor calls this. Stem separation is NOT automatic:
    // the heavy ONNX model is loaded lazily on the first request (off the message
    // thread) so simply instantiating the plugin in a host stays fast. No-op if
    // no sample is loaded or no model is installed.
    void requestStemSeparation();

    // True while a background separation pass is running; true once the three
    // stems exist for the current sample; [0,1] progress of the running pass.
    bool  isSeparatingStems() const noexcept { return stemSeparationInProgress.load (std::memory_order_acquire); }
    bool  areStemsReady()     const noexcept { return stemsReady.load (std::memory_order_acquire); }
    float getStemProgress()   const noexcept { return stemProgress.load (std::memory_order_acquire); }
    // True while the ONNX session is being built on the stem thread (the one-time
    // cost paid on the first separation request). Lets the UI show a "preparing"
    // hint before per-segment progress starts moving.
    bool  isLoadingStemModel() const noexcept { return stemModelLoading.load (std::memory_order_acquire); }
    // True if an htdemucs model file is installed AND its session hasn't failed to
    // build — lets the UI distinguish "no model installed" from an idle/too-long
    // state, and enables the STEMS button. Skipped == separation bypassed because
    // the sample exceeds kMaxStemSeparationSeconds.
    bool  areStemModelsAvailable() const noexcept;
    bool  wasStemSeparationSkipped() const noexcept { return stemSeparationSkipped.load (std::memory_order_acquire); }

    // Apply a user key override (rootIndex 0..11 = C..B). Replaces the displayed
    // key and records a key_correction in the flywheel (user pick = ground truth).
    void setUserKeyOverride (int rootIndex, bool isMajor);

    // Opt-in data-flywheel consent. When enabled, anonymous BPM and key
    // detection deltas are recorded locally (see EditTelemetry). Defaults off.
    bool isTelemetryEnabled() const noexcept { return editTelemetry.isEnabled(); }
    void setTelemetryEnabled (bool shouldEnable) { editTelemetry.setEnabled (shouldEnable); }

    // In-app software update checker (GitHub Releases). The editor reads its
    // result to show a "new version available" banner. See UpdateChecker.
    cuesampler::UpdateChecker& getUpdateChecker() noexcept { return updateChecker; }
    std::shared_ptr<const TempoAnalysisData> getTempoAnalysis() const;
    std::shared_ptr<const TempoEditState> getTempoEditState() const;
    std::shared_ptr<const ChopState> getChopState() const;
    uint64_t getTempoUiRevision() const noexcept;

    // Warp cache access. The audio thread should use getChopAudioCache().get(id)
    // to fetch the latest baked buffer for a chop; falls back to source-buffer
    // playback when the entry is null.
    cuesampler::ChopAudioCache& getChopAudioCache() noexcept { return chopAudioCache; }
    const cuesampler::ChopAudioCache& getChopAudioCache() const noexcept { return chopAudioCache; }

    // Asynchronously re-render a chop's warp cache entry. No-op if the chop
    // doesn't exist or no sample is loaded.
    void requestChopWarpRender (int chopId);

    // Marker editing API. All methods clamp inputs into the chop's range,
    // sort markers by sourceSample, and queue an asynchronous re-bake.
    // Returns the index of the new/updated marker, or -1 on failure.
    int  addOrUpdateChopWarpMarker (int chopId, int sourceSample, double localTimeSeconds, bool snappedToGrid = true);
    bool setChopWarpMarkerLocalTime (int chopId, int markerIndex, double localTimeSeconds, bool snappedToGrid = true);
    bool setChopWarpMarkerSourceSample (int chopId, int markerIndex, int sourceSample);
    bool removeChopWarpMarker (int chopId, int markerIndex);
    bool clearChopWarpMarkers (int chopId);

    // One-shot snap of a single marker to a specified division (overriding the
    // active default division). Returns false if the chop / marker / analysis
    // is not available.
    bool snapChopWarpMarkerToDivision (int chopId, int markerIndex, int divisionIndex);

    // ---- Warp UI mode -----------------------------------------------------
    // Editor-side toggle: when true, the waveform suppresses click-to-preview
    // and routes clicks to marker placement (wired in step 9). The flag lives
    // on the processor so the transport bar and waveform stay in sync without
    // bespoke callback plumbing.
    enum WarpDivision : int
    {
        WarpDivision_Bar       = 0,
        WarpDivision_Beat      = 1,
        WarpDivision_HalfBeat  = 2,
        WarpDivision_Sixteenth = 3
    };

    bool isWarpModeActive() const noexcept { return warpModeActive.load (std::memory_order_acquire); }
    void setWarpModeActive (bool active) noexcept { warpModeActive.store (active, std::memory_order_release); }

    int  getWarpDivision() const noexcept { return warpDivision.load (std::memory_order_acquire); }
    void setWarpDivision (int division) noexcept;

    // Division as a duration in seconds, derived from the latest tempo
    // analysis (with grid trim applied). Returns 0.0 if no analysis exists.
    double getWarpDivisionSeconds() const noexcept;

    // Fingerprint of the current global grid state. Markers stamp this at
    // snap time; if the current fingerprint diverges, the marker is stale.
    // Currently encodes the active beat period (after BPM trim).
    double getCurrentGridFingerprint() const noexcept;

    void setChopBarsCount (int bars);
    void setTempoAnalysisRegion (int startSample, int endSample);
    void clearTempoAnalysisRegion();
    void addChop (int startSample, int endSample);

    // Replaces the chop list with one chop per detected transient onset.
    // Light favors heavy hits only (sparser markers); Medium captures more
    // moderate hits as well. Runs on the calling thread; returns the number
    // of chops created (0 if no sample is loaded or nothing was detected).
    enum class TransientSensitivity : int { Light = 0, Medium = 1, Fine = 2 };
    int chopAtTransients (TransientSensitivity sensitivity);

    void selectChopAtSample (double samplePosition);
    void clearSelectedChop();
    void removeSelectedChop();
    void setSelectedChopCueNormalized (float normalizedValue);
    void setSelectedChopGainDecibels (float gainDecibels);
    void setSelectedChopPitchSemitones (float pitchSemitones);
    void toggleSelectedChopFavorite();

    // Edit undo. Snapshots the chop list + grid trim/offset/bars before each
    // destructive chop edit so the user can step back through their changes.
    // (DAW-level parameter automation is undone by the host; this covers the
    // non-parameter chop/grid edit state.) Message-thread facing.
    bool canUndoEdit() const noexcept;
    void undoLastEdit();

    // Renders a chop to a 24-bit WAV in the OS temp directory with all parameters baked in.
    // applySync stretches the audio to match the current DAW tempo when sync is enabled.
    // Called from the message thread. Returns an invalid File on failure.
    juce::File renderChopToTempWav (int chopId, bool applySync);

    juce::String loadedFileName;
    double sampleSampleRate = 0.0;
    juce::ChangeBroadcaster sampleChangeBroadcaster;
    juce::ChangeBroadcaster editChangeBroadcaster;

private:
    class TempoAnalysisJob;
    class DeferredSampleRestoreJob;
    class WarpRenderJob;
    class KeyDetectionJob;
    class PreparedWarmJob;
    class StemSeparationJob;
    class RemixJob;

    // Background "pre-render" warm: bakes each non-warp chop's pitch+time-stretched
    // audio into the ChopAudioCache prepared-entry cache so the audio thread can
    // stream it 1:1 instead of running Bungee in real time. Polled from a message-
    // thread timer (editor-independent) so it keeps the cache warm even with the
    // UI closed. See CachePollerTimer / PreparedWarmJob.
    void warmPreparedCacheTick();

    // Per-voice Bungee stretcher/stream wrapper. Defined in the .cpp so the
    // bungee headers (and Eigen) don't need to be visible to consumers of this
    // header (e.g. PluginEditor.cpp). VoiceState below holds a unique_ptr to
    // an instance and we provide an out-of-line destructor so unique_ptr can
    // be instantiated against this incomplete type.
    struct VoicePitchEngine;
    struct VoicePitchEngineSet;

    struct DeferredRestoreStateData
    {
        bool hasExplicitSampleState = false;
        bool hasSavedSample = false;
        juce::String samplePath;
        juce::String sampleFileName;
        juce::MemoryBlock embeddedSampleData;
        std::shared_ptr<TempoEditState> editState;
        std::shared_ptr<TempoAnalysisData> analysis;
        std::shared_ptr<ChopState> chopState;
        float restoredGridBpmTrim = 0.0f;
        float restoredGridStartOffset = 0.0f;
        float restoredWaveformZoom = 0.25f;
        float restoredWaveformScroll = 0.0f;
        float restoredGlobalPitch = 0.0f;
        bool restoredSyncToHost = false;
        bool restoredHalfTime = false;
        bool restoredMuteDrums = false;
        bool restoredMuteBass = false;
        bool restoredMuteVocals = false;
        int restoredBarsPerChop = 1;
        int restoredMidiOctaveOffset = 0;
        double restoredPlaybackPosition = 0.0;
    };

    std::unique_ptr<BeatThisAnalyzer> beatThisAnalyzer;
    std::unique_ptr<StemSeparator>    stemSeparator;
    KeyDetector keyDetector;
    AudioFingerprinter audioFingerprinter;
    cuesampler::EditTelemetry editTelemetry;
    cuesampler::UpdateChecker updateChecker;
    KeyDetector::Result detectedKeyResult;
    mutable std::mutex keyResultMutex;

    class CachePollerTimer;
    std::unique_ptr<CachePollerTimer> cachePollerTimer;

    struct VoiceState
    {
        double playbackSamplePosition = 0.0;
        double playbackStopSample = -1.0;
        // Set by transport Play; when >= 0, the render loop wraps back to this
        // sample after hitting playbackStopSample / sourceLength instead of
        // deactivating. MIDI-triggered voices leave it at -1 (one-shot).
        double playbackLoopStartSample = -1.0;
        bool playbackActive = false;
        bool bungeeResetPending = true;
        float midiVelocity = 1.0f;
        bool playbackTriggeredByMidi = false;

        double playbackSyncStartPpq = 0.0;
        double playbackSyncStartSample = 0.0;
        bool playbackSyncAnchorValid = false;

        float fadeGain = 1.0f;
        float fadeTarget = 1.0f;
        float fadeStep = 0.0f;

        uint64_t observedPitchEngineGeneration = 0;

        VoiceState();
        ~VoiceState();
        VoiceState (const VoiceState&) = delete;
        VoiceState& operator= (const VoiceState&) = delete;

        void reset()
        {
            playbackActive = false;
            playbackSamplePosition = 0.0;
            playbackStopSample = -1.0;
            playbackLoopStartSample = -1.0;
            playbackTriggeredByMidi = false;
            bungeeResetPending = true;
            fadeGain = 1.0f;
            fadeTarget = 1.0f;
            fadeStep = 0.0f;
            playbackSyncAnchorValid = false;
            observedPitchEngineGeneration = 0;
        }

        void startFade (float target, double durationSamples)
        {
            fadeTarget = target;
            if (durationSamples > 0)
                fadeStep = (fadeTarget - fadeGain) / (float) durationSamples;
            else
            {
                fadeGain = fadeTarget;
                fadeStep = 0.0f;
            }
        }

        void updateFade()
        {
            if (std::abs (fadeGain - fadeTarget) > std::abs (fadeStep))
                fadeGain += fadeStep;
            else
            {
                fadeGain = fadeTarget;
                fadeStep = 0.0f;
            }
        }
    };

    std::shared_ptr<LoadedSampleData> loadedSample;
    std::shared_ptr<const StemSet> stemSet;
    std::shared_ptr<TempoAnalysisData> tempoAnalysis;
    std::shared_ptr<TempoEditState> tempoEditState;
    std::shared_ptr<ChopState> chopState;

    // Undo history for chop/grid edits. Each snapshot is the full editable
    // state captured *before* a destructive edit. Guarded by editUndoLock so
    // it can be cleared from the (possibly background) sample-restore path as
    // well as the message thread.
    struct EditUndoSnapshot
    {
        std::shared_ptr<ChopState> chopState;
        float gridBpmTrim = 0.0f;
        float gridStartOffset = 0.0f;
        int   chopBarsCount = 1;
    };

    // Captures the current edit state before a mutation. A non-empty
    // coalesceKey collapses a rapid run of same-kind edits (e.g. a knob/edge
    // drag) into a single undo step.
    void pushEditUndoSnapshot (const juce::String& coalesceKey);
    void clearEditUndoHistory();

    mutable juce::CriticalSection editUndoLock;
    std::vector<EditUndoSnapshot> editUndoStack;
    juce::String editUndoCoalesceKey;
    double editUndoLastSnapshotMs = 0.0;
    static constexpr size_t maxEditUndoDepth = 64;
    static constexpr double editUndoCoalesceWindowMs = 600.0;

    VoiceState voices[2];
    int activeVoiceIdx = 0;


    cuesampler::ChopAudioCache chopAudioCache;

    // Bumped on every warm kick so an in-flight PreparedWarmJob can detect it has
    // been superseded (params changed) and abort early. Read on the worker thread.
    std::atomic<uint64_t> prepareWarmGeneration { 0 };
    // Message-thread-only state used by warmPreparedCacheTick() to decide whether a
    // fresh warm is needed (avoids re-kicking when nothing relevant has changed).
    std::shared_ptr<const ChopState> lastWarmChopState;
    // Also tracked so a stem mute (which swaps loadedSample to a new buffer with
    // the SAME chopState) forces a prepared-cache re-warm against the new audio.
    std::shared_ptr<const LoadedSampleData> lastWarmSample;
    bool   lastWarmValid       = false;
    int    lastWarmPitchCents  = 0;
    int    lastWarmStretchPpm  = 0;
    double lastWarmSourceRate  = 0.0;
    double lastWarmHostRate    = 0.0;
    std::shared_ptr<const VoicePitchEngineSet> pitchEngineSet;

    enum class TransportCommand : int
    {
        none = 0,
        start,
        pause,
        stop,
        silence,
        seek
    };

    std::atomic<int> pendingTransportCommand { (int) TransportCommand::none };
    std::atomic<double> pendingTransportSeekPosition { 0.0 };
    std::atomic<double> pendingStartPosition { 0.0 };
    std::atomic<double> pendingStartStopSample { -1.0 };
    std::atomic<double> pendingStartLoopSample { -1.0 };
    std::atomic<double> pendingStartSyncPpq { 0.0 };
    std::atomic<double> pendingStartSyncSample { 0.0 };
    std::atomic<bool> pendingStartSyncAnchorValid { false };
    std::atomic<uint64_t> pitchEngineGeneration { 0 };
    std::atomic<uint64_t> warpRenderGeneration { 0 };
    std::atomic<bool> warpModeActive { false };
    std::atomic<int>  warpDivision   { WarpDivision_Beat };
    std::atomic<uint64_t> restoreGeneration { 0 };
    std::atomic<uint64_t> tempoAnalysisGeneration { 0 };
    std::atomic<uint64_t> keyDetectionGeneration { 0 };
    std::atomic<bool> tempoAnalysisInProgress { false };
    std::atomic<bool> keyDetectionInProgress { false };

    // Stem-separation state. stemGeneration guards background separation jobs;
    // stemRemixGeneration coalesces rapid mute toggles. appliedStemMask is the
    // mute bitmask (drums=1|bass=2|vocals=4) currently baked into loadedSample,
    // or -1 when loadedSample is the raw original (equivalent to mask 0).
    std::atomic<uint64_t> stemGeneration { 0 };
    std::atomic<uint64_t> stemRemixGeneration { 0 };
    std::atomic<bool>  muteDrums { false };
    std::atomic<bool>  muteBass { false };
    std::atomic<bool>  muteVocals { false };
    std::atomic<bool>  stemsReady { false };
    std::atomic<bool>  stemSeparationInProgress { false };
    std::atomic<bool>  stemSeparationSkipped { false }; // sample exceeded the length guard
    std::atomic<float> stemProgress { 0.0f };
    std::atomic<int>   appliedStemMask { -1 };
    // Resolved once in the constructor (cheap path probe); empty when no htdemucs
    // model file is installed. Never mutated afterwards, so it is safe to read
    // from both the message thread (areStemModelsAvailable) and the stem thread
    // (ensureStemSeparatorLoaded) without a lock. The StemSeparator/ONNX session
    // itself is built lazily from this path on the first separation request.
    juce::String       stemModelPath;
    std::atomic<bool>  stemModelLoading { false };    // session being built on the stem thread
    std::atomic<bool>  stemModelLoadFailed { false }; // model file present but session build failed
    // Samples longer than this skip separation (avoids huge RAM/time on full songs).
    static constexpr double kMaxStemSeparationSeconds = 600.0; // 10 minutes
    std::atomic<uint64_t> tempoUiRevision { 0 };
    std::atomic<double> hostSampleRate { 44100.0 };
    std::atomic<double> playbackSamplePosition { 0.0 };
    std::atomic<bool> playbackActive { false };
    std::atomic<int> lastTriggeredChopId { -1 };
    std::atomic<uint64_t> chopTriggerRevision { 0 };
    std::atomic<float> outputMeterLevel { 0.0f };
    std::atomic<float> timeStretchRatio { 1.0f };
    std::atomic<float> pitchSemitones { 0.0f };
    std::atomic<double> hostBpm { 0.0 };
    std::atomic<double> hostPpqPosition { 0.0 };
    std::atomic<bool> hostHasPpqPosition { false };
    std::atomic<bool> hostTransportPlaying { false };
    std::atomic<bool> syncToHost { false };
    std::atomic<bool> halfTimeEnabled { false };
    std::atomic<float> gridBpmTrim { 0.0f };
    std::atomic<int> chopBarsCount { 1 };
    std::atomic<int> heldMidiNote { -1 };
    std::atomic<int> midiOctaveOffset { 0 };
    std::atomic<float> gridStartOffset { 0.0f };
    std::atomic<float> waveformZoom { 0.25f };
    std::atomic<float> waveformScroll { 0.0f };

    cuesampler::SSLBusCompressor compressor;
    std::atomic<float> compressorThresholdUiDb { 0.0f };
    std::atomic<float> compressorMakeupUiDb    { 0.0f };
    std::atomic<bool>  compressorEnabledUi     { false };

    cuesampler::BitCrusher bitCrusher;

    // CUERACK master chain (post sample/stem mix; replaces the legacy
    // crusher + compressor pair above, whose modules now live in the rack).
    cue::FxEngine fxEngine { apvts };
    std::atomic<float> bitCrusherBitsUi    { 0.0f };
    std::atomic<float> bitCrusherCrushUi   { 0.0f };
    std::atomic<bool>  bitCrusherEnabledUi { false };

    // Rolling mono window of the crushed output: the audio thread appends into
    // the ring, then publishes a chronological copy into the atomic array the
    // editor reads. Sized by kBitCrusherScopeSize.
    std::array<float, kBitCrusherScopeSize>              bitCrusherScopeRing {};
    int                                                  bitCrusherScopeWritePos = 0;
    std::array<std::atomic<float>, kBitCrusherScopeSize> bitCrusherScope {};

    // (Re)build the per-voice Bungee stretcher/stream pair so it matches the
    // currently known source sample rate, host sample rate, and channel
    // count. No-op when nothing has changed. Always called from the message
    // thread; safe to call before a sample is loaded (in which case it just
    // tears down any existing engine and waits for a sample).
    void ensurePitchEnginesReady (double sourceRate, double hostRate, int channels);

    void launchTempoAnalysis (std::shared_ptr<const LoadedSampleData> sampleData);
    void publishTempoAnalysis (std::shared_ptr<TempoAnalysisData> analysisResult, uint64_t analysisGeneration);

    // Stem separation, mirroring the launchTempoAnalysis / publishTempoAnalysis
    // pattern. launchStemSeparation resets stem state and kicks a background
    // pass; publishStems installs the result (generation-guarded) and applies any
    // active mutes; rebuildActiveMix swaps loadedSample to original − Σ(muted);
    // scheduleRebuildActiveMix posts a coalesced rebuild off the message thread.
    // resetStemState clears stems/flags WITHOUT launching a pass — used when a new
    // sample loads (separation is now user-initiated via requestStemSeparation).
    void resetStemState();
    void launchStemSeparation (std::shared_ptr<LoadedSampleData> sampleData);
    // Builds the StemSeparator/ONNX session on first use. MUST be called from the
    // stem thread (single-threaded pool): the model build + probe is the heavy,
    // deferred cost. Returns the ready separator, or nullptr if no model is
    // installed or the session failed to build.
    StemSeparator* ensureStemSeparatorLoaded();
    void publishStems (std::shared_ptr<const StemSet> newStemSet, uint64_t generation);
    void rebuildActiveMix();
    void scheduleRebuildActiveMix();
    void buildChopsFromAnalysis (const TempoAnalysisData& analysis);
    DeferredRestoreStateData parseDeferredRestoreState (const juce::ValueTree& state) const;
    void applyParsedRestoreState (const DeferredRestoreStateData& restoreState);
    void completeDeferredSampleRestore (const DeferredRestoreStateData& restoreState,
                                        std::shared_ptr<LoadedSampleData> restoredSample,
                                        uint64_t completedRestoreGeneration);
    bool serializeSampleToStateData (const LoadedSampleData& sampleData, juce::MemoryBlock& stateData) const;
    std::shared_ptr<LoadedSampleData> createLoadedSampleDataFromStateData (const juce::MemoryBlock& stateData,
                                                                           const juce::String& fileName);
    std::shared_ptr<LoadedSampleData> createLoadedSampleDataFromFile (const juce::File& file);
    KeyDetector::Result tryReadKeyFromMetadata (const juce::File& file);
    bool computeGridTimingMetrics (const TempoAnalysisData& analysis,
                                   double& beatPeriodSeconds,
                                   double& barPeriodSeconds,
                                   double& chopPeriodSeconds,
                                   double& gridAnchorSeconds) const noexcept;
    double getAdjustedAnalysisBpm (const TempoAnalysisData& analysis) const noexcept;
    void updateHostSyncStretchRatio (const TempoAnalysisData& analysis, double currentHostBpm) noexcept;
    void touchTempoUiRevision() noexcept;
    void requestHostStateSync();
    void notifyEditStateChanged();
    void handleAsyncUpdate() override;
    void handleMidiEvent (const juce::MidiMessage& msg, 
                          int sampleOffset, 
                          const std::shared_ptr<ChopState>& currentChopState,
                          double currentHostRate,
                          bool blockHasHostPpq,
                          bool blockHostTransportPlaying,
                          double blockStartHostPpq);

    // Thread pools moved to the end of the class so they are destroyed first.
    juce::ThreadPool restoreThreadPool { 1 };
    juce::ThreadPool analysisThreadPool { 1 };
    juce::ThreadPool warpRenderThreadPool { 1 };
    juce::ThreadPool keyDetectionThreadPool { 1 };
    // Offline HTDemucs-FT separation + mute remixes. Single thread (one pass at a
    // time), default priority — below the host's realtime audio thread so it can
    // never glitch playback. Runs both StemSeparationJob and RemixJob.
    juce::ThreadPool stemThreadPool { 1 };
    // Higher priority so the background bake finishes fast — and is biased onto a
    // performance core on Apple Silicon instead of a slow efficiency core. Still
    // below the host's real-time audio thread, so it cannot glitch playback.
    juce::ThreadPool prepareRenderThreadPool { 1, juce::Thread::osDefaultStackSize,
                                               juce::Thread::Priority::high };

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
