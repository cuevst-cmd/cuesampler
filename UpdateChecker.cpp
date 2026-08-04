#include "UpdateChecker.h"

#ifndef CUE_VERSION_STRING
 #define CUE_VERSION_STRING "0.0.0"
#endif

namespace cuesampler
{
namespace
{
constexpr int updateCacheSchema = 2;

const char* installerExtensionForCurrentPlatform() noexcept
{
   #if JUCE_WINDOWS
    return ".exe";
   #elif JUCE_MAC
    return ".pkg";
   #else
    return nullptr;
   #endif
}

const char* updateCachePlatform() noexcept
{
   #if JUCE_WINDOWS
    return "windows";
   #elif JUCE_MAC
    return "macos";
   #else
    return "unsupported";
   #endif
}
} // namespace

struct UpdateChecker::SharedState
{
    std::mutex mutex;
    std::atomic<bool> available { false };
    std::atomic<int> checkStatus { (int) CheckStatus::idle };
    Result current;
    juce::String skippedVersion;
    juce::int64 lastCheckMs = 0;
    bool cacheLoaded = false;
    bool checkRunning = false;
};

UpdateChecker::SharedState& UpdateChecker::sharedState()
{
    static SharedState state;
    return state;
}

// Background worker: one network check, then exits. The once-a-day throttle is
// persisted, so there is no reason to loop within a session.
class UpdateChecker::CheckThread final : public juce::Thread
{
public:
    explicit CheckThread (UpdateChecker& ownerIn)
        : juce::Thread ("CueUpdateCheck"), owner (ownerIn) {}

    void run() override
    {
        if (! threadShouldExit())
            owner.performCheck();
        else
            owner.markCheckFinished();
    }

private:
    UpdateChecker& owner;
};

UpdateChecker::UpdateChecker()
{
    loadCache();
}

UpdateChecker::~UpdateChecker()
{
    if (thread != nullptr)
    {
        thread->stopThread (5000); // signals exit + notify, then joins
        markCheckFinished();
    }
}

juce::String UpdateChecker::currentVersion()
{
    return normaliseVersion (CUE_VERSION_STRING);
}

void UpdateChecker::start()
{
    beginCheck (false);
}

void UpdateChecker::checkNow()
{
    auto& state = sharedState();
    bool clearedSkippedVersion = false;

    {
        const std::lock_guard<std::mutex> lock (state.mutex);
        if (state.skippedVersion.isNotEmpty())
        {
            state.skippedVersion.clear();
            clearedSkippedVersion = true;
        }
    }

    if (clearedSkippedVersion)
        saveCache();

    beginCheck (true);
}

bool UpdateChecker::beginCheck (bool bypassThrottle)
{
    // publish() completes just before CheckThread::run() returns. Avoid claiming
    // a new request in that tiny window; a second click can restart immediately
    // once JUCE reports the worker as stopped.
    if (thread != nullptr && thread->isThreadRunning())
        return false;

    auto& state = sharedState();

    // Re-eval cache against the live version first, in case
    // the user updated since the cache was written.
    {
        const std::lock_guard<std::mutex> lock (state.mutex);
        state.current.available = isNewer (state.current.latestVersion, currentVersion())
                                  && state.current.latestVersion != state.skippedVersion;
        state.available.store (state.current.available, std::memory_order_release);

        const auto now = juce::Time::currentTimeMillis();
        if (state.checkRunning)
            return false; // share the check already in flight

        if (! bypassThrottle
            && state.lastCheckMs != 0
            && (now - state.lastCheckMs) < kThrottleMs)
            return false; // checked recently — cached result stands

        state.checkRunning = true;
        state.checkStatus.store ((int) CheckStatus::checking, std::memory_order_release);
        ownsRunningCheck.store (true, std::memory_order_release);
    }

    if (thread == nullptr)
        thread = std::make_unique<CheckThread> (*this);

    if (! thread->isThreadRunning())
        thread->startThread();

    return true;
}

UpdateChecker::Result UpdateChecker::getResult() const
{
    auto& state = sharedState();
    const std::lock_guard<std::mutex> lock (state.mutex);
    return state.current;
}

bool UpdateChecker::isUpdateAvailable() const noexcept
{
    return sharedState().available.load (std::memory_order_acquire);
}

UpdateChecker::CheckStatus UpdateChecker::getCheckStatus() const noexcept
{
    return (CheckStatus) sharedState().checkStatus.load (std::memory_order_acquire);
}

void UpdateChecker::skipCurrentVersion()
{
    auto& state = sharedState();
    {
        const std::lock_guard<std::mutex> lock (state.mutex);
        state.skippedVersion    = state.current.latestVersion;
        state.current.available = false;
        state.available.store (false, std::memory_order_release);
    }
    saveCache();
}

void UpdateChecker::performCheck()
{
    bool shouldPublish = false;
    Result newestForPlatform;

    do
    {
        juce::URL url (kApiUrl);

        // GitHub's API rejects requests without a User-Agent (HTTP 403). The
        // Accept header pins the response schema. No auth token is sent.
        juce::WebInputStream req (url, /*addParametersToRequestBody*/ false);
        req.withExtraHeaders ("User-Agent: CueSampler-Updater\r\n"
                              "Accept: application/vnd.github+json\r\n")
           .withConnectionTimeout (8000)
           .withNumRedirectsToFollow (5);

        if (! req.connect (nullptr))
            break;

        const int status = req.getStatusCode();
        const auto body  = req.readEntireStreamAsString(); // drain before teardown
        if (status < 200 || status >= 300)
            break;

        const auto parsed = juce::JSON::parse (body);
        auto* releases = parsed.getArray();
        if (releases == nullptr)
            break;

        // Require a direct installer for this platform. The exact suffix does
        // not match checksum sidecars such as ".pkg.sha256".
        const auto* installerExt = installerExtensionForCurrentPlatform();

        if (installerExt == nullptr)
            break;

        for (const auto& releaseValue : *releases)
        {
            auto* release = releaseValue.getDynamicObject();
            if (release == nullptr
                || (bool) release->getProperty ("draft")
                || (bool) release->getProperty ("prerelease"))
                continue;

            Result candidate;
            candidate.latestVersion = normaliseVersion (release->getProperty ("tag_name").toString());
            if (candidate.latestVersion.isEmpty())
                continue;

            const auto assetsValue = release->getProperty ("assets");
            if (auto* assets = assetsValue.getArray())
            {
                for (const auto& assetValue : *assets)
                {
                    if (auto* asset = assetValue.getDynamicObject())
                    {
                        const auto name = asset->getProperty ("name").toString();
                        if (name.endsWithIgnoreCase (installerExt))
                        {
                            candidate.downloadUrl = asset->getProperty ("browser_download_url").toString();
                            break;
                        }
                    }
                }
            }

            // A release for the other OS is not an update for this instance.
            if (candidate.downloadUrl.isEmpty())
                continue;

            candidate.pageUrl = release->getProperty ("html_url").toString();
            candidate.notes   = release->getProperty ("body").toString();

            if (newestForPlatform.latestVersion.isEmpty()
                || isNewer (candidate.latestVersion, newestForPlatform.latestVersion))
                newestForPlatform = std::move (candidate);
        }

        // A valid releases array is a successful check even when this platform
        // has no installer yet. Caching that result preserves the daily throttle.
        shouldPublish = true;
    }
    while (false);

    if (shouldPublish)
        publish (std::move (newestForPlatform), /*fromNetwork*/ true);
    else
        markCheckFinished();
}

void UpdateChecker::publish (Result result, bool fromNetwork)
{
    std::function<void()> notify;
    auto& state = sharedState();

    {
        const std::lock_guard<std::mutex> lock (state.mutex);

        result.available = isNewer (result.latestVersion, currentVersion())
                           && result.latestVersion != state.skippedVersion;

        const bool firstTimeAvailable = result.available && ! state.current.available;

        state.current = std::move (result);
        if (fromNetwork)
            state.lastCheckMs = juce::Time::currentTimeMillis();
        state.checkRunning = false;

        state.available.store (state.current.available, std::memory_order_release);
        state.checkStatus.store ((int) (state.current.available ? CheckStatus::updateAvailable
                                                                : CheckStatus::upToDate),
                                 std::memory_order_release);
        ownsRunningCheck.store (false, std::memory_order_release);

        if (firstTimeAvailable && onUpdateAvailable)
            notify = onUpdateAvailable; // copy under lock; fire outside
    }

    if (fromNetwork)
        saveCache();

    if (notify)
        juce::MessageManager::callAsync ([notify] { notify(); });
}

void UpdateChecker::markCheckFinished()
{
    auto& state = sharedState();
    const std::lock_guard<std::mutex> lock (state.mutex);
    if (! ownsRunningCheck.exchange (false, std::memory_order_acq_rel))
        return;

    state.checkRunning = false;
    state.checkStatus.store ((int) CheckStatus::failed, std::memory_order_release);
}

juce::File UpdateChecker::baseDir() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("CueSampler");
    dir.createDirectory();
    return dir;
}

juce::File UpdateChecker::cacheFile() const { return baseDir().getChildFile ("update_check.json"); }

void UpdateChecker::loadCache()
{
    auto& state = sharedState();
    const std::lock_guard<std::mutex> lock (state.mutex);

    if (state.cacheLoaded)
        return;
    state.cacheLoaded = true;

    const auto file = cacheFile();
    if (! file.existsAsFile())
        return;

    const auto parsed = juce::JSON::parse (file);
    if (auto* obj = parsed.getDynamicObject())
    {
        if ((int) obj->getProperty ("schema") != updateCacheSchema
            || obj->getProperty ("platform").toString() != updateCachePlatform())
            return; // discard legacy/non-platform-aware cache entries

        state.current.latestVersion = obj->getProperty ("latestVersion").toString();
        state.current.downloadUrl   = obj->getProperty ("downloadUrl").toString();
        state.current.pageUrl       = obj->getProperty ("pageUrl").toString();
        state.current.notes         = obj->getProperty ("notes").toString();
        state.skippedVersion        = obj->getProperty ("skippedVersion").toString();
        state.lastCheckMs           = (juce::int64) (double) obj->getProperty ("lastCheckMs");
    }
}

void UpdateChecker::saveCache()
{
    auto& state = sharedState();
    const std::lock_guard<std::mutex> lock (state.mutex);

    auto obj = juce::DynamicObject::Ptr (new juce::DynamicObject());
    obj->setProperty ("schema", updateCacheSchema);
    obj->setProperty ("platform", updateCachePlatform());
    obj->setProperty ("latestVersion", state.current.latestVersion);
    obj->setProperty ("downloadUrl",   state.current.downloadUrl);
    obj->setProperty ("pageUrl",       state.current.pageUrl);
    obj->setProperty ("notes",         state.current.notes);
    obj->setProperty ("skippedVersion", state.skippedVersion);
    obj->setProperty ("lastCheckMs",   (double) state.lastCheckMs);
    cacheFile().replaceWithText (juce::JSON::toString (juce::var (obj.get())));
}

juce::String UpdateChecker::normaliseVersion (juce::String raw)
{
    raw = raw.trim();
    if (raw.startsWithIgnoreCase ("v"))
        raw = raw.substring (1);
    // Drop any pre-release / build suffix: "0.1.0-beta.2" -> "0.1.0".
    raw = raw.upToFirstOccurrenceOf ("-", false, false);
    raw = raw.upToFirstOccurrenceOf ("+", false, false);
    return raw.trim();
}

bool UpdateChecker::isNewer (const juce::String& candidate, const juce::String& current)
{
    const auto a = normaliseVersion (candidate);
    const auto b = normaliseVersion (current);
    if (a.isEmpty())
        return false;

    auto parts = [] (const juce::String& v)
    {
        juce::StringArray tokens;
        tokens.addTokens (v, ".", "");
        juce::Array<int> nums;
        for (const auto& t : tokens)
            nums.add (t.getIntValue());
        return nums;
    };

    const auto av = parts (a);
    const auto bv = parts (b);
    const int n = juce::jmax (av.size(), bv.size());
    for (int i = 0; i < n; ++i)
    {
        const int x = i < av.size() ? av[i] : 0;
        const int y = i < bv.size() ? bv[i] : 0;
        if (x != y)
            return x > y;
    }
    return false; // equal
}

} // namespace cuesampler
