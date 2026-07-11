#include "EditTelemetry.h"

#if __has_include("APIKeys.h")
    #include "APIKeys.h"
#else
    namespace APIKeys
    {
        static constexpr const char* TelemetryEndpointUrl  = "";
        static constexpr const char* TelemetrySharedSecret = "";
    }
#endif

namespace cuesampler
{

// Background worker that periodically POSTs the staged events to the endpoint.
class EditTelemetry::UploadThread final : public juce::Thread
{
public:
    explicit UploadThread (EditTelemetry& ownerIn)
        : juce::Thread ("CueTelemetryUpload"), owner (ownerIn) {}

    void run() override
    {
        // Brief settle delay so we're likely online before the first attempt.
        if (wait (5000))
            return;

        while (! threadShouldExit())
        {
            owner.performUpload();
            wait (kUploadIntervalMs); // wakes early when stopThread() notifies
        }
    }

private:
    EditTelemetry& owner;
};

class EditTelemetry::BpmCoalesceTimer final : public juce::Timer
{
public:
    explicit BpmCoalesceTimer (EditTelemetry& ownerIn) : owner (ownerIn) {}
    void timerCallback() override
    {
        stopTimer();
        owner.flushPendingBpmFromTimer();
    }
private:
    EditTelemetry& owner;
};

EditTelemetry::EditTelemetry()
    : endpointUrl (juce::String (APIKeys::TelemetryEndpointUrl).trim()),
      sharedSecret (juce::String (APIKeys::TelemetrySharedSecret).trim())
{
    loadSettings();
    bpmTimer = std::make_unique<BpmCoalesceTimer> (*this);
    // if (isEnabled())
    //     startUploads();
}

EditTelemetry::~EditTelemetry()
{
    // Worker first (may touch files), then timer (message thread), then flush.
    stopUploads();

    if (auto* helper = bpmTimer.release())
    {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
        {
            if (mm->isThisTheMessageThread())
                delete helper;
            else
                juce::MessageManager::callAsync ([helper] { delete helper; });
        }
        else
        {
            // MessageManager is dead, let it leak safely
        }
    }

    flush();
}

void EditTelemetry::setEnabled (bool shouldEnable)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        enabled.store (shouldEnable, std::memory_order_release);
    }
    saveSettings();

    if (shouldEnable)
        startUploads();
    else
        stopUploads();
}

juce::String EditTelemetry::getInstallId() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return installId;
}

void EditTelemetry::setSampleContext (const std::string& chromaprintFingerprint,
                                      double durationSeconds,
                                      double sampleRateIn,
                                      int numChannels)
{
    const std::lock_guard<std::mutex> lock (mutex);
    flushPendingBpmLocked();

    fingerprintId        = hashFingerprint (chromaprintFingerprint);
    sampleDurationSeconds = durationSeconds;
    sampleRate            = sampleRateIn;
    sampleChannels        = numChannels;
    lastBpmSignature      = {};
}

void EditTelemetry::clearSampleContext()
{
    const std::lock_guard<std::mutex> lock (mutex);
    flushPendingBpmLocked();

    fingerprintId        = {};
    sampleDurationSeconds = 0.0;
    sampleRate            = 0.0;
    sampleChannels        = 0;
    lastBpmSignature      = {};
}

void EditTelemetry::recordBpmCorrection (double algorithmBpm,
                                         double userBpm,
                                         float analysisConfidence,
                                         bool likelyDrifting)
{
    if (! isEnabled())
        return;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        hasPendingBpm    = true;
        pendingAlgoBpm   = algorithmBpm;
        pendingUserBpm   = userBpm;
        pendingConfidence = analysisConfidence;
        pendingDrifting  = likelyDrifting;
    }

    // Coalesce rapid edits (slider drags): (re)start the settle timer. Safe
    // because this method is only called from the message thread.
    if (bpmTimer != nullptr)
        bpmTimer->startTimer (kBpmCoalesceMs);
}

void EditTelemetry::recordKeyObservation (const std::string& localKey,
                                          float localConfidence,
                                          const std::string& metadataKey,
                                          const std::string& onlineKey)
{
    if (! isEnabled())
        return;

    auto fields = juce::DynamicObject::Ptr (new juce::DynamicObject());
    fields->setProperty ("localKey",   juce::String (localKey));
    fields->setProperty ("localConf",  (double) localConfidence);

    if (! metadataKey.empty())
        fields->setProperty ("metadataKey", juce::String (metadataKey));
    if (! onlineKey.empty())
        fields->setProperty ("onlineKey", juce::String (onlineKey));

    // Convenience flag: did the local guess match the authoritative source?
    const juce::String truth = ! onlineKey.empty()   ? juce::String (onlineKey)
                             : ! metadataKey.empty()  ? juce::String (metadataKey)
                                                      : juce::String();
    if (truth.isNotEmpty())
        fields->setProperty ("localCorrect", juce::String (localKey) == truth);

    writeEvent ("key_observation", fields);
}

void EditTelemetry::recordKeyCorrection (const std::string& detectedKey,
                                         float detectedConfidence,
                                         const std::string& userKey)
{
    if (! isEnabled())
        return;

    auto fields = juce::DynamicObject::Ptr (new juce::DynamicObject());
    fields->setProperty ("detectedKey",  juce::String (detectedKey));
    fields->setProperty ("detectedConf", (double) detectedConfidence);
    fields->setProperty ("userKey",      juce::String (userKey));
    fields->setProperty ("wasCorrect",   juce::String (detectedKey) == juce::String (userKey));

    writeEvent ("key_correction", fields);
}

void EditTelemetry::flush()
{
    const std::lock_guard<std::mutex> lock (mutex);
    flushPendingBpmLocked();
}

void EditTelemetry::flushPendingBpmFromTimer()
{
    const std::lock_guard<std::mutex> lock (mutex);
    flushPendingBpmLocked();
}

// Must hold `mutex`.
void EditTelemetry::flushPendingBpmLocked()
{
    if (! hasPendingBpm)
        return;

    hasPendingBpm = false;

    const auto trim = pendingUserBpm - pendingAlgoBpm;

    // Dedupe identical settled values for the same sample.
    const auto signature = juce::String (pendingAlgoBpm, 3) + "|" + juce::String (pendingUserBpm, 3);
    if (signature == lastBpmSignature)
        return;
    lastBpmSignature = signature;

    auto fields = juce::DynamicObject::Ptr (new juce::DynamicObject());
    fields->setProperty ("algoBpm",   pendingAlgoBpm);
    fields->setProperty ("userBpm",   pendingUserBpm);
    fields->setProperty ("trim",      trim);
    fields->setProperty ("conf",      (double) pendingConfidence);
    fields->setProperty ("drifting",  pendingDrifting);

    writeEventLocked ("bpm_correction", fields);
}

void EditTelemetry::writeEvent (const juce::String& type, juce::DynamicObject::Ptr fields)
{
    const std::lock_guard<std::mutex> lock (mutex);
    writeEventLocked (type, fields);
}

// Must hold `mutex`.
void EditTelemetry::writeEventLocked (const juce::String& type, juce::DynamicObject::Ptr fields)
{
    auto root = juce::DynamicObject::Ptr (new juce::DynamicObject());
    root->setProperty ("type", type);
    root->setProperty ("ts", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("install", installId);
    root->setProperty ("fp", fingerprintId);
    root->setProperty ("durSec", sampleDurationSeconds);
    root->setProperty ("sr", sampleRate);
    root->setProperty ("ch", sampleChannels);

    if (fields != nullptr)
        for (auto& prop : fields->getProperties())
            root->setProperty (prop.name, prop.value);

    const auto line = juce::JSON::toString (juce::var (root.get()), true) + "\n";
    logFile().appendText (line, false, false, nullptr);
}

juce::File EditTelemetry::baseDir() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("CueSampler");
    dir.createDirectory();
    return dir;
}

juce::File EditTelemetry::logFile() const       { return baseDir().getChildFile ("telemetry_events.jsonl"); }
juce::File EditTelemetry::uploadingFile() const { return baseDir().getChildFile ("telemetry_events.uploading.jsonl"); }
juce::File EditTelemetry::settingsFile() const  { return baseDir().getChildFile ("telemetry_settings.json"); }

void EditTelemetry::startUploads()
{
    // if (endpointUrl.isEmpty() || ! isEnabled())
    //     return;

    // if (uploadThread == nullptr)
    //     uploadThread = std::make_unique<UploadThread> (*this);

    // if (! uploadThread->isThreadRunning())
    //     uploadThread->startThread();
}

void EditTelemetry::stopUploads()
{
    if (uploadThread != nullptr)
        uploadThread->stopThread (4000); // signals exit + notify, then joins
}

// Background thread only. Stages the live events file (atomic rename) so
// concurrent appends are not lost, then POSTs the staged batch without holding
// the mutex during network I/O. On success the staged file is deleted; on
// failure it is left in place and retried on the next tick.
void EditTelemetry::performUpload()
{
    if (endpointUrl.isEmpty())
        return;

    const auto staged = uploadingFile();

    {
        const std::lock_guard<std::mutex> lock (mutex);
        flushPendingBpmLocked(); // make sure any coalesced event is on disk

        if (! staged.existsAsFile()) // no leftover from a previous failed run
        {
            const auto live = logFile();
            if (! live.existsAsFile() || live.getSize() == 0)
                return; // nothing to send

            if (! live.moveFileTo (staged))
                return; // couldn't stage; try again next tick
        }
    }

    const auto payload = staged.loadFileAsString();
    if (payload.trim().isEmpty())
    {
        staged.deleteFile();
        return;
    }

    if (postBatch (payload))
        staged.deleteFile(); // success → drop staged data
    // failure → leave staged file; next tick retries it
}

bool EditTelemetry::postBatch (const juce::String& payload)
{
    // The secret MUST stay in the URL query string: Apps Script reads it from
    // e.parameter.secret, which is only populated from the query (our body is
    // raw text/plain JSON, not parsed into parameters). withParameter() keeps it
    // as a URL parameter; the constructor's addParametersToRequestBody=FALSE then
    // leaves those parameters in the address rather than relocating them into the
    // POST body. Because that flag also defaults the method to GET, we force POST
    // explicitly with withCustomRequestCommand.
    juce::URL postUrl = juce::URL (endpointUrl).withPOSTData (payload);
    if (sharedSecret.isNotEmpty())
        postUrl = postUrl.withParameter ("secret", sharedSecret);

    // Apps Script answers a POST with a 302 to a script.googleusercontent.com
    // "echo" URL that only serves GET. Auto-following re-issues the POST (or
    // carries stale headers) and fails, so we follow it manually: POST without
    // following, capture Location, then do a clean GET.
    juce::WebInputStream post (postUrl, /*addParametersToRequestBody*/ false);
    post.withExtraHeaders ("Content-Type: text/plain\r\n")
        .withCustomRequestCommand ("POST")
        .withConnectionTimeout (8000)
        .withNumRedirectsToFollow (0);

    if (! post.connect (nullptr))
        return false;

    const int  postStatus = post.getStatusCode();
    const auto location   = post.getResponseHeaders().getValue ("Location", {});
    const auto postBody   = post.readEntireStreamAsString(); // also drains the stream

    // Success requires the server to actually acknowledge with {"ok":true} — a
    // 2xx alone is not enough, because Apps Script returns 200 even for a
    // rejection like {"ok":false,"err":"bad secret"}. Treating that as success
    // would silently discard data.
    auto serverAcked = [] (const juce::String& body) { return body.contains ("\"ok\":true"); };

    if (postStatus >= 200 && postStatus < 300 && serverAcked (postBody))
        return true; // answered directly without a redirect

    if (postStatus < 300 || postStatus >= 400 || location.isEmpty())
        return false;

    // Step 2: clean GET on the echo URL.
    juce::WebInputStream get (juce::URL (location), /*addParametersToRequestBody*/ false);
    get.withConnectionTimeout (8000)
       .withNumRedirectsToFollow (5);

    if (! get.connect (nullptr))
        return false;

    const int  getStatus = get.getStatusCode();
    const auto getBody   = get.readEntireStreamAsString(); // read fully before teardown (avoids -999)

    return (getStatus == 0 || (getStatus >= 200 && getStatus < 300)) && serverAcked (getBody);
}

void EditTelemetry::loadSettings()
{
    const std::lock_guard<std::mutex> lock (mutex);

    bool needsSave = false;
    const auto file = settingsFile();
    if (file.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (file);
        if (auto* obj = parsed.getDynamicObject())
        {
            enabled.store ((bool) obj->getProperty ("enabled"), std::memory_order_release);
            installId = obj->getProperty ("installId").toString();
        }
    }

    if (installId.isEmpty())
    {
        installId = juce::Uuid().toDashedString();
        needsSave = true;
    }

    if (needsSave)
    {
        auto obj = juce::DynamicObject::Ptr (new juce::DynamicObject());
        obj->setProperty ("enabled", enabled.load (std::memory_order_acquire));
        obj->setProperty ("installId", installId);
        settingsFile().replaceWithText (juce::JSON::toString (juce::var (obj.get())));
    }
}

void EditTelemetry::saveSettings()
{
    const std::lock_guard<std::mutex> lock (mutex);

    auto obj = juce::DynamicObject::Ptr (new juce::DynamicObject());
    obj->setProperty ("enabled", enabled.load (std::memory_order_acquire));
    obj->setProperty ("installId", installId);
    settingsFile().replaceWithText (juce::JSON::toString (juce::var (obj.get())));
}

juce::String EditTelemetry::hashFingerprint (const std::string& fingerprint)
{
    if (fingerprint.empty())
        return {};

    // 64-bit content hash (juce_core, no crypto module needed). The fingerprint
    // is one-way: the raw Chromaprint string is never stored.
    const auto hash = (juce::uint64) juce::String (fingerprint).hashCode64();
    return juce::String::toHexString ((juce::int64) hash);
}

} // namespace cuesampler
