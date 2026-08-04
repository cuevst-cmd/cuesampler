#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace cuesampler
{

// Checks GitHub Releases for a newer build and surfaces a download link in the
// editor. A loaded plugin binary cannot replace itself while a DAW holds it
// open, so "update" here means: tell the user, then hand off to the platform
// installer in the browser (Windows .exe / macOS notarized .pkg). The actual
// swap happens after they quit the host and run the installer.
//
// Source of truth is the GitHub releases API. The checker scans recent,
// published releases and chooses the newest one containing an installer for
// the running platform. This lets macOS ship before Windows (or vice versa)
// without advertising an unusable update on the other platform. The running
// version is the compile-time CUE_VERSION_STRING (fed from PROJECT_VERSION).
//
// Privacy: a check is an unauthenticated HTTPS GET to api.github.com. It sends
// no install id and no telemetry — only the implicit IP + User-Agent that any
// web request carries. Results are cached on disk and refreshed at most once
// per day (kThrottleMs).
//
// Threading: construct/destruct and every accessor run on the message thread.
// The network fetch runs on one instance-owned background thread, while its
// state is shared process-wide so a DAW restoring many instances performs one
// request and every editor sees the same result.
class UpdateChecker
{
public:
    enum class CheckStatus
    {
        idle = 0,
        checking,
        upToDate,
        updateAvailable,
        failed
    };

    struct Result
    {
        bool         available = false;   // a newer, non-skipped version exists
        juce::String latestVersion;        // e.g. "0.1.0" (leading 'v' stripped)
        juce::String downloadUrl;          // direct installer for this OS (.exe / .pkg), when present
        juce::String pageUrl;              // release html page (notes / fallback)
        juce::String notes;                // release body text
    };

    UpdateChecker();
    ~UpdateChecker();

    // Loads any cached result, then kicks a background refresh when the cache is
    // older than the once-a-day throttle. Safe to call more than once; only one
    // network check runs at a time.
    void start();

    // User-requested refresh. Bypasses the daily throttle, re-enables a version
    // previously dismissed with LATER, and still joins any process-wide check
    // already in flight instead of starting a duplicate request.
    void checkNow();

    // Snapshot of the most recent known result (cache or fresh network).
    Result getResult() const;

    // True once a newer, non-skipped version is known. Cheap; poll from the UI.
    bool isUpdateAvailable() const noexcept;
    CheckStatus getCheckStatus() const noexcept;

    // User chose "remind me later": stop nagging about this exact version until
    // a newer one ships. Persisted across sessions.
    void skipCurrentVersion();

    // Optional: fired on the message thread when a fresh check first finds an
    // update. The editor may use this instead of polling. Lifetime is the
    // caller's responsibility — clear it before the listener is destroyed.
    std::function<void()> onUpdateAvailable;

    // The version this binary was built as (CUE_VERSION_STRING).
    static juce::String currentVersion();

    // Compare two "x.y.z" strings (leading 'v' and any "-suffix" ignored).
    // Returns true when `candidate` is strictly newer than `current`.
    static bool isNewer (const juce::String& candidate, const juce::String& current);

private:
    class CheckThread;
    friend class CheckThread;
    struct SharedState;

    void performCheck();                 // background-thread body
    void publish (Result result, bool fromNetwork);
    void markCheckFinished();
    bool beginCheck (bool bypassThrottle);

    static SharedState& sharedState();

    juce::File baseDir() const;
    juce::File cacheFile() const;
    void loadCache();
    void saveCache();

    static juce::String normaliseVersion (juce::String raw);

    std::unique_ptr<CheckThread> thread;
    std::atomic<bool> ownsRunningCheck { false };

    static constexpr const char* kApiUrl =
        "https://api.github.com/repos/cuevst-cmd/cuesampler/releases?per_page=20";
    static constexpr juce::int64 kThrottleMs = (juce::int64) 24 * 60 * 60 * 1000; // 1 day

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace cuesampler
