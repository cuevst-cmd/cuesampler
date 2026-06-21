#include "UpdateChecker.h"

#ifndef CUE_VERSION_STRING
 #define CUE_VERSION_STRING "0.0.0"
#endif

namespace cuesampler
{

// Background worker: one settle delay, one network check, then exits. The
// once-a-day throttle is persisted, so there is no reason to loop within a
// session — `start()` spins this up again next launch if the cache is stale.
class UpdateChecker::CheckThread final : public juce::Thread
{
public:
    explicit CheckThread (UpdateChecker& ownerIn)
        : juce::Thread ("CueUpdateCheck"), owner (ownerIn) {}

    void run() override
    {
        // Brief settle so we're likely online before the first attempt.
        if (wait (4000))
            return; // stopThread() asked us to bail

        if (! threadShouldExit())
            owner.performCheck();
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
        thread->stopThread (5000); // signals exit + notify, then joins
}

juce::String UpdateChecker::currentVersion()
{
    return normaliseVersion (CUE_VERSION_STRING);
}

void UpdateChecker::start()
{
    // Re-evaluate "available" from the cache against the current binary in case
    // the user updated since the cache was written.
    {
        const std::lock_guard<std::mutex> lock (mutex);
        current.available = isNewer (current.latestVersion, currentVersion())
                            && current.latestVersion != skippedVersion;
        available.store (current.available, std::memory_order_release);

        const auto now = juce::Time::currentTimeMillis();
        if (lastCheckMs != 0 && (now - lastCheckMs) < kThrottleMs)
            return; // checked recently — cached result stands
    }

    if (thread == nullptr)
        thread = std::make_unique<CheckThread> (*this);

    if (! thread->isThreadRunning())
        thread->startThread();
}

UpdateChecker::Result UpdateChecker::getResult() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return current;
}

void UpdateChecker::skipCurrentVersion()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        skippedVersion    = current.latestVersion;
        current.available = false;
    }
    available.store (false, std::memory_order_release);
    saveCache();
}

void UpdateChecker::performCheck()
{
    juce::URL url (kApiUrl);

    // GitHub's API rejects requests without a User-Agent (HTTP 403). The Accept
    // header pins the response schema. No auth token: the repo is public and the
    // 60 req/hr unauthenticated limit is irrelevant for a daily check.
    juce::WebInputStream req (url, /*addParametersToRequestBody*/ false);
    req.withExtraHeaders ("User-Agent: CueSampler-Updater\r\n"
                          "Accept: application/vnd.github+json\r\n")
       .withConnectionTimeout (8000)
       .withNumRedirectsToFollow (5);

    if (! req.connect (nullptr))
        return;

    const int status = req.getStatusCode();
    const auto body  = req.readEntireStreamAsString(); // drain before teardown
    if (status < 200 || status >= 300)
        return;

    const auto parsed = juce::JSON::parse (body);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return;

    Result r;
    r.latestVersion = normaliseVersion (obj->getProperty ("tag_name").toString());
    r.pageUrl       = obj->getProperty ("html_url").toString();
    r.notes         = obj->getProperty ("body").toString();

    // Prefer a direct installer asset FOR THIS PLATFORM so the button starts the
    // download immediately; fall back to the release page when none is attached.
    // Windows ships a .exe (Inno Setup), macOS a notarized .pkg. Note ".exe" does
    // not match the ".exe.sha256" checksum sidecar, so the picker grabs the
    // installer, not its hash file.
   #if JUCE_WINDOWS
    const char* const installerExt = ".exe";
   #elif JUCE_MAC
    const char* const installerExt = ".pkg";
   #else
    const char* const installerExt = nullptr;
   #endif

    const auto assets = obj->getProperty ("assets");
    if (installerExt != nullptr)
    {
        if (auto* arr = assets.getArray())
        {
            for (const auto& a : *arr)
            {
                if (auto* ao = a.getDynamicObject())
                {
                    const auto name = ao->getProperty ("name").toString();
                    if (name.endsWithIgnoreCase (installerExt))
                    {
                        r.downloadUrl = ao->getProperty ("browser_download_url").toString();
                        break;
                    }
                }
            }
        }
    }

    if (r.latestVersion.isEmpty())
        return; // malformed response — keep the existing cache

    publish (std::move (r), /*fromNetwork*/ true);
}

void UpdateChecker::publish (Result result, bool fromNetwork)
{
    std::function<void()> notify;

    {
        const std::lock_guard<std::mutex> lock (mutex);

        result.available = isNewer (result.latestVersion, currentVersion())
                           && result.latestVersion != skippedVersion;

        const bool firstTimeAvailable = result.available && ! current.available;

        current = std::move (result);
        if (fromNetwork)
            lastCheckMs = juce::Time::currentTimeMillis();

        available.store (current.available, std::memory_order_release);

        if (firstTimeAvailable && onUpdateAvailable)
            notify = onUpdateAvailable; // copy under lock; fire outside
    }

    if (fromNetwork)
        saveCache();

    if (notify)
        juce::MessageManager::callAsync ([notify] { notify(); });
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
    const std::lock_guard<std::mutex> lock (mutex);

    const auto file = cacheFile();
    if (! file.existsAsFile())
        return;

    const auto parsed = juce::JSON::parse (file);
    if (auto* obj = parsed.getDynamicObject())
    {
        current.latestVersion = obj->getProperty ("latestVersion").toString();
        current.downloadUrl   = obj->getProperty ("downloadUrl").toString();
        current.pageUrl       = obj->getProperty ("pageUrl").toString();
        current.notes         = obj->getProperty ("notes").toString();
        skippedVersion        = obj->getProperty ("skippedVersion").toString();
        lastCheckMs           = (juce::int64) (double) obj->getProperty ("lastCheckMs");
    }
}

void UpdateChecker::saveCache()
{
    const std::lock_guard<std::mutex> lock (mutex);

    auto obj = juce::DynamicObject::Ptr (new juce::DynamicObject());
    obj->setProperty ("latestVersion", current.latestVersion);
    obj->setProperty ("downloadUrl",   current.downloadUrl);
    obj->setProperty ("pageUrl",       current.pageUrl);
    obj->setProperty ("notes",         current.notes);
    obj->setProperty ("skippedVersion", skippedVersion);
    obj->setProperty ("lastCheckMs",   (double) lastCheckMs);
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
