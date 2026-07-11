#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <mutex>
#include <string>

namespace cuesampler
{

// Local, opt-in "data flywheel" recorder for the two detections we want to
// improve: BPM and musical key. It captures the gap between what the analysis
// algorithms guessed and what the authoritative source (online lookup / file
// metadata) or the user settled on, keyed by an anonymous content fingerprint.
//
// Privacy contract (do not weaken without a fresh consent prompt):
//   * Never stores audio, file paths, or file names.
//   * Stores a *hash* of the Chromaprint fingerprint, not the fingerprint.
//   * Disabled by default; only collects after setEnabled(true).
//   * Writes newline-delimited JSON to a file in the user's app-data dir; the
//     network upload step is intentionally out of scope here.
//
// Threading: recordBpmCorrection() is expected on the message thread (it owns
// the coalescing Timer). setSampleContext()/recordKeyObservation() may be
// called from the key-detection background thread. All shared state is guarded
// by a mutex; file appends are serialized through the same lock.
class EditTelemetry
{
public:
    EditTelemetry();
    ~EditTelemetry();

    // --- Consent -----------------------------------------------------------
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_acquire); }
    void setEnabled (bool shouldEnable);

    // Stable, anonymous per-install id (random UUID). Useful later for
    // de-duplicating events server-side without identifying the user.
    juce::String getInstallId() const;

    // --- Sample lifecycle --------------------------------------------------
    // Call when a new sample's Chromaprint fingerprint is known. The raw
    // fingerprint string is hashed before storage. Flushes any pending event
    // for the previous sample first.
    void setSampleContext (const std::string& chromaprintFingerprint,
                           double durationSeconds,
                           double sampleRate,
                           int numChannels);

    // Call when a sample is unloaded / replaced. Flushes pending events and
    // clears the per-sample dedupe state.
    void clearSampleContext();

    // --- Signals -----------------------------------------------------------
    // BPM correction. Coalesced: rapid calls (e.g. a slider drag) keep only
    // the latest value, which is written once the value settles. A trim of
    // zero (algorithm confirmed correct) is still recorded — that is signal.
    void recordBpmCorrection (double algorithmBpm,
                              double userBpm,
                              float analysisConfidence,
                              bool likelyDrifting);

    // Key observation, logged once per detection pass. metadataKey / onlineKey
    // may be empty when unavailable; when present they act as a weak ground
    // truth label for the local FFT guess.
    void recordKeyObservation (const std::string& localKey,
                               float localConfidence,
                               const std::string& metadataKey,
                               const std::string& onlineKey);

    // User corrected the displayed key. userKey is ground truth; detectedKey is
    // whatever the plugin had shown. This is the strongest key label we get.
    void recordKeyCorrection (const std::string& detectedKey,
                              float detectedConfidence,
                              const std::string& userKey);

    // Force-write any pending coalesced event immediately.
    void flush();

    // Start/stop the background upload loop (one upload shortly after launch,
    // then every kUploadIntervalMs). No-op when no endpoint is configured or
    // telemetry is disabled. Called automatically by setEnabled().
    void startUploads();
    void stopUploads();

private:
    class UploadThread;
    friend class UploadThread;

    class BpmCoalesceTimer;
    std::unique_ptr<BpmCoalesceTimer> bpmTimer;
    void flushPendingBpmFromTimer();

    void writeEvent (const juce::String& type, juce::DynamicObject::Ptr fields);
    void writeEventLocked (const juce::String& type, juce::DynamicObject::Ptr fields);
    void flushPendingBpmLocked();

    // Background-thread body: stages the current events file and POSTs it to
    // the endpoint. Never holds `mutex` during the network call.
    void performUpload();
    bool postBatch (const juce::String& payload);

    juce::File baseDir() const;
    juce::File logFile() const;
    juce::File uploadingFile() const;
    juce::File settingsFile() const;
    void loadSettings();
    void saveSettings();

    static juce::String hashFingerprint (const std::string& fingerprint);

    mutable std::mutex mutex;
    std::atomic<bool> enabled { false };

    juce::String installId;
    juce::String fingerprintId;   // hashed fingerprint of the current sample
    double sampleDurationSeconds = 0.0;
    double sampleRate            = 0.0;
    int    sampleChannels        = 0;

    // Pending (coalesced) BPM correction.
    bool   hasPendingBpm   = false;
    double pendingAlgoBpm  = 0.0;
    double pendingUserBpm  = 0.0;
    float  pendingConfidence = 0.0f;
    bool   pendingDrifting   = false;

    // Dedupe: skip writing an identical BPM event for the same sample.
    juce::String lastBpmSignature;

    // Upload configuration (from APIKeys.h) and worker.
    juce::String endpointUrl;
    juce::String sharedSecret;
    std::unique_ptr<UploadThread> uploadThread;

    static constexpr int kBpmCoalesceMs   = 900;
    static constexpr int kUploadIntervalMs = 60 * 1000; // 60s; idle ticks make no network call

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditTelemetry)
};

} // namespace cuesampler
