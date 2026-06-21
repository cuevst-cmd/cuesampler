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
// Source of truth for "what's newest" is the GitHub API endpoint:
//   https://api.github.com/repos/<owner>/<repo>/releases/latest
// which already excludes drafts and pre-releases. The running version is the
// compile-time CUE_VERSION_STRING (fed from CMake's PROJECT_VERSION).
//
// Privacy: a check is an unauthenticated HTTPS GET to api.github.com. It sends
// no install id and no telemetry — only the implicit IP + User-Agent that any
// web request carries. Results are cached on disk and refreshed at most once
// per day (kThrottleMs).
//
// Threading: construct/destruct and every accessor run on the message thread.
// The network fetch runs on a private background thread; the parsed result is
// published behind `mutex` plus an atomic "available" flag for cheap polling.
class UpdateChecker
{
public:
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

    // Snapshot of the most recent known result (cache or fresh network).
    Result getResult() const;

    // True once a newer, non-skipped version is known. Cheap; poll from the UI.
    bool isUpdateAvailable() const noexcept { return available.load (std::memory_order_acquire); }

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

    void performCheck();                 // background-thread body
    void publish (Result result, bool fromNetwork);

    juce::File baseDir() const;
    juce::File cacheFile() const;
    void loadCache();
    void saveCache();

    static juce::String normaliseVersion (juce::String raw);

    mutable std::mutex mutex;
    std::atomic<bool> available { false };

    Result       current;          // guarded by mutex
    juce::String skippedVersion;   // guarded by mutex
    juce::int64  lastCheckMs = 0;  // guarded by mutex (0 = never)

    std::unique_ptr<CheckThread> thread;

    static constexpr const char* kApiUrl =
        "https://api.github.com/repos/cuevst-cmd/cuesampler/releases/latest";
    static constexpr juce::int64 kThrottleMs = (juce::int64) 24 * 60 * 60 * 1000; // 1 day

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace cuesampler
