#include "PluginProcessor.h"
#include "StemCache.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <cstring>
#include <iostream>
#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
int failures = 0;
void check (bool ok, const char* label)
{
    std::cout << (ok ? "PASS " : "FAIL ") << label << std::endl;
    if (! ok) ++failures;
}
bool equal (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples()) return false;
    for (int c = 0; c < a.getNumChannels(); ++c)
        if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), (size_t) a.getNumSamples() * sizeof(float)) != 0) return false;
    return true;
}
void pump()
{
   #if JUCE_MAC
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
   #elif JUCE_WINDOWS
    MSG message;
    while (PeekMessage (&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage (&message);
        DispatchMessage (&message);
    }
    juce::Thread::sleep (10);
   #else
    juce::Thread::sleep (10);
   #endif
}
template <typename Predicate> bool waitFor (Predicate ready)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (! ready() && juce::Time::getMillisecondCounterHiRes() < deadline) pump();
    return ready();
}
juce::TextButton* findButton (juce::Component& parent, const juce::String& text)
{
    if (auto* button = dynamic_cast<juce::TextButton*> (&parent); button != nullptr && button->getButtonText() == text) return button;
    for (auto* child : parent.getChildren())
        if (auto* found = findButton (*child, text)) return found;
    return nullptr;
}
}

// Test-only access to exercise real worker publication and serialization without
// invoking a multi-second neural separation for every regression fixture.
struct CueSamplerStateTests
{
    using P = AudioPluginAudioProcessor;
    static void setup (P& p)
    {
        auto source = std::make_shared<P::LoadedSampleData>();
        source->sampleRate = 8000;
        source->fileName = "restore fixture";
        source->filePath = "/missing/original.wav";
        source->buffer.setSize (2, 64000);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 64000; ++i)
                source->buffer.setSample (c, i, 0.2f * std::sin ((float) i * 0.05f));
        source->buffer.setSample (0, 0, 1.75f);
        source->buffer.setSample (1, 1, -1.5f);
        source->buffer.setSample (1, 2, 1.0e-12f);
        juce::MemoryBlock encoded;
        check (p.serializeSampleToStateData (*source, encoded), "encode exact source audio");
        source->serializedStateData = juce::var (encoded);

        cuesampler::StemCache::Entry entry;
        entry.sampleRate = source->sampleRate;
        entry.drums = source->buffer;
        entry.bass = source->buffer;
        entry.vocals = source->buffer;
        entry.drums.applyGain (0.25f);
        entry.bass.applyGain (0.5f);
        entry.vocals.applyGain (0.125f);
        entry.drums.setSample (0, 0, 1.25f);
        entry.bass.setSample (1, 0, -1.5f);
        check (cuesampler::StemCache::encode (entry, encoded), "encode exact stem audio");
        auto stems = std::make_shared<P::StemSet>();
        stems->source = source;
        stems->drums = std::move (entry.drums);
        stems->bass = std::move (entry.bass);
        stems->vocals = std::move (entry.vocals);
        stems->serializedStemData = juce::var (encoded);
        stems->cacheKey = "abcdef0123456789";

        const std::lock_guard<std::recursive_mutex> lock (p.sampleStateMutex);
        p.resetStemState();
        p.stemSource = source;
        std::atomic_store (&p.loadedSample, source);
        p.muteDrums.store (true);
        p.muteVocals.store (true);
        p.publishStems (stems, p.stemGeneration.load());
        auto analysis = std::make_shared<P::TempoAnalysisData>();
        analysis->estimatedBpm = 120;
        analysis->beatPeriodSeconds = 0.5;
        analysis->analysisEndSeconds = 8;
        std::atomic_store (&p.tempoAnalysis, analysis);
        p.chopBarsCount.store (2);
        auto chops = std::make_shared<P::ChopState>();
        P::ChopDefinition chop;
        chop.id = 7; chop.startSample = 0; chop.endSample = 32000;
        chop.cueOffsetSamples = 123; chop.gainDecibels = -3; chop.pitchSemitones = 2;
        chop.assignedMidiNote = 48; chop.attackMilliseconds = 12; chop.sustainLevel = 0.6f;
        chop.warpMarkers.push_back ({ 24000, 2.8, true, 1.0 });
        chops->chops.push_back (chop);
        chops->selectedChopId = 7; chops->nextChopId = 8;
        p.publishChopState (chops);
        auto stash = std::make_shared<P::ChopState>();
        chop.id = 8; chop.startSample = 32000; chop.endSample = 64000;
        chop.warpMarkers.clear(); chop.assignedMidiNote = 60;
        stash->chops.push_back (chop); stash->selectedChopId = 8; stash->nextChopId = 9;
        std::atomic_store (&p.stashedChopState, stash);
        p.playbackSamplePosition.store (12345);
        p.waveformZoom.store (0.63f);
        p.waveformScroll.store (0.31f);
        p.midiOctaveOffset.store (1);
        p.publishChopState (std::atomic_load (&p.chopState));
    }

    static bool restored (P& p)
    {
        const std::lock_guard<std::recursive_mutex> lock (p.sampleStateMutex);
        return ! p.pendingRestoreState.isValid() && std::atomic_load (&p.loadedSample) != nullptr;
    }

    static juce::AudioBuffer<float> readExport (const juce::File& file, double expectedRate)
    {
        juce::WavAudioFormat wav;
        auto input = file.createInputStream();
        std::unique_ptr<juce::AudioFormatReader> reader (
            input != nullptr ? wav.createReaderFor (input.release(), true) : nullptr);
        check (reader != nullptr, "export produces a readable WAV");
        if (reader == nullptr) return {};
        check (reader->sampleRate == expectedRate && reader->usesFloatingPointData && reader->bitsPerSample == 32,
               "export rate and floating-point format are correct");
        juce::AudioBuffer<float> result ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&result, 0, result.getNumSamples(), 0, true, true);
        file.deleteFile();
        return result;
    }

    static void runExports (const juce::File& scratch)
    {
        for (const double rate : { 8000.0, 22050.0, 48000.0, 96000.0 })
        {
            juce::AudioBuffer<float> impulse (1, 4097);
            // A band-limited pulse has a stable peak when downsampled; a
            // one-frame impulse above the destination Nyquist does not.
            for (int i = 0; i < impulse.getNumSamples(); ++i)
                impulse.setSample (0, i, (float) std::exp (-0.5 * std::pow ((i - 1024) / 8.0, 2.0)));
            const auto modelRate = StemSeparator::resample (impulse, rate, 44100);
            const auto roundTrip = StemSeparator::resample (modelRate, 44100, rate);
            int peak = 0;
            for (int i = 1; i < roundTrip.getNumSamples(); ++i)
                if (std::abs (roundTrip.getSample (0, i)) > std::abs (roundTrip.getSample (0, peak))) peak = i;
            std::cout << "STEM RESAMPLE rate=" << rate << " peak=" << peak << std::endl;
            check (std::abs (peak - 1024) <= 1, "stem sample-rate round trip does not add timing delay");
            juce::AudioBuffer<float> tiny (1, 1);
            tiny.setSample (0, 0, 0.75f);
            const auto tinyOut = StemSeparator::resample (tiny, rate, 44100);
            bool finite = true;
            for (int i = 0; i < tinyOut.getNumSamples(); ++i) finite = finite && std::isfinite (tinyOut.getSample (0, i));
            check (finite && tinyOut.getNumSamples() == (int) std::ceil (44100 / rate)
                   && std::abs (tinyOut.getSample (0, 0) - 0.75f) < 1.0e-6f,
                   "stem resampler safely pads short/fractional input lengths");
        }
        P p;
        p.hostSampleRate.store (44100);
        p.globalGainDecibels.store (0);
        auto source = std::make_shared<P::LoadedSampleData>();
        source->sampleRate = 44100;
        source->fileName = "export regression";
        source->buffer.setSize (2, 3 * 44100);
        source->buffer.clear();
        for (int i = 0; i < source->buffer.getNumSamples(); ++i)
        {
            const double t = (double) (i % 44100) / 44100;
            const float x = t >= 0.1 && t < 0.97 ? 0.3f * std::sin ((float) (juce::MathConstants<double>::twoPi * 440 * t)) : 0;
            source->buffer.setSample (0, i, x);
            source->buffer.setSample (1, i, x * 0.5f);
        }
        std::atomic_store (&p.loadedSample, source);
        auto layout = std::make_shared<P::ChopState>();
        P::ChopDefinition chop;
        chop.id = 7; chop.startSample = 44100; chop.endSample = 88200;
        chop.releaseMilliseconds = 0;
        layout->chops.push_back (chop);
        std::atomic_store (&p.chopState, std::shared_ptr<P::ChopState> (layout));

        const auto inspect = [&] (float pitch, bool half, double rate, bool warp, int start)
        {
            auto state = std::make_shared<P::ChopState> (*layout);
            auto& c = state->chops[0];
            c.startSample = start; c.endSample = start + 44100;
            if (warp) c.warpMarkers = { { start + 22050, 0.7, false, 0 } };
            std::atomic_store (&p.chopState, std::shared_ptr<P::ChopState> (state));
            p.pitchSemitones.store (pitch); p.halfTimeEnabled.store (half); p.hostSampleRate.store (rate);
            const auto audio = readExport (p.renderChopToTempWav (7, false, scratch), rate);
            if (audio.getNumSamples() == 0) return;
            const double stretch = half ? 2.0 : 1.0;
            check (audio.getNumSamples() == (int) std::llround (rate * stretch), "processed export has exact loop length");
            int onset = 0;
            while (onset < audio.getNumSamples() && std::abs (audio.getSample (0, onset)) < 0.03f) ++onset;
            const double onsetSeconds = onset / rate;
            const double expected = 0.1 * stretch * (warp ? 1.4 : 1.0);
            std::cout << "TIMING pitch=" << pitch << " half=" << half << " warp=" << warp
                      << " rate=" << rate << " start=" << start << " onset=" << onsetSeconds << " expected=" << expected << std::endl;
            check (std::abs (onsetSeconds - expected) < 0.035 * stretch, "no processing-delay silence or pre-chop bleed");
            // The tone extends to 97% of the source; confirm the end wasn't lost to pipeline delay.
            const int tailStart = (int) (rate * stretch * 0.88);
            check (audio.getRMSLevel (0, tailStart, (int) (rate * stretch * 0.04)) > 0.08f,
                   "processed export retains late-loop audio");
            int crossings = 0;
            const int from = (int) (rate * stretch * 0.35), count = (int) (rate * stretch * 0.15);
            for (int i = from + 1; i < from + count; ++i)
                if (audio.getSample (0, i - 1) <= 0 && audio.getSample (0, i) > 0) ++crossings;
            const double frequency = crossings / (count / rate);
            check (std::abs (frequency - 440 * std::pow (2.0, pitch / 12)) < 20,
                   "pitch is correct and independent of tempo/warp");
        };
        inspect (12, false, 44100, false, 44100);
        inspect (-12, false, 44100, false, 0);
        inspect (0, false, 48000, false, 44100);
        inspect (0, false, 44100, true, 44100);
        inspect (7, true, 48000, true, 0);
        inspect (-7, true, 44100, false, 44100);

        // Bypass must preserve float overshoots and exact cue/reverse coordinates.
        auto exact = std::make_shared<P::ChopState> (*layout);
        exact->chops[0].cueOffsetSamples = 100;
        exact->chops[0].reversed = true;
        source->buffer.setSample (0, 88199, 1.75f);
        std::atomic_store (&p.chopState, std::shared_ptr<P::ChopState> (exact));
        p.pitchSemitones.store (0); p.halfTimeEnabled.store (false); p.hostSampleRate.store (44100);
        const auto reversed = readExport (p.renderChopToTempWav (7, false, scratch), 44100);
        check (reversed.getNumSamples() == 44000 && reversed.getSample (0, 0) == 1.75f,
               "reverse/cue export preserves unclipped floating peaks at the correct boundary");

        auto stems = std::make_shared<P::StemSet>();
        stems->source = source;
        stems->drums = source->buffer; stems->drums.applyGain (0.25f);
        stems->bass = source->buffer; stems->bass.applyGain (0.5f);
        stems->vocals = source->buffer; stems->vocals.applyGain (0.125f);
        std::atomic_store (&p.stemSet, std::shared_ptr<const P::StemSet> (stems));
        p.appliedStemMask.store (0); p.muteDrums.store (true); p.muteVocals.store (true);
        const auto muted = readExport (p.renderChopToTempWav (7, false, scratch), 44100);
        check (muted.getNumSamples() == 44000 && muted.getSample (0, 0) == 1.75f * 0.625f,
               "export immediately honours stem mutes before playback remix publishes");

        p.muteDrums.store (false); p.muteVocals.store (false);
        std::atomic_store (&p.chopState, std::shared_ptr<P::ChopState> (layout));
        p.pitchSemitones.store (12);
        const auto request = p.captureChopExport (7, false, scratch);
        p.pitchSemitones.store (-12);
        bool done = false;
        juce::File asyncFile;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        p.renderChopExportAsync (request, [&] (juce::File file) { asyncFile = file; done = true; });
        check (juce::Time::getMillisecondCounterHiRes() - start < 100, "export dispatch does not block the UI for rendering");
        check (waitFor ([&] { return done; }), "background export completes on the message thread");
        const auto asyncAudio = readExport (asyncFile, 44100);
        if (asyncAudio.getNumSamples() > 22050)
        {
            int crossings = 0;
            for (int i = 11026; i < 22050; ++i)
                if (asyncAudio.getSample (0, i - 1) <= 0 && asyncAudio.getSample (0, i) > 0) ++crossings;
            check (std::abs (crossings * 4 - 880) < 20, "background export keeps the settings captured at the gesture");
        }
        check (! p.isChopExportCurrent (request), "changing controls invalidates a prepared drag file");
        const auto generation = p.warpRenderGeneration.load();
        const auto ratio = p.timeStretchRatio.load();
        const auto reset = p.voiceResetRequest.load();
        const auto finalFile = p.renderChopToTempWav (7, true, scratch);
        finalFile.deleteFile();
        check (p.warpRenderGeneration.load() == generation && p.timeStretchRatio.load() == ratio
               && p.voiceResetRequest.load() == reset, "export leaves live cache generations and transport untouched");
        // Short attacking chops with a long release used to export as silence.
        auto shortLayout = std::make_shared<P::ChopState> (*layout);
        auto& shortChop = shortLayout->chops[0];
        shortChop.startSample = 50000; shortChop.endSample = 54410;
        shortChop.attackMilliseconds = 10; shortChop.releaseMilliseconds = 4000;
        p.pitchSemitones.store (0);
        std::atomic_store (&p.chopState, shortLayout);
        const auto shortAudio = readExport (p.renderChopToTempWav (7, false, scratch), 44100);
        check (shortAudio.getNumSamples() == 4410 && shortAudio.getRMSLevel (0, 500, 1500) > 0.1f,
               "short chop with attack and long release keeps an audible body");
        check (shortAudio.getNumSamples() == 4410 && std::abs (shortAudio.getSample (0, 4409)) < 0.001f,
               "shortened release still finishes at the loop boundary");

        std::atomic_store (&p.chopState, layout);
        auto analysis = std::make_shared<P::TempoAnalysisData>();
        analysis->estimatedBpm = 120; analysis->beatPeriodSeconds = 0.5;
        std::atomic_store (&p.tempoAnalysis, std::shared_ptr<P::TempoAnalysisData> (analysis));
        p.hostBpm.store (80); p.timeStretchRatio.store (1.0f);
        p.muteBass.store (true); // applied mix remains original, intentionally
        p.pitchSemitones.store (5);
        const auto tempoFile = p.renderChopToTempWav (7, true, scratch);
        const auto tempoAudio = readExport (tempoFile, 44100);
        check (tempoAudio.getNumSamples() == 66150, "host tempo is captured without waiting for an audio callback");
        check (tempoAudio.getNumSamples() == 66150 && tempoAudio.getRMSLevel (0, 22050, 11025) < 0.13f
               && tempoAudio.getRMSLevel (0, 22050, 11025) > 0.07f,
               "stem attenuation survives combined tempo and pitch export");
        check (p.timeStretchRatio.load() == 1.0f, "sync export does not mutate the playback ratio");
        p.muteBass.store (false);
        // A captured prepared buffer must remain valid even if playback caches are cleared.
        const auto prepared = cuesampler::ChopAudioCache::renderPreparedChopSync (
            source->buffer, 44100, 44100, 7, 44100, 88200, 0, {}, 5, 1.0f, 1);
        p.chopAudioCache.storePrepared (prepared);
        const auto cachedRequest = p.captureChopExport (7, false, scratch);
        p.chopAudioCache.clear();
        bool cachedDone = false;
        juce::File cachedFile;
        p.renderChopExportAsync (cachedRequest, [&] (juce::File file) { cachedFile = file; cachedDone = true; });
        check (waitFor ([&] { return cachedDone; }) && cachedFile.existsAsFile(),
               "export retains immutable prepared audio across live cache invalidation");
        cachedFile.deleteFile();

        const auto copySource = p.renderChopToTempWav (7, false, scratch);
        const auto copyDestination = scratch.getChildFile ("saved export.wav");
        copyDestination.replaceWithText ("old contents");
        bool copyDone = false, copyOK = false;
        p.saveChopExportAsync (copySource, copyDestination, [&] (bool ok) { copyOK = ok; copyDone = true; });
        check (waitFor ([&] { return copyDone; }) && copyOK && ! copySource.existsAsFile(),
               "background Save As replaces destination and cleans up temporary audio");
        readExport (copyDestination, 44100);
        const auto failedSource = p.renderChopToTempWav (7, false, scratch);
        copyDone = false; copyOK = true;
        p.saveChopExportAsync (failedSource, scratch.getChildFile ("missing-parent/export.wav"),
                              [&] (bool ok) { copyOK = ok; copyDone = true; });
        check (waitFor ([&] { return copyDone; }) && ! copyOK && failedSource.existsAsFile(),
               "failed background Save As retains the rendered audio for recovery");
        failedSource.deleteFile();

        std::atomic_store (&p.stemSet, std::shared_ptr<const P::StemSet>());
        p.muteDrums.store (true);
        check (p.captureChopExport (7, false, scratch) == nullptr, "missing requested stems cannot silently export the original");
    }

    static void run (bool testSeparation)
    {
        P original;
        setup (original);
        const auto originalStems = std::atomic_load (&original.stemSet);
        const auto originalMix = original.getLoadedSample();
        juce::MemoryBlock saved;
        original.getStateInformation (saved);
        check (saved.getSize() > 0, "save project state");
        cuesampler::StemCache::clear();

        P reopened;
        reopened.stemModelId.clear();
        reopened.stemModelPath.clear();
        // Holding the publication mutex makes the immediate re-save deterministic:
        // the decoder may run, but cannot commit its result until this scope ends.
        {
            const std::lock_guard<std::recursive_mutex> lock (reopened.sampleStateMutex);
            auto oldRender = std::make_shared<cuesampler::ChopAudioCache::Entry>();
            oldRender->chopId = 7;
            reopened.chopAudioCache.store (oldRender);
            reopened.setStateInformation (saved.getData(), (int) saved.getSize());
            check (reopened.chopAudioCache.get (7) == nullptr, "restore invalidates previous warped audio");
            juce::MemoryBlock duringRestore;
            reopened.getStateInformation (duringRestore);
            check (duringRestore == saved, "immediate DAW re-save retains pending source and stems");
        }
        check (waitFor ([&] { return restored (reopened); }), "complete async restore");
        auto restoredStems = std::atomic_load (&reopened.stemSet);
        check (restoredStems != nullptr, "restore stems without cache, original file, or model");
        if (restoredStems == nullptr) return;
        check (equal (originalStems->source->buffer, restoredStems->source->buffer), "source restores bit-exactly including overshoots");
        check (equal (originalStems->drums, restoredStems->drums)
            && equal (originalStems->bass, restoredStems->bass)
            && equal (originalStems->vocals, restoredStems->vocals), "all stems restore bit-exactly");
        check (equal (originalMix->buffer, reopened.getLoadedSample()->buffer), "saved mute mix restores bit-exactly");
        check (reopened.getMuteDrums() && reopened.getMuteVocals() && ! reopened.getMuteBass(), "saved mute buttons restored");
        check (reopened.getChopBarsCount() == 2 && reopened.getMidiOctaveOffset() == 1, "bars and octave restored");
        const auto chops = reopened.getChopState();
        check (chops->selectedChopId == 7 && chops->chops.size() == 1
            && chops->chops[0].cueOffsetSamples == 123 && chops->chops[0].sustainLevel == 0.6f
            && chops->chops[0].warpMarkers.size() == 1
            && chops->chops[0].warpMarkers[0].localTimeSeconds == 2.8, "selection, cue, envelope and warp survive restore");
        check (std::atomic_load (&reopened.stashedChopState)->chops[0].id == 8, "inactive chop layer survives restore");
        check (reopened.getMidiNoteForChopId (7) == 48, "restored MIDI map matches saved pad");
        check (reopened.playbackSamplePosition.load() == 12345, "playhead restored");
        reopened.prepareToPlay (8000, 64);
        juce::AudioBuffer<float> output (2, 64);
        juce::MidiBuffer midi;
        reopened.processBlock (output, midi);
        check (reopened.playbackSamplePosition.load() == 12345, "idle audio callback keeps restored playhead");
        reopened.setMuteDrums (false); reopened.setMuteVocals (false);
        check (waitFor ([&] { return reopened.appliedStemMask.load() == 0; }), "unmute after reopen completes");
        check (equal (originalStems->source->buffer, reopened.getLoadedSample()->buffer), "unmuting restores original without double subtraction");

        // Real BARS rebuild: retain a marker in the later child of a split.
        reopened.setChopBarsCount (1);
        const auto split = reopened.getChopState();
        check (split->chops.size() >= 2 && split->chops[1].warpMarkers.size() == 1
            && std::abs (split->chops[1].warpMarkers[0].localTimeSeconds - 0.8) < 1.0e-9,
            "BARS split rebases retained warp targets");
        if (split->chops.size() >= 2)
        {
            cuesampler::WarpMap map;
            map.build (split->chops[1].startSample, split->chops[1].endSample, split->chops[1].warpMarkers, 8000);
            check (map.numDroppedMarkers() == 0 && ! map.isIdentity(), "split warp remains active");
        }

        // Exercise the real editor synchronization while the window stays open.
        std::unique_ptr<juce::AudioProcessorEditor> editor (reopened.createEditor());
        auto* one = findButton (*editor, "1");
        auto* two = findButton (*editor, "2");
        check (one != nullptr && two != nullptr, "find BARS controls");
        reopened.undoLastEdit();
        check (waitFor ([&] { return two != nullptr && two->getToggleState(); }), "undo refreshes highlighted BARS segment");
        check (one != nullptr && ! one->getToggleState(), "undo clears previous BARS highlight");
        reopened.setChopBarsCount (4);
        auto* four = findButton (*editor, "4");
        check (waitFor ([&] { return four != nullptr && four->getToggleState(); }), "BARS changes while editor stays open");
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (waitFor ([&] { return restored (reopened) && two != nullptr && two->getToggleState(); }), "second restore completes with editor open");
        check (two != nullptr && two->getToggleState(), "host restore keeps BARS display synchronized");
        editor.reset();

        // Older projects use a cache key, but a successful recall upgrades their
        // next save to embedded audio. No model is needed to decode old stems.
        auto legacy = juce::ValueTree::readFromData (static_cast<const char*> (saved.getData()) + 4, saved.getSize() - 4);
        legacy.setProperty ("version", 6, nullptr);
        legacy.removeProperty ("embeddedStemData", nullptr);
        check (cuesampler::StemCache::storeEncoded (originalStems->cacheKey,
                   *originalStems->serializedStemData.getBinaryData()), "seed legacy cache entry");
        juce::MemoryBlock legacyState;
        { juce::MemoryOutputStream stream (legacyState, false); stream.write ("CSB2", 4); legacy.writeToStream (stream); }
        reopened.setStateInformation (legacyState.getData(), (int) legacyState.getSize());
        check (waitFor ([&] { return restored (reopened); }), "legacy cache-only project restores without model");
        check (std::atomic_load (&reopened.stemSet) != nullptr
               && equal (originalMix->buffer, reopened.getLoadedSample()->buffer), "legacy mute mix restored");
        juce::MemoryBlock upgraded;
        reopened.getStateInformation (upgraded);
        cuesampler::StemCache::clear();
        reopened.setStateInformation (upgraded.getData(), (int) upgraded.getSize());
        check (waitFor ([&] { return restored (reopened); }), "upgraded legacy save restores after cache removal");
        check (std::atomic_load (&reopened.stemSet) != nullptr, "upgraded save embeds stems");

        // Save-time editing has no 256-chop/64-marker limit. Recall must not
        // silently trim either list (a normal long song can exceed 256 chops).
        auto largeLayout = std::make_shared<P::ChopState>();
        for (int i = 0; i < 300; ++i)
        {
            P::ChopDefinition chop;
            chop.id = i + 1; chop.startSample = i * 200; chop.endSample = (i + 1) * 200;
            largeLayout->chops.push_back (chop);
        }
        for (int i = 1; i <= 80; ++i)
            largeLayout->chops[0].warpMarkers.push_back ({ i * 2, (double) i * 2 / 8000, false, 0 });
        largeLayout->selectedChopId = 300; largeLayout->nextChopId = 301;
        reopened.publishChopState (largeLayout);
        juce::MemoryBlock largeSaved;
        reopened.getStateInformation (largeSaved);
        reopened.setStateInformation (largeSaved.getData(), (int) largeSaved.getSize());
        check (waitFor ([&] { return restored (reopened); }), "large chop layout restores");
        const auto largeRestored = reopened.getChopState();
        check (largeRestored->chops.size() == 300 && largeRestored->selectedChopId == 300,
               "all 300 chops and the late selection survive recall");
        check (! largeRestored->chops.empty() && largeRestored->chops[0].warpMarkers.size() == 80,
               "all 80 warp markers survive recall");
        reopened.publishChopState (std::make_shared<P::ChopState>());
        juce::MemoryBlock noChops;
        reopened.getStateInformation (noChops);
        reopened.setStateInformation (noChops.getData(), (int) noChops.getSize());
        check (waitFor ([&] { return restored (reopened); }), "empty chop layout restores");
        check (reopened.getChopState()->chops.empty(), "an intentionally empty layout stays empty");

        // Delayed work from before an empty-state recall must be rejected even
        // when no replacement separation/cache lookup is going to run.
        const auto staleGeneration = reopened.stemGeneration.load();
        const auto staleRemix = reopened.stemRemixGeneration.load();
        const auto staleSet = std::atomic_load (&reopened.stemSet);
        juce::ValueTree empty ("CueSamplerState");
        empty.setProperty ("hasLoadedSample", false, nullptr);
        empty.setProperty ("muteDrums", true, nullptr);
        juce::MemoryBlock emptyState;
        { juce::MemoryOutputStream stream (emptyState, false); stream.write ("CSB2", 4); empty.writeToStream (stream); }
        reopened.setStateInformation (emptyState.getData(), (int) emptyState.getSize());
        reopened.publishStems (staleSet, staleGeneration);
        reopened.rebuildActiveMix (staleRemix);
        check (reopened.getLoadedSample() == nullptr && std::atomic_load (&reopened.stemSet) == nullptr,
               "late cache/separation/remix cannot repopulate an empty project");

        // Restore cancellation while a decoder is pending, including immediate save.
        {
            const std::lock_guard<std::recursive_mutex> lock (reopened.sampleStateMutex);
            reopened.setStateInformation (saved.getData(), (int) saved.getSize());
            reopened.setStateInformation (emptyState.getData(), (int) emptyState.getSize());
        }
        for (int i = 0; i < 20; ++i) pump();
        check (reopened.getLoadedSample() == nullptr, "superseded restore cannot publish its sample");

        if (testSeparation)
        {
            original.requestStemSeparation();
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 60000.0;
            while (original.stemSeparationInProgress.load() && juce::Time::getMillisecondCounterHiRes() < deadline) pump();
            const auto actualStems = std::atomic_load (&original.stemSet);
            check (actualStems != nullptr && actualStems->serializedStemData.getBinaryData() != nullptr,
                   "real separation publishes portable stem data");
            if (actualStems != nullptr)
            {
                const auto actualMix = original.getLoadedSample();
                juce::MemoryBlock actualSaved;
                original.getStateInformation (actualSaved);
                cuesampler::StemCache::clear();
                reopened.setStateInformation (actualSaved.getData(), (int) actualSaved.getSize());
                check (waitFor ([&] { return restored (reopened); }), "real separation project reopens without cache/model");
                check (equal (actualMix->buffer, reopened.getLoadedSample()->buffer), "real separated mix restores bit-exactly");
            }
        }
    }
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto base = argc > 1 ? juce::File (juce::String (argv[1])) : juce::File::getCurrentWorkingDirectory();
    const auto scratch = base.getNonexistentChildFile ("state-test-cache", {}, false);
    scratch.createDirectory();
   #if JUCE_WINDOWS
    ::_putenv_s ("CUE_STEM_CACHE_DIR", scratch.getFullPathName().toRawUTF8());
   #else
    ::setenv ("CUE_STEM_CACHE_DIR", scratch.getFullPathName().toRawUTF8(), 1);
   #endif
    CueSamplerStateTests::runExports (scratch);
    CueSamplerStateTests::run (argc > 2 && juce::String (argv[2]) == "--separate");
    scratch.deleteRecursively();
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
