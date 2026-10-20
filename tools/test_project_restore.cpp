#include "PluginProcessor.h"
#include "StemCache.h"
#include "Mp3AudioCodec.h"
#include "MidiNoteNames.h"
#include "FlOctaveShortcut.h"
#include "LaptopMidiInput.h"
#include "LaptopNoteRouter.h"
#include "NativeLaptopKeyboard.h"
#include "FlLinkProtocol.h"
#include "WaveformColourAnalysis.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
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
double correlation (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples()) return 0.0;
    double dot = 0.0, aa = 0.0, bb = 0.0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const auto x = (double) a.getSample (c, i), y = (double) b.getSample (c, i);
            dot += x * y; aa += x * x; bb += y * y;
        }
    return dot / std::sqrt (aa * bb);
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
juce::Component* findType (juce::Component& parent, const juce::String& name)
{
    if (juce::String (typeid (parent).name()).contains (name)) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* found = findType (*child, name)) return found;
    return nullptr;
}
juce::Component* findComponentId (juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* found = findComponentId (*child, id)) return found;
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
        // An existing C4 project fixture: new imports use C2, but recall and
        // edit tests must continue exercising this saved project's old map.
        p.recordedMidiRoot.store (48);
        p.midiBankMode.store (false);
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

    static juce::AudioBuffer<float> readExport (const juce::File& file, double expectedRate, int expectedSamples)
    {
        juce::MemoryBlock encoded;
        check (file.getFileExtension().equalsIgnoreCase (".mp3") && file.loadFileAsData (encoded),
               "export produces an MP3 file");
        juce::AudioBuffer<float> result;
        check (cuesampler::Mp3AudioCodec::decode (encoded, expectedRate, 2, expectedSamples, result),
               "export MP3 decodes at the requested rate and logical length");
        file.deleteFile();
        return result;
    }

    static void runExports (const juce::File& scratch)
    {
        // Spectral rejection, gain, stereo independence and phase alignment are
        // properties of the conversion, independent of neural-model output.
        for (const auto rates : { std::pair<double, double> { 96000, 44100 },
                                  { 48000, 44100 }, { 44100, 8000 }, { 8000, 44100 } })
        {
            const auto [inputRate, outputRate] = rates;
            for (const bool reject : { false, true })
            {
                if (reject && inputRate < outputRate) continue;
                const double frequency = reject ? (outputRate == 8000 ? 10000 : inputRate == 48000 ? 23000 : 30000) : 1000;
                juce::AudioBuffer<float> tone (2, (int) inputRate);
                for (int i = 0; i < tone.getNumSamples(); ++i)
                {
                    tone.setSample (0, i, (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / inputRate));
                    tone.setSample (1, i, 0.25f);
                }
                const auto start = juce::Time::getMillisecondCounterHiRes();
                const auto converted = StemSeparator::resample (tone, inputRate, outputRate);
                double energy = 0, error = 0, dcError = 0;
                const int margin = 256;
                for (int i = margin; i < converted.getNumSamples() - margin; ++i)
                {
                    const auto sample = converted.getSample (0, i);
                    energy += sample * sample;
                    error = std::max (error, std::abs (sample - std::sin (juce::MathConstants<double>::twoPi * frequency * i / outputRate)));
                    dcError = std::max (dcError, std::abs ((double) converted.getSample (1, i) - 0.25));
                }
                const double rms = std::sqrt (energy / (converted.getNumSamples() - 2 * margin));
                std::cout << "SINC " << inputRate << "->" << outputRate << " tone=" << frequency
                          << " rms=" << rms << " elapsed-ms=" << juce::Time::getMillisecondCounterHiRes() - start << std::endl;
                check (reject ? rms < 0.001 : error < 0.001, reject ? "resampling rejects above-Nyquist aliasing" : "resampling preserves passband gain and phase");
                check (dcError < 1.0e-6, "resampling preserves DC and channel independence");
                if (! reject && inputRate == 96000)
                    check (equal (tone, StemSeparator::resample (tone, inputRate, inputRate)), "equal-rate resampling preserves every sample");
            }
        }
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
            const auto expectedFrames = (int) std::llround (rate * (half ? 2.0 : 1.0));
            const auto audio = readExport (p.renderChopToTempWav (7, false, scratch), rate, expectedFrames);
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
        const auto reversed = readExport (p.renderChopToTempWav (7, false, scratch), 44100, 44000);
        check (reversed.getNumSamples() == 44000 && std::abs (reversed.getSample (0, 0)) > 0.5f,
               "reverse/cue MP3 export keeps the peak at the correct boundary");

        auto stems = std::make_shared<P::StemSet>();
        stems->source = source;
        stems->drums = source->buffer; stems->drums.applyGain (0.25f);
        stems->bass = source->buffer; stems->bass.applyGain (0.5f);
        stems->vocals = source->buffer; stems->vocals.applyGain (0.125f);
        std::atomic_store (&p.stemSet, std::shared_ptr<const P::StemSet> (stems));
        p.appliedStemMask.store (0); p.muteDrums.store (true); p.muteVocals.store (true);
        const auto muted = readExport (p.renderChopToTempWav (7, false, scratch), 44100, 44000);
        check (muted.getNumSamples() == 44000 && muted.getRMSLevel (0, 2000, 8000) < reversed.getRMSLevel (0, 2000, 8000) * 0.72f,
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
        const auto asyncAudio = readExport (asyncFile, 44100, 44100);
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
        const auto shortAudio = readExport (p.renderChopToTempWav (7, false, scratch), 44100, 4410);
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
        const auto tempoAudio = readExport (tempoFile, 44100, 66150);
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
        const auto copyDestination = scratch.getChildFile ("saved export.mp3");
        copyDestination.replaceWithText ("old contents");
        bool copyDone = false, copyOK = false;
        p.saveChopExportAsync (copySource, copyDestination, [&] (bool ok) { copyOK = ok; copyDone = true; });
        check (waitFor ([&] { return copyDone; }) && copyOK && ! copySource.existsAsFile(),
               "background Save As replaces destination and cleans up temporary audio");
        readExport (copyDestination, 44100, 44100);
        const auto failedSource = p.renderChopToTempWav (7, false, scratch);
        copyDone = false; copyOK = true;
        p.saveChopExportAsync (failedSource, scratch.getChildFile ("missing-parent/export.mp3"),
                              [&] (bool ok) { copyOK = ok; copyDone = true; });
        check (waitFor ([&] { return copyDone; }) && ! copyOK && failedSource.existsAsFile(),
               "failed background Save As retains the rendered audio for recovery");
        failedSource.deleteFile();

        std::atomic_store (&p.stemSet, std::shared_ptr<const P::StemSet>());
        p.muteDrums.store (true);
        check (p.captureChopExport (7, false, scratch) == nullptr, "missing requested stems cannot silently export the original");
    }

    static void runMousePlaybackModes()
    {
        P p;
        setup (p);
        p.waveformZoom.store (0); p.waveformScroll.store (0);
        auto layout = std::make_shared<P::ChopState>();
        P::ChopDefinition chop;
        chop.id = 1; chop.startSample = 0; chop.endSample = 2048;
        chop.cueOffsetSamples = 128; chop.assignedMidiNote = -2;
        layout->chops.push_back (chop); layout->selectedChopId = 1; layout->nextChopId = 2;
        p.publishChopState (layout);
        p.prepareToPlay (8000, 64);
        juce::AudioBuffer<float> output (2, 64);
        juce::MidiBuffer midi;
        auto render = [&] (int blocks)
        {
            for (int i = 0; i < blocks; ++i) { midi.clear(); p.processBlock (output, midi); }
        };
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* gate = findButton (*editor, "GATE");
        auto* oneShot = findButton (*editor, "ONE SHOT");
        check (gate != nullptr && oneShot != nullptr, "both playback mode choices are always visible");
        if (gate == nullptr || oneShot == nullptr) return;
        check (gate->getToggleState() && ! oneShot->getToggleState(), "Gate is visibly selected by default");
        oneShot->onClick();
        check (! gate->getToggleState() && oneShot->getToggleState()
               && p.getChopPlaybackMode() == P::ChopPlaybackMode::OneShot, "One Shot selector updates processor and active highlight");

        // Exercise actual waveform handlers, including a full click before the
        // audio thread has seen any start/release commands. This chop has no MIDI key.
        std::function<juce::Component* (juce::Component&)> findWave = [&] (juce::Component& c) -> juce::Component*
        {
            if (juce::String (typeid (c).name()).contains ("WaveformDisplayComponent")) return &c;
            for (auto* child : c.getChildren()) if (auto* found = findWave (*child)) return found;
            return nullptr;
        };
        auto* wave = findWave (*editor);
        check (wave != nullptr, "waveform component available for mouse regression");
        if (wave == nullptr) return;
        const juce::Point<float> point (45.0f, 100.0f);
        const juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
            wave, wave, juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(), 1, false);
        wave->mouseDown (event); wave->mouseUp (event); render (1);
        check (p.voice.playbackActive && p.voice.playbackTriggeredByMouse && p.voice.chopOneShot
               && p.getMidiNoteForChopId (1) == -1 && output.getMagnitude (0, 64) > 0,
               "quick waveform click plays an unmapped One Shot after mouse-up");
        check (p.voice.playbackSamplePosition >= 128 && p.voice.playbackSamplePosition < 256,
               "mouse audition starts at the chop cue");
        gate->onClick(); render (40);
        check (! p.voice.playbackActive, "One Shot ends without looping even when mode changes during playback");
        wave->mouseDown (event); render (80);
        check (p.voice.playbackActive && ! p.voice.chopOneShot && p.voice.playbackSamplePosition < 2048,
               "Gate waveform hold loops past the chop boundary");
        oneShot->onClick(); wave->mouseUp (event); render (20);
        check (! p.voice.playbackActive, "mouse-up releases a latched Gate even after switching to One Shot");
        wave->mouseDown (event); render (40);
        check (! p.voice.playbackActive, "holding the mouse does not loop a One Shot");
        wave->mouseUp (event); render (1);
        wave->mouseDown (event); render (1);
        check (p.voice.playbackSamplePosition >= 128 && p.voice.playbackSamplePosition < 256,
               "repeated mouse clicks retrigger from the cue instead of resuming");
        wave->mouseUp (event);
        p.stopPlayback(); render (1);
        check (! p.voice.playbackActive, "Stop cancels a mouse One Shot");

        gate->onClick(); wave->mouseDown (event); render (1);
        p.startPlayback(); render (1); wave->mouseUp (event); render (10);
        check (p.voice.playbackActive && ! p.voice.playbackTriggeredByMouse,
               "mouse-up does not stop a newer transport voice");
        p.stopPlayback(); render (1);
        gate->onClick(); wave->mouseDown (event); render (1);
        editor.reset(); render (20);
        check (! p.voice.playbackActive, "closing the editor releases a held mouse Gate");
    }

    static void runExportHandle()
    {
        P p; setup (p);
        p.waveformZoom.store (0); p.waveformScroll.store (0);
        auto layout = std::make_shared<P::ChopState>();
        P::ChopDefinition chop;
        chop.id = 1; chop.startSample = 0; chop.endSample = 32000;
        layout->chops.push_back (chop); layout->selectedChopId = 1; layout->nextChopId = 2;
        p.publishChopState (layout);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        editor->setVisible (true);
        for (int i = 0; i < 30; ++i) pump();
        std::function<juce::Component* (juce::Component&)> findWave = [&] (juce::Component& c) -> juce::Component*
        {
            if (juce::String (typeid (c).name()).contains ("WaveformDisplayComponent")) return &c;
            for (auto* child : c.getChildren()) if (auto* found = findWave (*child)) return found;
            return nullptr;
        };
        auto* wave = findWave (*editor);
        auto* tooltip = dynamic_cast<juce::TooltipClient*> (wave);
        check (wave != nullptr && tooltip != nullptr, "export waveform provides gesture hints");
        if (wave == nullptr || tooltip == nullptr) return;
        auto eventAt = [&] (juce::Point<float> point, bool pressed)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
                juce::ModifierKeys (pressed ? juce::ModifierKeys::leftButtonModifier : 0),
                1, 0, 0, 0, 0, wave, wave, juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(), 1, false);
        };
        juce::Point<float> dragPoint, adsrPoint;
        bool foundDrag = false, foundAdsr = false;
        for (int x = 30; x < wave->getWidth() - 80; x += 3)
        {
            const juce::Point<float> point ((float) x, 48.0f);
            wave->mouseMove (eventAt (point, false));
            if (tooltip->getTooltip().startsWith ("Drag this chop")) { dragPoint = point; foundDrag = true; }
            if (tooltip->getTooltip().startsWith ("Adjust this chop")) { adsrPoint = point; foundAdsr = true; }
        }
        check (foundDrag && foundAdsr && dragPoint != adsrPoint, "ADSR and DRAG AUDIO have distinct hit targets and hints");
        if (! foundDrag || ! foundAdsr) return;
        auto click = eventAt (dragPoint, true);
        auto gate = std::make_shared<juce::WaitableEvent>();
        p.exportThreadPool.addJob ([gate] { gate->wait (10000); });
        wave->mouseDown (click);
        check (p.exportThreadPool.getNumJobs() == 2, "pressing DRAG AUDIO immediately queues background preparation");
        wave->mouseUp (click);
        wave->mouseDoubleClick (click);
        check (! p.isPlaying() && ! p.getChopState()->chops.front().favorite,
               "export handle neither auditions nor toggles a favorite");
        gate->signal();
        check (waitFor ([&] { return p.exportThreadPool.getNumJobs() == 0; }), "early-release export preparation completes");
        for (int i = 0; i < 10; ++i) pump();
        check (findButton (*editor, "SAVE MP3...") == nullptr, "preparation click does not open the ADSR menu");

        auto reuseGate = std::make_shared<juce::WaitableEvent>();
        p.exportThreadPool.addJob ([reuseGate] { reuseGate->wait (10000); });
        wave->mouseDown (click); wave->mouseUp (click);
        check (p.exportThreadPool.getNumJobs() == 1, "repeat click reuses prepared audio without another render");
        p.setChopEnvelopeParameter (1, P::ChopEnvelopeParameter::Attack, 25);
        for (int i = 0; i < 5; ++i) pump();
        wave->mouseDown (click); wave->mouseUp (click);
        check (p.exportThreadPool.getNumJobs() == 2, "editing the chop invalidates prepared export audio");
        reuseGate->signal();
        check (waitFor ([&] { return p.exportThreadPool.getNumJobs() == 0; }), "updated export finishes after an early release");
        for (int i = 0; i < 10; ++i) pump();
        auto adsrClick = eventAt (adsrPoint, true);
        wave->mouseDown (adsrClick); wave->mouseUp (adsrClick);
        check (findButton (*editor, "SAVE MP3...") != nullptr, "ADSR opens the envelope menu with a separate Save MP3 action");
    }

    // Opt-in CPU paint benchmark; timings are reported, never used as flaky CI assertions.
    static void runUiBenchmark()
    {
        P p;
        auto source = std::make_shared<P::LoadedSampleData>();
        source->sampleRate = 48000;
        source->buffer.setSize (2, 48000 * 60);
        juce::Random random (12345);
        for (int i = 0; i < source->buffer.getNumSamples(); ++i)
        {
            const float envelope = 0.15f + 0.8f * std::abs (std::sin ((float) i * 0.000023f));
            for (int ch = 0; ch < 2; ++ch)
                source->buffer.setSample (ch, i, envelope * (random.nextFloat() * 2.0f - 1.0f));
        }
        std::atomic_store (&p.loadedSample, source);
        auto layout = std::make_shared<P::ChopState>();
        for (int i = 0; i < 32; ++i)
        {
            P::ChopDefinition chop;
            chop.id = i + 1;
            chop.startSample = i * 90000;
            chop.endSample = (i + 1) * 90000;
            layout->chops.push_back (chop);
        }
        layout->nextChopId = 33;
        layout->selectedChopId = 1;
        p.publishChopState (layout);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        std::function<juce::Component* (juce::Component&)> findWave = [&] (juce::Component& c) -> juce::Component*
        {
            if (juce::String (typeid (c).name()).contains ("WaveformDisplayComponent")) return &c;
            for (auto* child : c.getChildren()) if (auto* found = findWave (*child)) return found;
            return nullptr;
        };
        auto* wave = findWave (*editor);
        check (wave != nullptr, "benchmark waveform exists");
        if (wave == nullptr) return;
        for (int scale : { 1, 2 })
            for (int scenario = 0; scenario < 4; ++scenario)
            {
                juce::Image image (juce::Image::ARGB, wave->getWidth() * scale, wave->getHeight() * scale, true);
                std::vector<double> times;
                for (int frame = 0; frame < 100; ++frame)
                {
                    auto edited = std::make_shared<P::ChopState> (*layout);
                    for (auto& chop : edited->chops)
                        if (scenario != 0)
                            chop.warpMarkers.push_back ({ chop.startSample + 45000,
                                (scenario == 2 || (scenario == 3 && chop.id == 1)) ? 0.55 + (frame % 20) * 0.025 : 0.7, false, 0.0 });
                    edited->chops[0].setCueNormalized ((frame % 50) * 0.01);
                    p.publishChopState (edited);
                    const auto start = juce::Time::getMillisecondCounterHiRes();
                    juce::Graphics g (image);
                    g.addTransform (juce::AffineTransform::scale ((float) scale));
                    wave->paintEntireComponent (g, true);
                    if (frame >= 10) times.push_back (juce::Time::getMillisecondCounterHiRes() - start);
                }
                std::sort (times.begin(), times.end());
                double sum = 0.0;
                for (auto t : times) sum += t;
                std::cout << "BENCH scale=" << scale << " scenario=" << scenario
                          << " size=" << image.getWidth() << "x" << image.getHeight()
                          << " mean_ms=" << sum / (double) times.size()
                          << " p95_ms=" << times[times.size() * 95 / 100]
                          << " max_ms=" << times.back() << std::endl;
            }
    }

    static void runWaveformZoom (const juce::File& scratch)
    {
        P p; setup (p);
        p.setWaveformZoom (0.0f);
        p.setWaveformScroll (0.0f);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        // The console harness has no NSApplication event loop for the GL orb.
        if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
        editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
        editor->setVisible (true);
        for (int i = 0; i < 30; ++i) pump();
        check (findButton (*editor, "ZOOM +") == nullptr
               && findButton (*editor, "ZOOM -") == nullptr
               && findButton (*editor, "FIT") == nullptr, "redundant horizontal zoom buttons are removed");
        auto* heightPlus = findButton (*editor, "+");
        check (heightPlus != nullptr, "waveform height control remains available");
        if (heightPlus == nullptr) return;
        auto* wave = heightPlus->getParentComponent();
        juce::ScrollBar* bar = nullptr;
        for (auto* child : wave->getChildren())
            if (auto* candidate = dynamic_cast<juce::ScrollBar*> (child)) bar = candidate;
        check (bar != nullptr, "zoom tests can inspect the waveform viewport");
        if (bar == nullptr) return;
        juce::Slider* zoomKnob = nullptr;
        std::function<void (juce::Component&)> findZoom = [&] (juce::Component& c)
        {
            if (auto* slider = dynamic_cast<juce::Slider*> (&c);
                slider != nullptr && slider->getTooltip().startsWith ("ZOOM:")) zoomKnob = slider;
            for (auto* child : c.getChildren()) findZoom (*child);
        };
        findZoom (*editor);
        check (zoomKnob != nullptr, "zoom knob is available for binding checks");
        if (zoomKnob == nullptr) return;

        const auto event = [wave] (float x, juce::ModifierKeys mods = {})
        {
            const juce::Point<float> point (x, 150.0f);
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
                mods, 1, 0, 0, 0, 0, wave, wave,
                juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(), 0, false);
        };
        const auto centre = event ((float) wave->getWidth() * 0.5f);
        zoomKnob->setValue (0.0, juce::sendNotificationSync);
        juce::MouseWheelDetails wheel {};
        wheel.isSmooth = true;
        wheel.deltaY = 0.01f;
        for (int i = 0; i < 100; ++i) wave->mouseWheelMove (centre, wheel);
        check (std::abs (p.getWaveformZoom() - 0.08f) < 0.00001f,
               "rapid small trackpad deltas accumulate before animation ticks");
        if (zoomKnob != nullptr)
            check (std::abs (zoomKnob->getValue() - p.getWaveformZoom()) < 0.00001,
                   "trackpad zoom synchronizes the ZOOM knob");
        zoomKnob->setValue (0.0, juce::sendNotificationSync);
        zoomKnob->setValue (0.2, juce::sendNotificationSync);
        check (std::abs (p.getWaveformZoom() - 0.2f) < 0.00001f,
               "ZOOM knob updates horizontal zoom");
        zoomKnob->setValue (0.0, juce::sendNotificationSync);
        check (waitFor ([&] { return ! bar->isVisible(); }), "zero zoom shows the entire sample");
        zoomKnob->setValue (0.2, juce::sendNotificationSync);
        check (waitFor ([&] { return std::abs (bar->getCurrentRangeSize() - 1.0 / std::pow (10.0, 1.2)) < 0.0001; }),
               "knob zoom animates to the requested viewport");
        const double beforeSize = bar->getCurrentRangeSize();
        const double beforeCentre = bar->getCurrentRangeStart() + beforeSize * 0.5;
        wave->mouseMagnify (centre, 2.0f);
        check (p.getWaveformZoom() > 0.2f, "native pinch zoom changes the viewport target");
        check (waitFor ([&] { return std::abs (bar->getCurrentRangeSize() * 2.0 - beforeSize) < 0.001; }),
               "pinch scale doubles horizontal magnification");
        check (std::abs (bar->getCurrentRangeStart() + bar->getCurrentRangeSize() * 0.5 - beforeCentre) < 0.001,
               "pinch keeps the sample under the pointer anchored");
        wave->mouseMagnify (centre, 0.5f);
        check (std::abs (p.getWaveformZoom() - 0.2f) < 0.00001f, "reverse pinch returns to the previous zoom");
        const float savedZoom = p.getWaveformZoom();
        wave->mouseMagnify (centre, 0.0f);
        wave->mouseMagnify (event (0.0f), 2.0f);
        check (p.getWaveformZoom() == savedZoom, "invalid or out-of-display pinch events leave zoom unchanged");
        wheel.deltaY = 0.0f;
        wheel.deltaX = -0.01f;
        const float scrollBefore = p.getWaveformScroll();
        for (int i = 0; i < 100; ++i) wave->mouseWheelMove (centre, wheel);
        check (std::abs (p.getWaveformScroll() - scrollBefore - 0.02f) < 0.00001f,
               "rapid horizontal trackpad scrolling retains every delta");
        wheel.deltaY = 0.001f;
        wave->mouseWheelMove (centre, wheel);
        check (p.getWaveformZoom() == savedZoom, "horizontal trackpad pan ignores vertical drift");
        wheel.deltaX = 0.001f;
        wheel.deltaY = 0.01f;
        const float driftScroll = p.getWaveformScroll();
        wave->mouseWheelMove (centre, wheel);
        check (std::abs (p.getWaveformZoom() - savedZoom - 0.0008f) < 0.00001f,
               "vertical trackpad zoom ignores horizontal drift");
        check (p.getWaveformScroll() != driftScroll, "vertical zoom adjusts scroll to preserve the pointer anchor");
        zoomKnob->setValue (savedZoom, juce::sendNotificationSync);
        const auto ctrlEvent = event ((float) wave->getWidth() * 0.5f, juce::ModifierKeys (juce::ModifierKeys::ctrlModifier));
        wheel.deltaX = 0.1f;
        wave->mouseWheelMove (ctrlEvent, wheel);
        check (std::abs (p.getWaveformZoom() - savedZoom - 0.0008f) < 0.00001f,
               "Ctrl-wheel pinch translation zooms even with horizontal drift");
        zoomKnob->setValue (savedZoom, juce::sendNotificationSync);
        juce::MemoryBlock state;
        p.getStateInformation (state);
        P reopened;
        reopened.setStateInformation (state.getData(), (int) state.getSize());
        check (waitFor ([&] { return restored (reopened); })
               && std::abs (reopened.getWaveformZoom() - savedZoom) < 0.00001f
               && std::abs (reopened.getWaveformScroll() - p.getWaveformScroll()) < 0.00001f,
               "gesture and knob viewport changes survive project restore");
        juce::FileOutputStream screenshot (scratch.getParentDirectory().getChildFile ("waveform-zoom.png"));
        screenshot.setPosition (0);
        screenshot.truncate();
        juce::PNGImageFormat().writeImageToStream (wave->createComponentSnapshot (wave->getLocalBounds()), screenshot);
    }

    static void runPerformanceUi (const juce::File& scratch)
    {
        const auto findType = [] (juce::Component& root, const juce::String& name)
        {
            std::function<juce::Component* (juce::Component&)> visit = [&] (juce::Component& c) -> juce::Component*
            {
                if (juce::String (typeid (c).name()).contains (name)) return &c;
                for (auto* child : c.getChildren()) if (auto* found = visit (*child)) return found;
                return nullptr;
            };
            return visit (root);
        };
        {
            P p; setup (p);
            p.publishChopState (std::make_shared<P::ChopState>());
            p.setWaveformZoom (0.15f);
            p.playbackSamplePosition.store (1000);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
            editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
            editor->setVisible (true);
            auto* wave = findType (*editor, "WaveformDisplayComponent");
            auto* bar = wave != nullptr ? dynamic_cast<juce::ScrollBar*> (findType (*wave, "ScrollBar")) : nullptr;
            check (bar != nullptr && bar->isVisible(), "zoomed waveform exposes its horizontal scrollbar");
            if (bar != nullptr && wave != nullptr)
            {
                const auto region = juce::Rectangle<int> (30, 110, wave->getWidth() - 60, 80);
                const auto before = wave->createComponentSnapshot (region);
                const auto sameImage = [&] (const juce::Image& image)
                {
                    for (int y = 0; y < before.getHeight(); ++y)
                        for (int x = 0; x < before.getWidth(); ++x)
                            if (before.getPixelAt (x, y) != image.getPixelAt (x, y)) return false;
                    return true;
                };
                const double requested = (1.0 - bar->getCurrentRangeSize()) * 0.7;
                bar->setCurrentRangeStart (requested, juce::sendNotificationSync);
                check (std::abs (p.getWaveformScroll() - 0.7f) < 0.0001f,
                       "scrollbar updates the bound scroll value immediately");
                check (sameImage (wave->createComponentSnapshot (region)),
                       "scrollbar defers waveform movement to animation instead of jumping per drag event");
                check (waitFor ([&] { return ! sameImage (wave->createComponentSnapshot (region)); }),
                       "scrollbar moves the waveform on animation frames");
                for (int i = 0; i < 30; ++i) pump();
                check (std::abs (bar->getCurrentRangeStart() - requested) < 0.0001
                       && std::abs (p.getWaveformScroll() - 0.7f) < 0.0001f,
                       "animation never feeds its lagging position back into the thumb or scroll parameter");
            }
        }
        P p; setup (p);
        auto layout = std::make_shared<P::ChopState>();
        // Overlap deliberately: only the triggered tile should show a playhead.
        for (int id : { 1, 2 })
        {
            P::ChopDefinition chop;
            chop.id = id; chop.startSample = 0; chop.endSample = 16000;
            chop.favorite = true; chop.favoriteOrder = id;
            layout->chops.push_back (chop);
        }
        layout->nextChopId = 3; layout->selectedChopId = 1;
        p.publishChopState (layout);
        p.setFavoritesViewEnabled (true);
        p.setMidiPerformanceMode (P::MidiPerformanceMode::LegacyFavorites);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        // The console harness has no NSApplication event loop for the GL orb.
        if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
        editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
        editor->setVisible (true);
        auto* view = findType (*editor, "FavoritesViewComponent");
        auto* grid = view != nullptr ? view->getChildComponent (0) : nullptr;
        check (grid != nullptr && grid->isShowing(), "favorites tiles are visible for performance tests");
        if (grid == nullptr) return;
        const auto greenX = [&] (int slot)
        {
            const auto image = grid->createComponentSnapshot (grid->getLocalBounds());
            int sum = 0, count = 0;
            for (int x = slot * grid->getWidth() / 2; x < (slot + 1) * grid->getWidth() / 2; ++x)
            {
                const auto colour = image.getPixelAt (x, grid->getHeight() / 2);
                if (colour.getGreen() > 120 && colour.getRed() < 30 && colour.getBlue() < 110)
                { sum += x; ++count; }
            }
            return count > 0 ? sum / count : -1;
        };
        p.lastTriggeredChopId.store (1); p.playbackActive.store (true);
        p.playbackSamplePosition.store (4000);
        p.selectChopById (2);
        const int quarter = greenX (0);
        check (quarter > 0 && greenX (1) < 0, "playhead follows the playing chop, not selection or overlapping neighbors");
        p.playbackSamplePosition.store (12000);
        check (greenX (0) > quarter + grid->getWidth() / 6, "favorite playhead advances with playback");
        p.playbackSamplePosition.store (4000);
        check (greenX (0) == quarter, "favorite playhead returns to the cue on loop or retrigger");
        auto reverse = std::make_shared<P::ChopState> (*layout);
        reverse->chops[0].reversed = true; reverse->chops[0].cueOffsetSamples = 4000;
        p.publishChopState (reverse); p.playbackSamplePosition.store (8000);
        check (std::abs (greenX (0) - quarter) <= 2, "reverse cue playback tracks the mirrored waveform");
        auto warp = std::make_shared<P::ChopState> (*layout);
        warp->chops[0].warpMarkers.push_back ({ 8000, 0.5, false, 0.0 });
        p.publishChopState (warp); p.playbackSamplePosition.store (4000);
        check (greenX (0) > quarter + grid->getWidth() / 12, "warped playhead maps playback time onto source waveform");
        {
            juce::FileOutputStream out (scratch.getSiblingFile ("favorites-playhead.png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), out);
        }
        p.playbackActive.store (false);
        check (greenX (0) < 0 && greenX (1) < 0, "stopping playback clears favorite playheads");
        p.publishChopState (std::make_shared<P::ChopState> (*layout));
        p.prepareToPlay (8000, 64);
        const auto point = juce::Point<float> ((float) grid->getWidth() * 0.25f, (float) grid->getHeight() * 0.5f);
        const auto now = juce::Time::getCurrentTime();
        const auto click = juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
            grid, grid, now, point, now, 2, false);
        grid->mouseDown (click); grid->mouseDoubleClick (click);
        juce::AudioBuffer<float> audio (2, 64); juce::MidiBuffer midi;
        p.processBlock (audio, midi);
        check (p.getChopState()->favoriteChopIndices.size() == 2
               && p.getMidiNoteForChopId (1) == 48 && p.getMidiNoteForChopId (2) == 49,
               "double-clicking a favorite preserves membership, order and MIDI assignments");
        check (p.isPlaying() && p.getLastTriggeredChopId() == 1,
               "double-clicking a favorite still auditions the pad");
        grid->mouseUp (click);
        for (int i = 0; i < 10; ++i) { midi.clear(); p.processBlock (audio, midi); }
        check (! p.isPlaying(), "favorite double-click releases Gate playback normally");

        auto navigation = std::make_shared<P::ChopState>();
        P::ChopDefinition early, late, plain;
        early.id = 10; early.startSample = 0; early.endSample = 8000;
        early.favorite = true; early.favoriteOrder = 2;
        late.id = 73; late.startSample = 48000; late.endSample = 56000;
        late.favorite = true; late.favoriteOrder = 1; late.setCueNormalized (0.3);
        late.assignedMidiNote = 84;
        plain.id = 100; plain.startSample = 56000; plain.endSample = 64000;
        navigation->chops = { early, late, plain };
        navigation->nextChopId = 101; navigation->selectedChopId = 73;
        p.publishChopState (navigation);
        p.setMidiPerformanceMode (P::MidiPerformanceMode::LegacyFavorites);
        p.editChangeBroadcaster.sendChangeMessage();
        auto* editFavorite = findButton (*editor, "EDIT CHOP");
        check (editFavorite != nullptr && editFavorite->isVisible() && editFavorite->isEnabled(),
               "favorites expose an Edit Chop action for the selected favorite");
        if (editFavorite == nullptr) return;
        {
            juce::FileOutputStream out (scratch.getSiblingFile ("favorites-edit-action.png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), out);
        }
        editFavorite->onClick();
        auto* wave = findType (*editor, "WaveformDisplayComponent");
        auto* bar = wave != nullptr ? dynamic_cast<juce::ScrollBar*> (findType (*wave, "ScrollBar")) : nullptr;
        check (! p.isFavoritesViewEnabled() && ! view->isVisible() && wave != nullptr && wave->isVisible()
               && p.getChopState()->selectedChopId == 73,
               "Edit Chop returns to the original chop by ID, not its favorite slot");
        check (bar != nullptr && bar->getCurrentRangeStart() * 64000 <= 48000
               && (bar->getCurrentRangeStart() + bar->getCurrentRangeSize()) * 64000 >= 56000
               && bar->getCurrentRangeSize() < 0.2,
               "Edit Chop immediately frames both edges of an offscreen favorite");
        check (p.getChopState()->favoriteChopIndices == std::vector<int> { 1, 0 }
               && std::abs (p.getChopState()->chops[1].getCueNormalized() - 0.3) < 0.0001
               && p.getMidiNoteForChopId (73) == 48,
               "Edit Chop preserves favorites, cue values and the independently selected MIDI mapping");
        for (int i = 0; i < 10; ++i) pump();
        check (p.getChopState()->selectedChopId == 73,
               "a previous pad trigger cannot redirect the editing selection");
        p.setSelectedChopGainDecibels (-9.0f);
        check (std::abs (p.getChopState()->chops[1].gainDecibels + 9.0f) < 0.001f
               && std::abs (p.getChopState()->chops[0].gainDecibels) < 0.001f,
               "subsequent edits affect the opened favorite's original chop");
        p.setFavoritesViewEnabled (true);
        check (waitFor ([&] { return view->isVisible(); }), "favorites can be reopened after editing");
        p.selectChopById (100);
        check (waitFor ([&] { return ! editFavorite->isEnabled(); }) && p.getChopState()->selectedChopId == 100,
               "Favorites preserves an outside selection and disables Edit Chop for that nonfavorite");
        p.selectChopById (10);
        check (waitFor ([&] { return editFavorite->isEnabled(); }), "Edit Chop follows selection changes");
        p.publishChopState (std::make_shared<P::ChopState>());
        p.editChangeBroadcaster.sendChangeMessage();
        check (waitFor ([&] { return ! editFavorite->isEnabled(); }), "Edit Chop is disabled for empty favorites");
        editFavorite->onClick();
        check (p.isFavoritesViewEnabled(), "a stale Edit Chop action safely ignores a removed selection");
    }

    static void runFavoritesAudio()
    {
        P p; setup (p);
        auto layout = std::make_shared<P::ChopState>();
        for (int i = 0; i < 3; ++i)
        {
            P::ChopDefinition chop;
            chop.id = i + 1; chop.startSample = i * 16000; chop.endSample = (i + 1) * 16000;
            chop.favorite = true; chop.favoriteOrder = 3 - i;
            layout->chops.push_back (chop);
        }
        layout->selectedChopId = 1; layout->nextChopId = 4;
        p.publishChopState (layout);
        p.prepareToPlay (8000, 64);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        // The console harness has no NSApplication event loop for the GL orb.
        if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
        editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
        editor->setVisible (true);
        auto* button = findButton (*editor, "FAVORITES");
        auto* view = findType (*editor, "FavoritesViewComponent");
        auto* grid = view != nullptr ? view->getChildComponent (0) : nullptr;
        check (button != nullptr && grid != nullptr, "Favorites audio controls exist");
        if (button == nullptr || grid == nullptr) return;
        juce::AudioBuffer<float> audio (2, 64); juce::MidiBuffer midi;
        const auto render = [&]
        {
            p.processBlock (audio, midi); midi.clear();
            return audio.getMagnitude (0, audio.getNumSamples());
        };
        p.startPlayback();
        check (render() > 0.001f, "transport produces audio before Favorites");
        button->onClick();
        check (render() > 0.001f && p.isPlaying(), "transport continues when Favorites opens");
        button->onClick();
        check (render() > 0.001f && p.isPlaying(), "transport continues when Favorites closes");
        // Opening the view must also preserve a not-yet-consumed Play request.
        p.startPlayback(); button->onClick();
        check (render() > 0.001f && p.isPlaying(), "Favorites does not overwrite a pending Play command");
        for (auto mode : { P::ChopPlaybackMode::Gate, P::ChopPlaybackMode::OneShot })
        {
            p.setChopPlaybackMode (mode);
            p.setMidiPerformanceMode (P::MidiPerformanceMode::Legacy);
            button->onClick();
            const auto currentState = p.getChopState();
            const auto& hostMap = currentState->midiMap.noteToChopIndex;
            const int heldNote = (int) std::distance (hostMap.begin(), std::find (hostMap.begin(), hostMap.end(), 0));
            midi.addEvent (juce::MidiMessage::noteOn (1, heldNote, (juce::uint8) 100), 0);
            render();
            button->onClick();
            p.setMidiPerformanceMode (P::MidiPerformanceMode::LegacyFavorites);
            check (render() > 0.001f && p.voice.activeChopId == 1,
                   "explicit Favorites MIDI activation preserves the already sounding chop");
            midi.addEvent (juce::MidiMessage::noteOff (1, heldNote), 0);
            for (int block = 0; block < 20; ++block) render();
            check (mode == P::ChopPlaybackMode::OneShot ? p.isPlaying() : ! p.isPlaying(),
                   "original MIDI note-off still respects Gate and One Shot after view switching");
            for (int slot = 0; slot < 3; ++slot)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, 48 + slot, (juce::uint8) 100), 0);
                bool audible = render() > 0.001f;
                // Exercise selection refresh and change listeners while sounding.
                for (int block = 0; block < 12; ++block) { pump(); audible = render() > 0.001f && audible; }
                check (audible && p.getLastTriggeredChopId() == 3 - slot,
                       "Favorites MIDI stays audible after UI selection refresh");
                midi.addEvent (juce::MidiMessage::noteOff (1, 48 + slot), 0);
                render();
                if (mode == P::ChopPlaybackMode::OneShot)
                    check (render() > 0.001f, "Favorites One Shot continues after MIDI note-off");
            }
            for (int slot = 0; slot < 3; ++slot)
            {
                const auto point = juce::Point<float> ((slot + 0.5f) * grid->getWidth() / 3.0f,
                                                       grid->getHeight() * 0.5f);
                const auto now = juce::Time::getCurrentTime();
                const auto click = juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
                    juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
                    grid, grid, now, point, now, 1, false);
                grid->mouseDown (click);
                bool audible = render() > 0.001f;
                for (int block = 0; block < 12; ++block) { pump(); audible = render() > 0.001f && audible; }
                check (audible && p.getLastTriggeredChopId() == 3 - slot
                       && p.voice.playbackTriggeredByMouse,
                       "Favorites mouse tiles stay audible after UI refresh");
                grid->mouseUp (click);
                for (int block = 0; block < 20; ++block) render();
                check (mode == P::ChopPlaybackMode::OneShot ? p.isPlaying() : ! p.isPlaying(),
                       "Favorites mouse release respects Gate and One Shot");
            }
        }
        const auto point = juce::Point<float> (grid->getWidth() / 6.0f, grid->getHeight() * 0.5f);
        const auto now = juce::Time::getCurrentTime();
        const auto click = juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
            grid, grid, now, point, now, 1, false);
        p.setChopPlaybackMode (P::ChopPlaybackMode::Gate);
        grid->mouseDown (click); render();
        view->setVisible (false);
        for (int block = 0; block < 20; ++block) render();
        check (! p.isPlaying() && ! p.mousePreviewHeld.load(), "hiding Favorites releases a mouse Gate");
        view->setVisible (true);
        p.setChopPlaybackMode (P::ChopPlaybackMode::OneShot);
        grid->mouseDown (click); grid->mouseUp (click);
        check (render() > 0.001f && p.isPlaying(), "a complete Favorites click between audio blocks starts a One Shot");
    }

    static void runSampleImport (const juce::File& scratch)
    {
        P p;
        p.prepareToPlay (48000, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        // Keep GL out of the console harness; import still runs real UI listeners.
        if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
        for (const auto rate : { 44100, 48000 })
            for (const auto channels : { 1, 2 })
            {
                const auto file = scratch.getChildFile ("import-" + juce::String (rate)
                                                     + "-" + juce::String (channels) + ".wav");
                juce::AudioBuffer<float> signal (channels, rate);
                for (int channel = 0; channel < channels; ++channel)
                    for (int i = 0; i < rate; ++i)
                        signal.setSample (channel, i, 0.2f * std::sin ((float) i * 0.04f));
                auto output = file.createOutputStream();
                auto writer = juce::WavAudioFormat().createWriterFor (output.get(), rate,
                    (unsigned int) channels, 24, {}, 0);
                check (writer != nullptr, "create real WAV import fixture");
                if (writer == nullptr) return;
                output.release();
                check (writer->writeFromAudioSampleBuffer (signal, 0, rate), "write WAV import fixture");
                delete writer;
                // A replacement sample starts a fresh C4 Window, even after
                // opening a project that retained its historical C4 mapping.
                p.recordedMidiRoot.store (48); p.setMidiBankModeEnabled (false); p.setMidiOctaveOffset (7);
                std::cout << "IMPORT " << rate << " Hz / " << channels << " channels" << std::endl;
                check (p.loadAudioFile (file) == P::SampleLoadResult::loaded, "sample imports successfully");
                for (int i = 0; i < 10; ++i) pump();
                check (p.getLoadedSample() != nullptr && p.getLoadedSample()->buffer.getNumSamples() == rate,
                       "import publishes the complete source for UI and playback");
                check (p.getMidiRootNote() == 24 && p.getKeyboardStartNote() == 48
                       && p.getMidiPerformanceMode() == P::MidiPerformanceMode::Window
                       && p.getChopState()->midiMap.rootNote == 48 && p.getKeyboardMidiBank() == 1,
                       "new import starts a C4 Window independently of retained legacy root settings");
                juce::AudioBuffer<float> audio (2, 128);
                juce::MidiBuffer midi;
                p.startPlayback(); p.processBlock (audio, midi);
                check (audio.getMagnitude (0,128) > 0.001f, "imported sample produces audio");
            }

        // Hold the worker to prove UI dispatch returns without decoding/encoding,
        // and that a newer selection discards an older queued import.
        juce::WaitableEvent workerEntered, releaseWorker;
        p.sampleImportThreadPool.addJob ([&]
        {
            workerEntered.signal();
            releaseWorker.wait (5000);
        });
        check (workerEntered.wait (5000), "import worker is held for dispatch regression");
        p.recordedMidiRoot.store (48); p.setMidiBankModeEnabled (false); p.setMidiOctaveOffset (7);
        const auto previous = p.getLoadedSample();
        bool supersededCompleted = false, completed = false;
        p.loadAudioFileAsync (scratch.getChildFile ("import-44100-1.wav"),
                             [&] (auto) { supersededCompleted = true; });
        p.loadAudioFileAsync (scratch.getChildFile ("import-48000-2.wav"),
                             [&] (auto result) { completed = result == P::SampleLoadResult::loaded; });
        check (p.getSampleImportStatus() != nullptr
               && p.getSampleImportStatus()->fileName == "import-48000-2.wav",
               "loading state immediately identifies the latest selected file");
        check (p.getLoadedSample() == previous && ! completed,
               "async import returns while worker is busy and preserves current playback sample");
        check (p.getMidiRootNote() == 48 && ! p.isMidiBankModeEnabled() && p.getKeyboardStartNote() == 84,
               "pending replacement leaves existing project MIDI assignments intact");
        auto* loading = findType (*editor, "SampleImportOverlayComponent");
        auto* chooseAnother = findButton (*editor, "CHOOSE ANOTHER SAMPLE");
        check (waitFor ([&] { return loading != nullptr && loading->isVisible(); })
               && chooseAnother != nullptr && chooseAnother->isEnabled(),
               "loading overlay offers an enabled replacement control");
        if (loading != nullptr)
        {
            juce::FileOutputStream screenshot (scratch.getParentDirectory().getChildFile ("sample-import-loading.png"));
            screenshot.setPosition (0); screenshot.truncate();
            juce::PNGImageFormat().writeImageToStream (loading->createComponentSnapshot (loading->getLocalBounds()), screenshot);
        }
        releaseWorker.signal();
        check (waitFor ([&] { return completed; }), "async import completes on the message thread");
        check (! supersededCompleted && p.getLoadedSample()->sourceFile.getFileName() == "import-48000-2.wav",
               "latest import wins over a superseded request");
        check (p.getMidiRootNote() == 24 && p.getKeyboardStartNote() == 48
               && p.getMidiPerformanceMode() == P::MidiPerformanceMode::Window
               && p.getChopState()->midiMap.rootNote == 48 && p.getKeyboardMidiBank() == 1,
               "successful async replacement starts a C4 Window");
        check (p.getSampleImportStatus() == nullptr, "successful import clears its loading state");

        // Drop a replacement onto the actual loading panel, including an
        // unsupported first entry, to exercise the UI path while already busy.
        workerEntered.reset(); releaseWorker.reset();
        p.sampleImportThreadPool.addJob ([&]
        {
            workerEntered.signal();
            releaseWorker.wait (5000);
        });
        check (workerEntered.wait (5000), "hold worker for loading-panel replacement");
        p.loadAudioFileAsync (scratch.getChildFile ("import-48000-2.wav"), {});
        check (waitFor ([&] { return loading != nullptr && loading->isVisible(); }),
               "loading panel reappears for a subsequent replacement");
        auto* dropTarget = dynamic_cast<juce::FileDragAndDropTarget*> (loading);
        check (dropTarget != nullptr, "loading panel accepts replacement file drops");
        if (dropTarget != nullptr)
            dropTarget->filesDropped ({ scratch.getChildFile ("ignore.txt").getFullPathName(),
                                       scratch.getChildFile ("import-44100-1.wav").getFullPathName() }, 20, 20);
        check (p.getSampleImportStatus()->fileName == "import-44100-1.wav",
               "drop replaces the pending filename immediately");
        releaseWorker.signal();
        check (waitFor ([&] { return p.getSampleImportStatus() == nullptr; })
               && p.getLoadedSample()->sourceFile.getFileName() == "import-44100-1.wav",
               "different sample replaces the old audio after preparation");
        check (waitFor ([&] { return loading != nullptr && ! loading->isVisible(); }),
               "loading panel disappears when replacement is ready");
        const auto imported = p.getLoadedSample();
        check (imported->getSerializedStateData().getBinaryData() != nullptr,
               "async publication includes embedded project audio immediately");
        p.recordedMidiRoot.store (48); p.setMidiBankModeEnabled (false); p.setMidiOctaveOffset (7);
        bool failed = false;
        p.loadAudioFileAsync (scratch.getChildFile ("missing-import.wav"),
                             [&] (auto result) { failed = result == P::SampleLoadResult::failed; });
        check (waitFor ([&] { return failed; }) && p.getLoadedSample() == imported,
               "failed async import preserves the current sample");
        check (p.getMidiRootNote() == 48 && ! p.isMidiBankModeEnabled() && p.getKeyboardStartNote() == 84,
               "failed replacement preserves the current project's MIDI root, channel aliases and viewport");
        check (p.getSampleImportStatus() == nullptr, "failed import clears the loading state");

        juce::MemoryBlock savedState;
        p.getStateInformation (savedState);
        workerEntered.reset(); releaseWorker.reset();
        p.sampleImportThreadPool.addJob ([&]
        {
            workerEntered.signal();
            releaseWorker.wait (5000);
        });
        check (workerEntered.wait (5000), "hold import worker before project restoration");
        bool staleCompleted = false;
        p.loadAudioFileAsync (scratch.getChildFile ("import-44100-1.wav"),
                             [&] (auto) { staleCompleted = true; });
        p.setStateInformation (savedState.getData(), (int) savedState.getSize());
        releaseWorker.signal();
        check (waitFor ([&] { return restored (p)
                                   && p.sampleImportThreadPool.getNumJobs() == 0; }),
               "restoration completes with a queued import canceled");
        check (! staleCompleted && p.getLoadedSample()->buffer.getNumSamples() == imported->buffer.getNumSamples()
               && p.getSampleImportStatus() == nullptr,
               "pending import cannot replace restored project audio");
        check (p.getMidiRootNote() == 48 && ! p.isMidiBankModeEnabled() && p.getKeyboardStartNote() == 84,
               "canceled stale import cannot overwrite the restored project's MIDI assignments");

        int progressCalls = 0;
        juce::MemoryBlock canceledEncoding;
        check (! cuesampler::Mp3AudioCodec::encode (imported->buffer, imported->sampleRate,
                    canceledEncoding, nullptr, 320, [&] (float) { return ++progressCalls < 3; })
               && progressCalls == 3,
               "abandoned import encoding cancels between audio blocks");

        // Keep compaction blocked: import and an immediate self-contained save
        // must succeed independently of MP3 encoding and the original disk file.
        juce::WaitableEvent encoderEntered, releaseEncoder;
        p.sampleStateEncodingThreadPool.addJob ([&]
        {
            encoderEntered.signal();
            releaseEncoder.wait (5000);
        });
        check (encoderEntered.wait (5000), "hold optional persistence compression");
        const auto removable = scratch.getChildFile ("delete-after-import.wav");
        check (scratch.getChildFile ("import-48000-2.wav").copyFileTo (removable), "create removable import fixture");
        p.loadAudioFileAsync (removable, {});
        check (waitFor ([&] { return p.getSampleImportStatus() == nullptr; }),
               "sample is ready while persistence compression is blocked");
        check (p.getMidiRootNote() == 24 && p.getKeyboardStartNote() == 48
               && p.getMidiPerformanceMode() == P::MidiPerformanceMode::Window
               && p.getChopState()->midiMap.rootNote == 48 && p.getKeyboardMidiBank() == 1,
               "replacement after legacy project recall starts a fresh C4 Window");
        const auto fastSample = std::atomic_load (&p.loadedSample);
        const auto snapshot = fastSample->getSerializedStateData();
        check (snapshot.getBinaryData() != nullptr
               && std::memcmp (snapshot.getBinaryData()->getData(), "RIFF", 4) == 0,
               "uncompressed source snapshot is embedded before compaction");
        juce::MemoryBlock immediateSave;
        p.getStateInformation (immediateSave);
        check (removable.deleteFile(), "remove original file before immediate-save recall");
        P immediateRecall;
        immediateRecall.setStateInformation (immediateSave.getData(), (int) immediateSave.getSize());
        check (waitFor ([&] { return restored (immediateRecall); })
               && equal (fastSample->buffer, immediateRecall.getLoadedSample()->buffer),
               "immediate save recalls exact audio without the source file or completed compression");

        P::StemSet stems;
        stems.source = fastSample;
        stems.drums.setSize (fastSample->buffer.getNumChannels(), fastSample->buffer.getNumSamples());
        stems.drums.clear();
        const auto mixed = P::createStemMix (stems, 1);
        check (mixed != nullptr && mixed->deferredStateData == fastSample->deferredStateData,
               "stem remixes share the source persistence snapshot");
        releaseEncoder.signal();
        check (waitFor ([&]
        {
            const auto data = fastSample->getSerializedStateData();
            return data.getBinaryData() != nullptr && std::memcmp (data.getBinaryData()->getData(), "CUMP", 4) == 0;
        }), "source snapshot compacts to MP3 after playback is available");
        check (p.getLoadedSample() == fastSample
               && mixed->getSerializedStateData() == fastSample->getSerializedStateData(),
               "compaction preserves playback identity and reaches existing stem remixes");
        juce::MemoryBlock compactSave;
        p.getStateInformation (compactSave);
        P compactRecall;
        compactRecall.setStateInformation (compactSave.getData(), (int) compactSave.getSize());
        check (waitFor ([&] { return restored (compactRecall); })
               && compactRecall.getLoadedSample()->buffer.getNumSamples() == fastSample->buffer.getNumSamples(),
               "compacted project still recalls with the same sample clock");
    }

    static void runImportBenchmark (const juce::File& scratch)
    {
        constexpr int rate = 48000, samples = rate * 30;
        juce::AudioBuffer<float> signal (2, samples);
        for (int i = 0; i < samples; ++i)
        {
            signal.setSample (0, i, 0.2f * std::sin ((float) i * 0.04f));
            signal.setSample (1, i, 0.2f * std::sin ((float) i * 0.06f));
        }
        const auto file = scratch.getChildFile ("import-benchmark.wav");
        auto output = file.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> writer (juce::WavAudioFormat().createWriterFor (
            output.get(), rate, 2, 24, {}, 0));
        if (writer == nullptr) { check (false, "create import benchmark fixture"); return; }
        output.release();
        check (writer->writeFromAudioSampleBuffer (signal, 0, samples), "write 30-second stereo benchmark");
        writer.reset();
        P p;
        std::array<double, 3> before {}, after {};
        for (size_t i = 0; i < before.size(); ++i)
        {
            auto start = juce::Time::getMillisecondCounterHiRes();
            const auto legacy = p.createLoadedSampleDataFromFile (file);
            before[i] = juce::Time::getMillisecondCounterHiRes() - start;
            start = juce::Time::getMillisecondCounterHiRes();
            const auto fast = p.createLoadedSampleDataFromFile (file, nullptr, {}, true);
            after[i] = juce::Time::getMillisecondCounterHiRes() - start;
            check (legacy != nullptr && fast != nullptr && equal (legacy->buffer, fast->buffer),
                   "fast and previous import paths decode identical audio");
        }
        std::sort (before.begin(), before.end()); std::sort (after.begin(), after.end());
        std::cout << "IMPORT BENCHMARK 30s / 48kHz / stereo WAV: median before=" << before[1]
                  << " ms, ready before compression=" << after[1] << " ms, speedup="
                  << before[1] / after[1] << "x (decode/persistence preparation only)" << std::endl;
    }

    static void runMidiPerformance (const juce::File& scratch)
    {
        using Mode = P::MidiPerformanceMode;
        const auto idFor = [] (int ordinal) { return 10000 - ordinal * 7; };
        const auto makeSong = [&] (int count)
        {
            auto state = std::make_shared<P::ChopState>();
            for (int ordinal = count - 1; ordinal >= 0; --ordinal)
            {
                P::ChopDefinition chop;
                chop.id = idFor (ordinal); chop.startSample = ordinal * 256; chop.endSample = (ordinal + 1) * 256;
                chop.assignedMidiNote = ordinal == 0 ? 84 : -2;
                state->chops.push_back (chop);
            }
            state->selectedChopId = count > 0 ? idFor (0) : -1; state->nextChopId = 10001;
            return state;
        };
        const auto idAt = [] (const P& processor, int note, int channel = 1)
        {
            const auto state = processor.getChopState();
            const int index = state->midiMap.channelNoteToChopIndex[(size_t) (channel - 1)][(size_t) note];
            return index >= 0 && (size_t) index < state->chops.size() ? state->chops[(size_t) index].id : -1;
        };
        P p; setup (p);
        const auto source = p.getLoadedSample();
        for (const int count : { 0, 1, 55, 56, 57, 220 })
        {
            p.publishChopState (makeSong (count)); p.setMidiPerformanceMode (Mode::Window);
            p.setMidiWindowStart (99999);
            const int start = juce::jmax (0, count - 56);
            bool exact = p.getMidiWindowStart() == start;
            for (int slot = 0; slot < 56; ++slot)
                exact &= idAt (p, 48 + slot) == (start + slot < count ? idFor (start + slot) : -1);
            check (exact, "C4-G8 Window clamps and maps chronological sparse IDs at every capacity boundary");
            check (p.getMappedMidiNotes().count() == (size_t) juce::jmin (56, count),
                   "Window never spills beyond the 56 laptop notes");
        }

        p.publishChopState (makeSong (220)); p.setMidiPerformanceMode (Mode::Window);
        auto edited = std::make_shared<P::ChopState> (*p.getChopState());
        auto moved = std::find_if (edited->chops.begin(), edited->chops.end(), [&] (const auto& c) { return c.id == idFor (0); });
        moved->startSample = 60000; moved->endSample = 61000;
        P::ChopDefinition inserted; inserted.id = 20000; inserted.startSample = 1; inserted.endSample = 128;
        edited->chops.insert (edited->chops.begin() + 3, inserted); p.publishChopState (edited);
        bool stable = true;
        for (int slot = 0; slot < 56; ++slot) stable &= idAt (p, 48 + slot) == idFor (slot);
        check (stable, "inserting or moving chop boundaries cannot give committed Window notes to other chops");
        p.setMidiWindowStart (1);
        check (idAt (p, 48) == idFor (1), "an explicit Window commit selects a fresh chronological range");

        p.publishChopState (makeSong (220)); p.setMidiPerformanceMode (Mode::Window); p.setMidiWindowStart (99999);
        check (p.getMidiWindowStart() == 164 && idAt (p, 48) == idFor (164) && idAt (p, 103) == idFor (219),
               "final Window reaches the song's last chop without a partial hidden range");
        p.setMidiWindowStart (10); p.undoLastEdit();
        check (p.getMidiWindowStart() == 164 && idAt (p, 48) == idFor (164), "one Window commit has one undoable stable map");

        const auto favorite = [&] (int ordinal) { p.selectChopById (idFor (ordinal)); p.toggleSelectedChopFavorite(); };
        favorite (0); favorite (199); p.setMidiPerformanceMode (Mode::Favorites);
        check (idAt (p, 48) == idFor (0) && idAt (p, 49) == idFor (199),
               "distant Favorites receive C4 and C-sharp4 in addition order");
        p.setFavoritesViewEnabled (true); p.setFavoritesViewEnabled (false); p.selectChopById (idFor (100));
        check (p.getMidiPerformanceMode() == Mode::Favorites && idAt (p, 48) == idFor (0) && idAt (p, 49) == idFor (199),
               "browsing either view and selecting an outside pad preserves the active Favorites MIDI set");
        favorite (0);
        check (idAt (p, 48) == -1 && idAt (p, 49) == idFor (199), "unfavoriting reserves its empty MIDI slot and preserves surviving notes");
        favorite (36);
        check (idAt (p, 48) == -1 && idAt (p, 50) == idFor (36), "new Favorites append after reservations instead of filling gaps");
        const auto reservedOwners = p.getChopState()->favoriteMidiOwnerIds;
        p.setMidiMappingLocked (true);
        check (! p.resetFavoriteMidiLayout() && p.getChopState()->favoriteMidiOwnerIds == reservedOwners,
               "MIDI Lock rejects an explicit Favorites layout reset");
        p.setMidiMappingLocked (false);
        check (p.resetFavoriteMidiLayout() && idAt (p, 48) == idFor (199) && idAt (p, 49) == idFor (36)
               && p.getChopState()->favoriteMidiHighWater == 2,
               "explicit Favorites reset reclaims gaps in addition order");
        p.undoLastEdit();
        check (p.getChopState()->favoriteMidiOwnerIds == reservedOwners && idAt (p, 48) == -1
               && idAt (p, 49) == idFor (199) && idAt (p, 50) == idFor (36),
               "one Undo restores the exact pre-reset reservation and MIDI notes");
        favorite (0);
        check (idAt (p, 48) == idFor (0) && idAt (p, 49) == idFor (199) && idAt (p, 50) == idFor (36),
               "re-adding a favorite restores its reserved note");
        p.selectChopById (idFor (199)); p.removeSelectedChop();
        check (idAt (p, 49) == -1 && p.getChopState()->favoriteMidiOwnerIds[1] == idFor (199),
               "deleting a favorite preserves its unresolved reservation owner");
        p.undoLastEdit();
        check (idAt (p, 49) == idFor (199), "Undo restores a deleted owner to its exact reserved note");
        for (int ordinal = 1; ordinal <= 60; ++ordinal) if (ordinal != 36) favorite (ordinal);
        bool singleChannel = true;
        for (const int channel : p.getChopState()->midiMap.channelForChopIndex) singleChannel &= channel <= 1;
        check (p.getMappedMidiNotes().count() == 56 && p.getMidiNoteForChopId (idFor (60)) == -1 && singleChannel,
               "excess Favorites stay mouse-playable without automatic channel overflow or slot reuse");

        p.setMidiMappingLocked (true);
        const int savedBars = p.getChopBarsCount();
        const auto lockedIds = p.getChopState()->favoriteMidiOwnerIds;
        const bool modeRejected = ! p.setMidiPerformanceMode (Mode::Window);
        const bool windowRejected = ! p.setMidiWindowStart (0);
        p.setManualChopModeActive (true); p.setChopBarsCount (savedBars + 1); p.clearAllChops();
        p.selectChopById (idFor (199)); p.removeSelectedChop(); p.toggleSelectedChopFavorite();
        check (modeRejected && windowRejected && p.getMidiPerformanceMode() == Mode::Favorites
               && ! p.isManualChopModeActive() && p.getChopBarsCount() == savedBars && p.getChopState()->chops.size() == 220
               && p.getChopState()->favoriteMidiOwnerIds == lockedIds && idAt (p, 49) == idFor (199),
               "MIDI Lock rejects map, Window, layer, regrid and assigned-member deletion or removal");
        p.setSelectedChopGainDecibels (-6.0f);
        const auto soundState = p.getChopState();
        const auto sound = std::find_if (soundState->chops.begin(), soundState->chops.end(), [&] (const auto& c) { return c.id == idFor (199); });
        check (sound != soundState->chops.end() && std::abs (sound->gainDecibels + 6.0f) < 0.001f,
               "MIDI Lock permits sound edits to the assigned chop");
        p.setMidiMappingLocked (false);
        check (p.getChopState()->favoriteMidiOwnerIds == lockedIds && idAt (p, 49) == idFor (199), "unlocking alone preserves every committed binding");

        p.prepareToPlay (8000, 64);
        juce::AudioBuffer<float> audio (2, 64); juce::MidiBuffer midi;
        const auto render = [&] { p.processBlock (audio, midi); midi.clear(); };
        p.setMidiPerformanceMode (Mode::Window); p.setMidiWindowStart (0); p.setChopPlaybackMode (P::ChopPlaybackMode::Gate);
        midi.addEvent (juce::MidiMessage::noteOn (3, 48, (juce::uint8) 100), 0); render();
        const int heldId = p.getPlayingChopId();
        p.setMidiWindowStart (164); render();
        check (heldId == idFor (0) && p.getPlayingChopId() == heldId && idAt (p, 48) == idFor (164),
               "committing a new Window changes future notes while the original Gate keeps its chop ID");
        midi.addEvent (juce::MidiMessage::noteOff (2, 48), 0); render();
        check (p.heldMidiNote.load() == 48, "another MIDI channel cannot release the pre-commit Gate");
        midi.addEvent (juce::MidiMessage::noteOff (3, 48), 0); render(); for (int block = 0; block < 10; ++block) render();
        check (! p.isPlaying(), "the original channel and note release a Gate after the map changes");
        p.setMidiWindowStart (0); p.setChopPlaybackMode (P::ChopPlaybackMode::OneShot);
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0); render();
        p.setMidiWindowStart (164); midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0); render();
        check (p.getPlayingChopId() == idFor (0), "One Shot ignores note-off and keeps its original chop through a Window change");
        for (int block = 0; block < 10; ++block) render();
        check (! p.isPlaying(), "the original One Shot still ends at its own boundary");

        p.setMidiPerformanceMode (Mode::Favorites);
        const auto automaticOwners = p.getChopState()->favoriteMidiOwnerIds;
        p.setManualChopModeActive (true); p.setMidiPerformanceMode (Mode::Window);
        check (idAt (p, 48) == 8, "manual layer creates an independent performance layout");
        p.setMidiMappingLocked (true);
        juce::MemoryBlock saved; p.getStateInformation (saved);
        P recalled; recalled.setStateInformation (saved.getData(), (int) saved.getSize());
        check (waitFor ([&] { return restored (recalled); }) && recalled.isManualChopModeActive()
               && recalled.isMidiMappingLocked() && recalled.getMidiPerformanceMode() == Mode::Window && idAt (recalled, 48) == 8,
               "project recall restores the active manual layer's map and MIDI Lock");
        recalled.setMidiMappingLocked (false); recalled.setManualChopModeActive (false);
        check (recalled.getMidiPerformanceMode() == Mode::Favorites && recalled.getMidiWindowStart() == 164
               && recalled.getChopState()->favoriteMidiOwnerIds == automaticOwners && idAt (recalled, 49) == idFor (199),
               "project recall retains the inactive layer's Window, Favorites reservations and chosen mode");
        recalled.setMidiPerformanceMode (Mode::Legacy);
        check (recalled.getMidiNoteForChopId (idFor (0)) == 84 && recalled.getMidiNoteForChopId (idFor (1)) == -1,
               "opting out of performance mapping restores the original explicit and unassigned legacy notes");

        {
            P regrid; setup (regrid); regrid.chopBarsCount.store (1);
            auto original = std::make_shared<P::ChopState>();
            for (const auto fixture : { std::array<int, 4> { 101, 0, 16000, 12000 },
                                       { 202, 16000, 32000, 0 }, { 303, 56000, 64000, 0 } })
            {
                P::ChopDefinition chop;
                chop.id = fixture[0]; chop.startSample = fixture[1]; chop.endSample = fixture[2];
                chop.cueOffsetSamples = fixture[3]; chop.favorite = true; chop.favoriteOrder = (int64_t) original->chops.size() + 1;
                original->chops.push_back (chop);
            }
            original->selectedChopId = 101; original->nextChopId = 304;
            regrid.publishChopState (original); regrid.setMidiPerformanceMode (Mode::Window);
            regrid.setMidiPerformanceMode (Mode::Favorites); regrid.selectChopById (303); regrid.removeSelectedChop();
            P::TempoAnalysisData analysis;
            analysis.estimatedBpm = 240; analysis.beatPeriodSeconds = 0.25; analysis.analysisEndSeconds = 8;
            regrid.buildChopsFromAnalysis (analysis);
            const auto split = regrid.getChopState();
            const int splitFirst = idAt (regrid, 48), splitSecond = idAt (regrid, 49);
            const auto firstChild = std::find_if (split->chops.begin(), split->chops.end(),
                [splitFirst] (const auto& c) { return c.id == splitFirst; });
            check (firstChild != split->chops.end() && firstChild->startSample == 8000 && firstChild->endSample == 16000
                   && splitFirst != splitSecond && split->midiWindowChopIds[0] == splitFirst
                   && firstChild->favorite && firstChild->favoriteOrder == 1 && split->favoriteChopIndices.size() == 2
                   && split->favoriteMidiOwnerIds[2] == 303 && idAt (regrid, 50) == -1,
                   "split regrid follows the owner's cue once, shares Window identity and retains deleted-owner gaps");
            analysis.estimatedBpm = 60; analysis.beatPeriodSeconds = 1.0;
            regrid.buildChopsFromAnalysis (analysis);
            const auto merged = regrid.getChopState();
            check (idAt (regrid, 48) >= 0 && idAt (regrid, 49) == -1 && idAt (regrid, 50) == -1
                   && merged->favoriteMidiOwnerIds[1] == splitSecond && merged->favoriteMidiOwnerIds[2] == 303
                   && merged->midiWindowChopIds[0] == idAt (regrid, 48) && regrid.getMappedMidiNotes().count() == 1
                   && merged->favoriteChopIndices.size() == 1,
                   "merge regrid gives one descendant to the first slot and preserves unresolved owners without duplicate notes");
            analysis.estimatedBpm = 240; analysis.beatPeriodSeconds = 0.25;
            regrid.buildChopsFromAnalysis (analysis);
            check (regrid.getChopState()->favoriteMidiOwnerIds[1] == splitSecond
                   && regrid.getChopState()->favoriteMidiOwnerIds[2] == 303 && idAt (regrid, 49) == -1 && idAt (regrid, 50) == -1,
                   "later regrids cannot assign unrelated nearby audio to unresolved reservations");
        }

        P ui; std::atomic_store (&ui.loadedSample, std::const_pointer_cast<P::LoadedSampleData> (source));
        ui.publishChopState (makeSong (220)); ui.setMidiPerformanceMode (Mode::Window);
        std::unique_ptr<juce::AudioProcessorEditor> editor (ui.createEditor());
        if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
        editor->addToDesktop (juce::ComponentPeer::windowIsTemporary); editor->setVisible (true);
        auto* pads = findComponentId (*editor, "chop-pads");
        auto* modes = dynamic_cast<juce::ComboBox*> (findComponentId (*editor, "midi-map-mode"));
        auto* lock = dynamic_cast<juce::Button*> (findComponentId (*editor, "midi-map-lock"));
        auto* handle = findComponentId (*editor, "midi-window-handle");
        check (pads != nullptr && modes != nullptr && lock != nullptr && handle != nullptr, "performance mode, Lock and dedicated Window handle are present");
        if (pads == nullptr || modes == nullptr || lock == nullptr || handle == nullptr) return;
        check (modes->getNumItems() == 2 && modes->getItemText (0).equalsIgnoreCase ("Window")
               && modes->getItemText (1).equalsIgnoreCase ("Favorites"),
               "MIDI selector exposes only Window and Favorites");
        check (waitFor ([&] { return modes->getSelectedId() == 1 && (int) pads->getProperties()["midiMappedPadCount"] == 56; }),
               "Window UI identifies all 56 mapped pads without hiding the other chops");
        lock->onClick();
        check (ui.isMidiMappingLocked() && lock->getToggleState() && ! modes->isEnabled() && ! handle->isEnabled(),
               "MIDI Lock visibly disables map selection and Window dragging");
        lock->onClick();
        const int columns = juce::jmax (1, (int) pads->getProperties()["padColumns"]);
        const auto point = handle->getLocalBounds().toFloat().getCentre();
        const auto eventAt = [&] (juce::Point<float> position)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position,
                juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
                handle, handle, now, point, now, 1, false);
        };
        const auto dragged = point.translated (3.0f * (float) pads->getWidth() / (float) columns, 0.0f);
        handle->mouseDown (eventAt (point)); handle->mouseDrag (eventAt (dragged));
        check (ui.getMidiWindowStart() == 0 && (int) pads->getProperties()["midiWindowPreviewStart"] == 3 && ! ui.isPlaying(),
               "Window dragging previews whole chops without remapping MIDI or auditioning");
        handle->mouseUp (eventAt (dragged));
        check (ui.getMidiWindowStart() == 3 && idAt (ui, 48) == idFor (3), "releasing the handle commits the previewed Window");
        ui.undoLastEdit();
        check (ui.getMidiWindowStart() == 0 && idAt (ui, 48) == idFor (0), "one Undo restores the complete pre-drag Window");
        ui.setMidiPerformanceMode (Mode::Legacy);
        check (waitFor ([&] { return modes->getSelectedId() == 1 && handle->isEnabled()
            && (int) pads->getProperties()["midiMappedPadCount"] == 1; })
               && idAt (ui, 84) == idFor (0) && idAt (ui, 48) == -1,
               "an older saved map displays Window while retaining its exact original MIDI assignments");
        handle->mouseDown (eventAt (point));
        check (ui.getMidiPerformanceMode() == Mode::Legacy && idAt (ui, 84) == idFor (0) && idAt (ui, 48) == -1,
               "pressing the saved-map Window handle does not migrate MIDI before release");
        handle->mouseUp (eventAt (point));
        check (ui.getMidiPerformanceMode() == Mode::Window && idAt (ui, 48) == idFor (0),
               "releasing the Window handle explicitly activates its performance map");
        ui.undoLastEdit();
        check (ui.getMidiPerformanceMode() == Mode::Legacy && idAt (ui, 84) == idFor (0) && idAt (ui, 48) == -1,
               "one Undo restores both the old saved mode and its original MIDI assignments");
        ui.setMidiPerformanceMode (Mode::Window);
        modes->setSelectedId (2, juce::sendNotificationSync);
        check (ui.getMidiPerformanceMode() == Mode::Favorites && ! handle->isEnabled(), "explicit mode selector changes MIDI source and disables the inactive Window handle");
        for (const int ordinal : { 0, 199 }) { ui.selectChopById (idFor (ordinal)); ui.toggleSelectedChopFavorite(); }
        juce::Button* theme = nullptr;
        std::function<void (juce::Component&)> findTheme = [&] (juce::Component& component)
        {
            if (auto* button = dynamic_cast<juce::Button*> (&component);
                button != nullptr && button->getTooltip() == "Switch between light and dark themes.") theme = button;
            for (auto* child : component.getChildren()) findTheme (*child);
        };
        findTheme (*editor);
        const auto previousTheme = theme != nullptr ? theme->getButtonText() : juce::String();
        for (const int modeId : { 1, 2 })
        {
            modes->setSelectedId (modeId, juce::sendNotificationSync);
            for (const auto* themeName : { "dark", "light" })
            {
                if (theme != nullptr && theme->onClick && theme->getButtonText() != juce::String (themeName).toUpperCase()) theme->onClick();
                check (waitFor ([&] { return (int) pads->getProperties()["padFavoriteCount"] == 2
                    && (int) pads->getProperties()["padAnimatingCount"] == 0; }),
                       "distant favorite pad highlights settle in either MIDI mode and theme");
                const auto padImage = pads->createComponentSnapshot (pads->getLocalBounds());
                const int padColumns = juce::jmax (1, (int) pads->getProperties()["padColumns"]);
                const int padRows = juce::jmax (1, (int) pads->getProperties()["padRows"]);
                bool filledPink = true;
                for (const int ordinal : { 0, 199 })
                {
                    const int x = juce::roundToInt (((float) (ordinal % padColumns) + 0.25f) * pads->getWidth() / padColumns);
                    const int y = juce::roundToInt (((float) (ordinal / padColumns) + 0.25f) * pads->getHeight() / padRows);
                    const auto colour = padImage.getPixelAt (x, y);
                    filledPink &= colour.getRed() > colour.getGreen() + 20 && colour.getBlue() > colour.getGreen() + 15;
                }
                check (filledPink, "favorite pads use a pink face rather than only a colored border");
                juce::FileOutputStream snapshot (scratch.getParentDirectory().getChildFile (
                    "midi-performance-" + juce::String (modeId == 1 ? "window-" : "favorites-") + themeName + ".png"));
                snapshot.setPosition (0); snapshot.truncate();
                juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), snapshot);
            }
        }
        if (theme != nullptr && theme->onClick && theme->getButtonText() != previousTheme) theme->onClick();
        modes->setSelectedId (1, juce::sendNotificationSync);
        handle->mouseDown (eventAt (point)); handle->mouseDrag (eventAt (dragged));
        editor.reset();
        for (int tick = 0; tick < 10; ++tick) pump();
        check (ui.getMidiWindowStart() == 0 && ! ui.isPlaying() && ! ui.mousePreviewHeld.load(),
               "closing during a Window drag cancels its preview without committing or leaving an audition held");
    }

    static void runChopPads (const juce::File& scratch)
    {
        P p;
        auto source = std::make_shared<P::LoadedSampleData>();
        source->sampleRate = 8000;
        source->fileName = "chronological pad fixture";
        source->buffer.setSize (2, 512 * 1024);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < source->buffer.getNumSamples(); ++sample)
                source->buffer.setSample (channel, sample, 0.2f * std::sin ((float) sample * 0.05f));
        std::atomic_store (&p.loadedSample, source);

        // Storage order, IDs, MIDI notes and favorite order all differ from
        // source time. Equal starts additionally exercise the end/ID tie break.
        auto layout = std::make_shared<P::ChopState>();
        for (const auto fixture : { std::array<int, 4> { 71, 3072, 4096, 72 },
                                   { 9, 1024, 2048, 36 }, { 50, 0, 512, -2 },
                                   { 22, 0, 1024, 60 }, { 4, 0, 512, 84 } })
        {
            P::ChopDefinition chop;
            chop.id = fixture[0]; chop.startSample = fixture[1]; chop.endSample = fixture[2];
            chop.assignedMidiNote = fixture[3]; chop.cueOffsetSamples = 32;
            chop.releaseMilliseconds = 5.0f;
            chop.favorite = chop.id == 9; chop.favoriteOrder = chop.favorite ? 1 : -1;
            layout->chops.push_back (chop);
        }
        layout->selectedChopId = 9; layout->nextChopId = 72;
        p.publishChopState (layout);
        const auto originalMap = p.getChopState()->midiMap.channelNoteToChopIndex;
        p.prepareToPlay (8000, 64);
        juce::AudioBuffer<float> audio (2, 64); juce::MidiBuffer midi;
        const auto render = [&] (int blocks = 1)
        {
            for (int block = 0; block < blocks; ++block) { midi.clear(); p.processBlock (audio, midi); }
        };
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
        editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
        editor->setVisible (true);
        auto* pads = findComponentId (*editor, "chop-pads");
        check (pads != nullptr && findType (*editor, "GlassKeyboard") == nullptr,
               "chronological pads replace the visual piano keyboard");
        if (pads == nullptr) return;
        const auto refreshCount = [&] (int count)
        {
            p.notifyEditStateChanged();
            return waitFor ([&] { return (int) pads->getProperties()["padCount"] == count; });
        };
        check (refreshCount (5), "pad grid exposes every chop independently of MIDI assignments");
        const auto cell = [&] (int index)
        {
            const int columns = juce::jmax (1, (int) pads->getProperties()["padColumns"]);
            const int rows = juce::jmax (1, (int) pads->getProperties()["padRows"]);
            const float width = (float) pads->getWidth() / (float) columns;
            const float height = (float) pads->getHeight() / (float) rows;
            return juce::Rectangle<float> ((float) (index % columns) * width,
                                          (float) (index / columns) * height, width, height);
        };
        const auto eventAt = [&] (juce::Point<float> point, int clicks = 1)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
                juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
                pads, pads, now, point, now, clicks, false);
        };
        const auto press = [&] (int index) { pads->mouseDown (eventAt (cell (index).getCentre())); };
        const auto release = [&] (int index) { pads->mouseUp (eventAt (cell (index).getCentre())); };
        const auto stop = [&] { p.stopPlayback(); render(); };

        bool chronological = true;
        int index = 0;
        for (const int id : { 4, 50, 22, 9, 71 })
        {
            press (index); render();
            chronological &= p.getLastTriggeredChopId() == id && p.getChopState()->selectedChopId == id
                && p.getPlayingChopId() == id && p.voice.playbackTriggeredByMouse
                && audio.getMagnitude (0, 64) > 0.001f;
            release (index++); render (10);
        }
        check (chronological, "pad hits follow start/end/ID order through the final chop");
        check (p.getChopState()->midiMap.channelNoteToChopIndex == originalMap
               && p.getMidiNoteForChopId (50) == -1,
               "clicking chronological pads preserves explicit and unmapped MIDI assignments");

        stop();
        check (p.getPlayingChopId() == -1 && p.getLastTriggeredChopId() == 71,
               "Stop clears the playing pad while retaining the historical trigger");
        // Start in the unchopped tail so this pad-identity check is independent
        // of the transport's per-chop envelope processing at source time zero.
        p.clearSelectedChop(); p.setPlaybackSamplePosition (8192); render(); p.startPlayback(); render();
        check (p.isPlaying() && p.getPlayingChopId() == -1 && p.getLastTriggeredChopId() == 71,
               "whole-sample transport does not illuminate the last auditioned pad");
        stop();
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        p.processBlock (audio, midi); midi.clear();
        check (p.getPlayingChopId() == 9, "host MIDI publishes the exact sounding pad ID");
        midi.addEvent (juce::MidiMessage::noteOff (1, 36), 0);
        p.processBlock (audio, midi); midi.clear(); render (10);
        press (1); render(); p.setPlaybackSamplePosition (4096); render();
        check (p.getPlayingChopId() == -1 && p.getLastTriggeredChopId() == 50,
               "seeking clears the playing pad binding without erasing trigger history");
        release (1); stop();
        p.selectChopById (9); p.armManualChopCapture (0);
        for (const int invalidId : { -1, 99999 })
        {
            p.startChopPreview (invalidId); render();
            check (p.manualChopCaptureArmed.load() && ! p.isPlaying() && p.getPlayingChopId() == -1
                   && p.getLastTriggeredChopId() == 50 && p.getChopState()->selectedChopId == 9,
                   "invalid pad preview IDs preserve armed manual capture and cannot audition another chop");
        }
        p.cancelManualChopCapture();

        p.setChopPlaybackMode (P::ChopPlaybackMode::OneShot);
        press (1); release (1); render();
        check (p.voice.playbackActive && p.voice.activeChopId == 50 && p.voice.chopOneShot
               && p.voice.playbackSamplePosition >= 32 && p.voice.playbackSamplePosition < 128,
               "quick unmapped One Shot pad click survives release and begins at its cue");
        render (20);
        check (! p.voice.playbackActive, "One Shot pad finishes at its chop end");
        press (1); render (20);
        check (! p.voice.playbackActive, "holding a One Shot pad does not loop");
        release (1);
        p.setChopPlaybackMode (P::ChopPlaybackMode::Gate);
        press (1); release (1); render (20);
        check (! p.voice.playbackActive && ! p.mousePreviewHeld.load(),
               "quick Gate pad click cannot strand a held voice");
        press (1); render (30);
        check (p.voice.playbackActive && ! p.voice.chopOneShot && p.voice.activeChopId == 50
               && p.voice.playbackSamplePosition >= 32 && p.voice.playbackSamplePosition < 512,
               "held Gate pad loops only its target chop");
        p.setChopPlaybackMode (P::ChopPlaybackMode::OneShot);
        release (1); render (20);
        check (! p.voice.playbackActive, "pad release respects the Gate mode latched at mouse-down");

        p.setMidiPerformanceMode (P::MidiPerformanceMode::Favorites);
        const auto favoritesMap = p.getChopState()->midiMap.channelNoteToChopIndex;
        p.setFavoritesViewEnabled (true);
        check (refreshCount (5) && pads->isShowing(), "Favorites retains all chronological pads");
        p.setChopPlaybackMode (P::ChopPlaybackMode::Gate);
        press (1); render();
        check (p.voice.activeChopId == 50 && p.getChopState()->selectedChopId == 50
               && ! p.isFavoritesViewEnabled() && p.getMidiNoteForChopId (50) == -1
               && p.getMidiPerformanceMode() == P::MidiPerformanceMode::Favorites
               && p.getChopState()->midiMap.channelNoteToChopIndex == favoritesMap,
               "an outside pad opens its waveform and auditions directly without changing Favorites MIDI");
        release (1); render (20);

        const auto isFavorite = [&] (int id)
        {
            const auto state = p.getChopState();
            const auto found = std::find_if (state->chops.begin(), state->chops.end(), [id] (const auto& chop) { return chop.id == id; });
            return found != state->chops.end() && found->favorite;
        };
        press (2); release (2); render (20);
        check (! isFavorite (22) && p.getChopState()->favoriteChopIndices.size() == 1,
               "a single pad click auditions without changing Favorites");
        pads->mouseExit (eventAt (cell (2).getCentre()));
        check (waitFor ([&] { return (int) pads->getProperties()["padAnimatingCount"] == 0; }), "pad highlights settle after audition");
        pads->mouseMove (eventAt (cell (2).getCentre()));
        check (waitFor ([&] { return (int) pads->getProperties()["padAnimatingCount"] > 0; }), "hovering a pad starts a smooth highlight transition");
        check (waitFor ([&] { return (int) pads->getProperties()["padAnimatingCount"] == 0; }), "hover highlight animation settles without changing playback");
        pads->mouseExit (eventAt (cell (2).getCentre()));

        press (1); release (1); render (20);
        const auto doubleTap = eventAt (cell (1).getCentre(), 2);
        pads->mouseDown (doubleTap); render();
        check (p.getPlayingChopId() == 50 && p.mousePreviewHeld.load(), "the second pad press still auditions its exact Gate chop");
        // JUCE dispatches mouseDoubleClick after the second mouseUp, carrying
        // the old left-button modifiers on both callbacks.
        pads->mouseUp (doubleTap);
        check (! isFavorite (50) && ! p.mousePreviewHeld.load(), "the second mouse-up releases Gate without toggling Favorites before JUCE's double-click callback");
        const auto beforeFavoriteToggle = p.getChopTriggerRevision();
        pads->mouseDoubleClick (doubleTap);
        check (p.getChopTriggerRevision() == beforeFavoriteToggle, "double-tap favorite callback does not retrigger the sounding chop");
        render (20);
        check (isFavorite (50) && isFavorite (9) && ! isFavorite (22) && p.getChopState()->favoriteChopIndices.size() == 2
               && p.getMidiPerformanceMode() == P::MidiPerformanceMode::Favorites && p.getMidiNoteForChopId (9) == 48
               && p.getMidiNoteForChopId (50) == 49,
               "double-tapping a chronological source pad favorites its exact sparse ID and appends without remapping neighbors");
        check (! p.isPlaying() && ! p.mousePreviewHeld.load(), "mouse-up releases Gate playback after a pad double-tap");
        const auto doubleClickPad = [&] (int slot)
        {
            press (slot); release (slot); render (20);
            const auto secondPress = eventAt (cell (slot).getCentre(), 2);
            pads->mouseDown (secondPress); pads->mouseUp (secondPress); pads->mouseDoubleClick (secondPress); render (20);
        };
        p.setMidiMappingLocked (true);
        doubleClickPad (1);
        check (isFavorite (50) && p.getMidiNoteForChopId (50) == 49, "MIDI Lock protects an assigned favorite from pad double-tap removal");
        p.setMidiMappingLocked (false);
        doubleClickPad (1);
        check (! isFavorite (50) && isFavorite (9) && p.getMidiNoteForChopId (9) == 48
               && p.getMidiNoteForChopId (50) == -1 && p.getChopState()->favoriteMidiOwnerIds[1] == 50,
               "double-tapping again removes only that favorite and preserves its MIDI reservation");
        press (0); release (0); render (20);
        const auto neighborPress = eventAt (cell (1).getCentre(), 2);
        pads->mouseDown (neighborPress); pads->mouseUp (neighborPress); pads->mouseDoubleClick (neighborPress); render (20);
        check (! isFavorite (4) && ! isFavorite (50) && isFavorite (9) && p.getChopState()->favoriteChopIndices.size() == 1,
               "rapid clicks on neighboring pads do not toggle Favorites when JUCE reports a double-click");

        stop();
        const int previousSelection = p.getChopState()->selectedChopId;
        const auto beforeGap = p.getChopState();
        const auto gap = eventAt ({ 0.0f, cell (0).getCentreY() });
        pads->mouseDown (gap); pads->mouseUp (gap); pads->mouseDoubleClick (gap); render();
        check (! p.voice.playbackActive && p.getChopState() == beforeGap,
               "pad edge gutter clicks cannot select, trigger or favorite chops");
        if ((int) pads->getProperties()["padColumns"] > 1)
        {
            const auto between = eventAt ({ cell (0).getRight(), cell (0).getCentreY() });
            pads->mouseDown (between); pads->mouseUp (between); render();
            check (! p.voice.playbackActive && p.getChopState()->selectedChopId == previousSelection,
                   "gaps between pads do not trigger either neighbor");
        }

        auto* theme = static_cast<juce::Button*> (nullptr);
        std::function<void (juce::Component&)> findTheme = [&] (juce::Component& component)
        {
            if (auto* button = dynamic_cast<juce::Button*> (&component);
                button != nullptr && button->getTooltip() == "Switch between light and dark themes.") theme = button;
            for (auto* child : component.getChildren()) findTheme (*child);
        };
        findTheme (*editor);
        const auto originalSize = juce::Point<int> (editor->getWidth(), editor->getHeight());
        const auto originalTheme = theme != nullptr ? theme->getButtonText() : juce::String();
        for (const int count : { 16, 64, 220, 512 })
        {
            auto song = std::make_shared<P::ChopState>();
            // Reverse storage order exposes accidental positional playback as
            // well as layouts that only show the current MIDI bank.
            for (int ordinal = count - 1; ordinal >= 0; --ordinal)
            {
                P::ChopDefinition chop;
                chop.id = ordinal + 1; chop.startSample = ordinal * 1024; chop.endSample = (ordinal + 1) * 1024;
                chop.assignedMidiNote = -2;
                song->chops.push_back (chop);
            }
            song->selectedChopId = 1; song->nextChopId = count + 1;
            p.publishChopState (song);
            check (refreshCount (count), "pad count grows to include the complete song");
            for (const float scale : { 1.0f, 0.85f })
            {
                editor->setSize (juce::roundToInt ((float) originalSize.x * scale),
                                 juce::roundToInt ((float) originalSize.y * scale));
                const int columns = (int) pads->getProperties()["padColumns"];
                const int rows = (int) pads->getProperties()["padRows"];
                bool allInside = columns > 0 && rows > 0 && columns * rows >= count;
                for (int pad = 0; pad < count; ++pad)
                    allInside &= pads->getLocalBounds().toFloat().contains (cell (pad).getCentre());
                check (allInside && pads->isShowing()
                       && editor->getLocalBounds().contains (editor->getLocalArea (pads, pads->getLocalBounds())),
                       "every pad fits inside the grid at normal and minimum editor sizes");
                press (count - 1); render();
                check (p.voice.activeChopId == count && p.getChopState()->selectedChopId == count,
                       "the final unmapped pad remains playable after resizing");
                release (count - 1); render (20);
                if (columns * rows > count)
                {
                    const auto blank = eventAt (cell (count).getCentre());
                    pads->mouseDown (blank); pads->mouseUp (blank); render();
                    check (! p.voice.playbackActive && p.getChopState()->selectedChopId == count,
                           "unused final-row space does not trigger the last chop");
                    const auto beforeDoubleTap = p.getChopState();
                    pads->mouseDoubleClick (eventAt (cell (count).getCentre(), 2));
                    check (p.getChopState() == beforeDoubleTap && ! p.isPlaying(),
                           "double-tapping an empty grid cell does not favorite a neighboring chop");
                }
            }
            editor->setSize (originalSize.x, originalSize.y);
            if (theme != nullptr && theme->onClick)
                for (const auto* themeName : { "dark", "light" })
                {
                    if (theme->getButtonText() != juce::String (themeName).toUpperCase()) theme->onClick();
                    pump();
                    juce::FileOutputStream image (scratch.getParentDirectory().getChildFile (
                        "chop-pads-" + juce::String (count) + "-" + themeName + ".png"));
                    image.setPosition (0); image.truncate();
                    juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), image);
                }
        }
        if (theme != nullptr && theme->onClick && theme->getButtonText() != originalTheme) theme->onClick();

        p.publishChopState (std::make_shared<P::ChopState>());
        check (refreshCount (0), "empty sample state clears all pads");
        stop();
        const auto emptyClick = eventAt (pads->getLocalBounds().toFloat().getCentre());
        pads->mouseDown (emptyClick); pads->mouseUp (emptyClick); pads->mouseDoubleClick (emptyClick); render();
        check (! p.voice.playbackActive && ! p.mousePreviewHeld.load(), "empty pad grid clicks do not start transport playback");

        p.publishChopState (layout); refreshCount (5);
        press (1); render(); editor.reset(); render (20);
        check (! p.voice.playbackActive && ! p.mousePreviewHeld.load(), "closing the editor releases a held pad Gate");
    }

    static void runOctaveBanking (const juce::File& scratch)
    {
        {
            std::vector<juce::MidiMessage> preview, host;
            cuesampler::LaptopNoteRouter router (
                [&] (const auto& message) { preview.push_back (message); },
                [&] (const auto& message) { host.push_back (message); });
            router.send (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), false);
            router.send (juce::MidiMessage::noteOff (1, 48), true);
            check (preview.size() == 2 && host.empty() && preview.back().isNoteOff(),
                   "connecting FL cannot redirect a local held note's release or duplicate playback");
            router.send (juce::MidiMessage::noteOn (3, 107, (juce::uint8) 100), true);
            router.send (juce::MidiMessage::noteOff (3, 107), false);
            check (preview.size() == 2 && host.size() == 2 && host.back().getChannel() == 3
                   && host.back().getNoteNumber() == 107 && host.back().isNoteOff(),
                   "disconnecting FL retains the original remote channel and pitch for release");

            cuesampler::LaptopPhysicalKeys physical;
            cuesampler::LaptopMidiInput input ([&] (const auto& message) { router.send (message, false); });
            auto down = [&] (int key) { return input.press (key, 48, 1); };
            auto up = [&] (int key) { input.releaseKey (key); };
            physical.down (6, 'Z', false, down);
            physical.releaseAll ([&] { input.releaseAll(); });
            const auto count = preview.size();
            physical.down (6, 'Z', true, down);
            check (preview.size() == count, "native held-key repeats cannot retrigger after OCT or focus changes");
            physical.reconcile ([] (int) { return false; }, up);
            physical.down (6, 'Z', false, down);
            check (preview.size() == count + 1 && preview.back().isNoteOn(),
                   "physical reconciliation clears blocked laptop keys without JUCE's event cache");
            physical.up (6, up);
        }
        {
            using Result = cuesampler::FlOctaveShortcutResult;
            using Layout = cuesampler::FlTypingLayout;
            std::vector<int> dispatched;
            const auto post = [&] (cuesampler::FlOctaveShortcut shortcut)
            {
                dispatched.push_back (shortcut.octave);
                return true;
            };
            for (int octave = 2; octave <= 6; ++octave)
            {
                const auto key = cuesampler::makeFlOctaveShortcut (octave * 12);
                check (key && key->macKeyCode == 84 + octave - 2,
                       "FL shortcut uses the absolute numeric-keypad octave key");
                check (cuesampler::dispatchFlOctaveShortcut (octave * 12, { true, true, false, true }, post) == Result::queued,
                       "active FL window receives each supported octave request");
            }
            check (dispatched == std::vector<int> ({ 2, 3, 4, 5, 6 }), "octave requests are absolute, with no accumulating relative transpose");
            for (int octave = 4; octave <= 8; ++octave)
            {
                const auto key = cuesampler::makeFlOctaveShortcut (octave * 12, Layout::extended);
                check (key && key->octave == octave - 2 && key->macKeyCode == 84 + octave - 4,
                       "extended C4-C8 requests still use only documented FL shortcuts 2-6");
            }
            check (! cuesampler::makeFlOctaveShortcut (36, Layout::extended)
                   && ! cuesampler::makeFlOctaveShortcut (108, Layout::extended),
                   "extended layout cannot request an unsupported FL shortcut");
            const auto layoutFile = juce::File (__FILE__).getParentDirectory().getParentDirectory()
                .getChildFile ("assets/fl-studio/CUE SAMPLER Extended.scr");
            const auto lines = juce::StringArray::fromLines (layoutFile.loadFileAsString().trimEnd());
            check (lines.size() == 52 && lines[42] == "72" && lines[14] == "84", "extended layout preserves the normal Z/Q piano rows");
            std::bitset<128> reachable;
            for (int octave = 4; octave <= 8; ++octave)
            {
                const auto key = cuesampler::makeFlOctaveShortcut (octave * 12, Layout::extended);
                for (const auto& line : lines)
                    if (line.containsOnly ("0123456789"))
                    {
                        const int note = line.getIntValue() + (key->octave - 4) * 12;
                        if (note >= 0 && note <= 127) reachable.set ((size_t) note);
                    }
                check (lines[42].getIntValue() + (key->octave - 4) * 12 == octave * 12,
                       "same laptop key records the requested absolute extended octave");
            }
            check (reachable.count() == 80 && reachable.test (48) && reachable.test (127),
                   "extended layout and documented shortcuts cover every MIDI note from C4 to G10");
            for (const int count : { 1, 12, 13, 36, 37, 80, 100 })
            {
                std::bitset<128> mapped;
                for (int i = 0; i < count && 48 + i < 128; ++i) mapped.set ((size_t) (48 + i));
                const int last = (juce::jmin (127, 48 + count - 1) / 12) * 12;
                const auto view = cuesampler::keyboardOctaveRange (mapped);
                const auto extended = cuesampler::keyboardOctaveRange (mapped, 48, 96, 32);
                check (view.hasNotes && view.firstNote == 48 && view.lastNote == last,
                       "keyboard navigation follows mapped chops, including a partial final octave");
                check (extended.hasNotes && extended.firstNote == 48 && extended.lastNote == juce::jmin (96, last),
                       "extended range follows chops and keeps highest notes reachable on upper keys");
            }
            check (! cuesampler::keyboardOctaveRange ({}).hasNotes, "empty chop maps disable octave navigation");
            std::bitset<128> sparse; sparse.set (36); sparse.set (127);
            check (cuesampler::keyboardOctaveRange (sparse).firstNote == 36
                   && cuesampler::keyboardOctaveRange (sparse).lastNote == 120,
                   "range follows actual absolute pins rather than assuming chop count or root");
            for (const int note : { -12, 0, 12, 25, 73, 84, 120, 128 })
                check (cuesampler::dispatchFlOctaveShortcut (note, { true, true, false, true }, post) == Result::unsupportedOctave,
                       "unsupported octave cannot emit a different FL shortcut");
            check (cuesampler::dispatchFlOctaveShortcut (60, { false, true, false, true }, post) == Result::unsupportedHost,
                   "bridge does not dispatch in another host");
            check (cuesampler::dispatchFlOctaveShortcut (60, { true, false, false, true }, post) == Result::inactiveHost,
                   "bridge does not dispatch while FL is inactive");
            check (cuesampler::dispatchFlOctaveShortcut (60, { true, true, true, true }, post) == Result::keysHeld,
                   "held typing keys prevent octave changes and mismatched note-offs");
            check (cuesampler::dispatchFlOctaveShortcut (60, { true, true, false, false }, post) == Result::unavailableWindow,
                   "dialog or missing target window prevents dispatch");
            check (dispatched.size() == 5, "all blocked requests emit zero shortcuts");
            check (cuesampler::dispatchFlOctaveShortcut (60, { true, true, false, true }, [] (auto) { return false; }) == Result::failed,
                   "failed native dispatch is not reported as queued");
            if (! cuesampler::isFlOctaveShortcutAvailable())
                check (cuesampler::postFlOctaveShortcut (60) == Result::unsupportedHost,
                       "real native bridge refuses shortcuts in the test process");
        }
        P p;
        {
            using Result = cuesampler::FlOctaveShortcutResult;
            int view = 72;
            std::vector<int> requested;
            const auto post = [&] (int note) { requested.push_back (note); return Result::queued; };
            const auto show = [&] (int note) { view = note; };
            auto result = cuesampler::stepFlTypingOctave (view, -1, post, show);
            check (result == Result::queued && view == 60 && requested == std::vector<int> { 60 },
                   "OCT minus from C6 requests FL C5 and shows the same absolute octave");
            for (const auto blocked : { Result::keysHeld, Result::inactiveHost, Result::unavailableWindow, Result::failed })
            {
                result = cuesampler::stepFlTypingOctave (view, -1, [blocked] (int) { return blocked; }, show);
                check (result == blocked && view == 60, "blocked FL octave request cannot move the screen keyboard");
            }
            for (int i = 0; i < 10; ++i) cuesampler::stepFlTypingOctave (view, -1, post, show);
            check (view == 24 && requested == std::vector<int> ({ 60, 48, 36, 24 }),
                   "FL OCT minus stops at C2 without sending unsupported or repeated shortcuts");
            requested.clear();
            for (int i = 0; i < 4; ++i) cuesampler::stepFlTypingOctave (view, 1, post, show);
            check (view == 72 && requested == std::vector<int> ({ 36, 48, 60, 72 }),
                   "FL OCT plus reaches C6 with matching native octave requests");
            for (int i = 0; i < 10; ++i) result = cuesampler::stepFlTypingOctave (view, 1, post, show);
            check (view == 120 && ! result && requested == std::vector<int> ({ 36, 48, 60, 72 }),
                   "OCT browses screen keys through C10 without sending unsupported FL shortcuts");
            for (int i = 0; i < 3; ++i)
                check (cuesampler::stepFlTypingOctave (view, -1, post, show) == Result::viewOnly,
                       "higher screen-keyboard navigation leaves FL's typing octave unchanged");
            check (view == 84 && cuesampler::flTypingViewForNote (view, 72) == 84,
                   "FL typing and clips cannot pull the user out of the higher screen-keyboard view");
            result = cuesampler::stepFlTypingOctave (view, -1, [] (int) { return Result::keysHeld; }, show);
            check (result == Result::keysHeld && view == 84, "blocked return to native typing keeps the C7 view intact");
            result = cuesampler::stepFlTypingOctave (view, -1, post, show);
            check (result == Result::queued && view == 72 && requested.back() == 72,
                   "returning from screen C7 to C6 resynchronizes FL's actual octave");
            view = 12;
            requested.clear();
            result = cuesampler::stepFlTypingOctave (view, -1, post, show, 0);
            check (result == Result::viewOnly && view == 0 && requested.empty(),
                   "old C0 projects retain their lower screen keys without unsupported native commands");
            check (cuesampler::flTypingViewForNote (0, 72) == 72,
                   "first FL C6 input reveals the matching C6 screen octave");
            check (cuesampler::flTypingViewForNote (60, 72) == 60
                   && cuesampler::flTypingViewForNote (60, 84) == 60,
                   "overlapping typing rows keep one stable viewport and their actual note labels");
            check (cuesampler::flTypingViewForNote (72, 24) == 24,
                   "changing FL's octave externally reveals lower incoming notes");
            check (cuesampler::flTypingViewForNote (72, 0) == 72
                   && cuesampler::flTypingViewForNote (72, 127) == 72,
                   "non-typing controller notes cannot invent unsupported FL octave commands");
        }
        check (p.getParameters().isEmpty(), "FL Link controller identity is absent from the testing build");
        auto source = std::make_shared<P::LoadedSampleData>();
        source->sampleRate = 8000; source->fileName = "octave fixture";
        source->buffer.setSize (2, 220 * 128);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < source->buffer.getNumSamples(); ++i)
                source->buffer.setSample (ch, i, 0.2f * std::sin ((float) i * 0.1f));
        juce::MemoryBlock sourceState;
        check (p.serializeSampleToStateData (*source, sourceState), "encode octave fixture for project recall");
        source->serializedStateData = juce::var (sourceState);
        {
            P fresh;
            check (fresh.getMidiRootNote() == 24 && fresh.getKeyboardStartNote() == 24,
                   "new processors start their first chop and keyboard view at FL C2");
            std::atomic_store (&fresh.loadedSample, source);
            auto chops = std::make_shared<P::ChopState>();
            for (int i = 0; i < 220; ++i)
            {
                P::ChopDefinition chop;
                chop.id = i + 1; chop.startSample = i * 128; chop.endSample = (i + 1) * 128;
                chops->chops.push_back (chop);
            }
            chops->selectedChopId = 1; chops->nextChopId = 221;
            fresh.publishChopState (chops); fresh.prepareToPlay (8000, 64);
            check (fresh.getMidiNoteForChopId (1) == 24 && fresh.getMidiNoteForChopId (49) == 72
                   && fresh.getMidiNoteForChopId (61) == 84 && fresh.getMidiNoteForChopId (104) == 127
                   && fresh.getMidiNoteForChopId (105) == 24 && fresh.getMidiChannelForChopId (105) == 2
                   && fresh.getMidiBankCount() == 3 && fresh.getUnreachableChopCount() == 0,
                   "C2-root chops fill C2-G10 and restart at C2 on each later MIDI channel");
            juce::AudioBuffer<float> audio (2, 64); juce::MidiBuffer midi;
            const auto render = [&] { fresh.processBlock (audio, midi); midi.clear(); };
            bool complete = true;
            for (int i = 0; i < 220; ++i)
            {
                const int note = 24 + i % 104, channel = i / 104 + 1;
                fresh.keyboardState.noteOn (channel, note, 1.0f); render();
                complete &= fresh.getLastTriggeredChopId() == i + 1;
                fresh.keyboardState.noteOff (channel, note, 0.0f); render();
                midi.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) 100), 0); render();
                complete &= fresh.getLastTriggeredChopId() == i + 1;
                midi.addEvent (juce::MidiMessage::noteOff (channel, note), 0); render();
            }
            check (complete, "all 220 C2-root screen and host channel/note pairs trigger the same chops");
            fresh.setMidiOctaveOffset (7);
            midi.addEvent (juce::MidiMessage::noteOn (1, 24, (juce::uint8) 100), 0); render();
            check (fresh.getLastTriggeredChopId() == 1, "first chop remains DAW C2 while viewing higher screen octaves");
            midi.addEvent (juce::MidiMessage::noteOff (1, 24), 0); render();
            fresh.keyboardState.noteOn (1, 127, 1.0f); render();
            check (fresh.getLastTriggeredChopId() == 104, "G10 screen key reaches the last chop of the C2-root range");
            fresh.keyboardState.noteOff (1, 127, 0.0f); render();
            juce::MemoryBlock saved; fresh.getStateInformation (saved);
            P recall; recall.setStateInformation (saved.getData(), (int) saved.getSize());
            check (waitFor ([&] { return restored (recall); }) && recall.getMidiRootNote() == 24
                   && recall.getKeyboardStartNote() == 84 && recall.getMidiNoteForChopId (1) == 24
                   && recall.getMidiChannelForChopId (209) == 3,
                   "C2 mapping, overflow ranges and the higher screen view survive project recall");
        }
        // Keep the existing full-128-note fixture to verify old C0 projects.
        p.recordedMidiRoot.store (0); p.setMidiOctaveOffset (0);
        std::atomic_store (&p.loadedSample, source);
        auto layout = std::make_shared<P::ChopState>();
        for (int i = 0; i < 128; ++i)
        {
            P::ChopDefinition chop;
            chop.id = i + 1; chop.startSample = i * 128; chop.endSample = (i + 1) * 128;
            layout->chops.push_back (chop);
        }
        layout->selectedChopId = 1; layout->nextChopId = 129;
        p.publishChopState (layout); p.prepareToPlay (8000, 64);
        const auto original = p.getChopState();
        check (cuesampler::midiNoteName (0) == "C0" && cuesampler::midiNoteName (12) == "C1"
               && cuesampler::midiNoteName (48) == "C4" && cuesampler::midiNoteName (60) == "C5"
               && cuesampler::midiNoteName (72) == "C6" && cuesampler::midiNoteName (127) == "G10",
               "note labels match FL Studio throughout the MIDI range");
        check (p.getMidiRootNote() == 0 && p.isMidiBankModeEnabled()
               && p.getMidiNoteForChopId (1) == 0 && p.getMidiNoteForChopId (13) == 12
               && p.getMidiNoteForChopId (100) == 99,
               "saved C0 positional chops keep their original consecutive absolute MIDI notes");
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        if (auto* orb = findType (*editor, "VisualizerOrb")) orb->setVisible (false);
        editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
        editor->setVisible (true);
        juce::Button* plus = nullptr; juce::Button* minus = nullptr; juce::Button* flLink = nullptr;
        juce::Button* theme = nullptr;
        juce::Slider* chopRange = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* b = dynamic_cast<juce::Button*> (&c))
            {
                if (b->getComponentID() == "octave-up") plus = b;
                if (b->getComponentID() == "octave-down") minus = b;
                if (b->getComponentID() == "fl-octave-link") flLink = b;
                if (b->getTooltip() == "Switch between light and dark themes.") theme = b;
            }
            if (auto* slider = dynamic_cast<juce::Slider*> (&c); slider != nullptr && slider->getComponentID() == "chop-range")
                chopRange = slider;
            for (auto* child : c.getChildren()) find (*child);
        };
        find (*editor);
        check (chopRange == nullptr,
               "CHOPS slider is absent from the editor");
        auto* pads = findComponentId (*editor, "chop-pads");
        check (plus != nullptr && minus != nullptr && pads != nullptr, "FL octave controls and chronological chop pads are available");
        if (plus == nullptr || minus == nullptr || pads == nullptr) return;
        check (! plus->isShowing() && ! minus->isShowing() && pads->isShowing(),
               "non-FL hosts show every chop pad and hide FL-only octave controls");
        check (flLink == nullptr, "FL LINK button is absent from the testing build");
        check (! pads->keyPressed (juce::KeyPress ('z', 0, 0))
               && ! pads->keyPressed (juce::KeyPress ('a', 0, 0)),
               "CUE leaves laptop piano keys to the DAW");
        {
            juce::FileOutputStream image (scratch.getParentDirectory().getChildFile ("fl-octave-controls.png"));
            image.setPosition (0); image.truncate();
            auto* controls = plus->getParentComponent();
            juce::PNGImageFormat().writeImageToStream (controls->createComponentSnapshot (controls->getLocalBounds()), image);
        }
        check (waitFor ([&] { return (int) pads->getProperties()["padCount"] == 128; })
               && p.getKeyboardStartNote() == 0, "all 128 pads are visible with the original FL C0 MIDI mapping");
        if (theme != nullptr && theme->onClick)
        {
            // Visual QA artifacts use the real component tree. Restore both
            // the editor size and persisted theme before behavioral checks.
            struct RestoreVisualState
            {
                juce::Component& editor;
                juce::Button& theme;
                const juce::Point<int> size;
                const juce::String themeName;
                ~RestoreVisualState()
                {
                    editor.setSize (size.x, size.y);
                    if (theme.getButtonText() != themeName && theme.onClick) theme.onClick();
                }
            } restore { *editor, *theme, { editor->getWidth(), editor->getHeight() }, theme->getButtonText() };
            const auto writeSnapshot = [&] (const juce::String& name, juce::Rectangle<int> bounds)
            {
                juce::FileOutputStream image (scratch.getParentDirectory().getChildFile (name));
                image.setPosition (0); image.truncate();
                juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (bounds), image);
            };
            for (const auto* themeName : { "dark", "light" })
            {
                if (theme->getButtonText() != juce::String (themeName).toUpperCase()) theme->onClick();
                editor->setSize (restore.size.x, restore.size.y);
                const auto padBounds = editor->getLocalArea (pads, pads->getLocalBounds());
                const float scale = (float) padBounds.getHeight() / (float) pads->getHeight();
                const auto footerBounds = padBounds.toFloat().expanded (8.0f * scale, 4.0f * scale)
                    .withTop ((float) padBounds.getY() - 32.0f * scale)
                    .withBottom ((float) padBounds.getBottom() + 6.0f * scale)
                    .getSmallestIntegerContainer().getIntersection (editor->getLocalBounds());
                writeSnapshot ("pads-footer-" + juce::String (themeName) + ".png", footerBounds);
                writeSnapshot ("pads-ui-" + juce::String (themeName) + ".png", editor->getLocalBounds());
                editor->setSize (juce::roundToInt ((float) restore.size.x * 0.85f),
                                 juce::roundToInt ((float) restore.size.y * 0.85f));
                writeSnapshot ("pads-ui-minimum-" + juce::String (themeName) + ".png", editor->getLocalBounds());
            }
        }
        plus->onClick();
        check (waitFor ([&] { return p.getKeyboardStartNote() == 12; }), "OCT + selects FL C1");
        plus->onClick();
        check (waitFor ([&] { return p.getKeyboardStartNote() == 24; }), "second OCT + selects FL C2");
        {
            juce::FileOutputStream image (scratch.getParentDirectory().getChildFile ("octave-pads.png"));
            image.setPosition (0); image.truncate();
            juce::PNGImageFormat().writeImageToStream (pads->createComponentSnapshot (pads->getLocalBounds()), image);
        }
        check (p.getChopState() == original, "OCT navigation never republishes or remaps chops");
        juce::AudioBuffer<float> audio (2, 64); juce::MidiBuffer midi;
        const auto render = [&] { p.processBlock (audio, midi); midi.clear(); };
        // Reproduce the reported mismatch through the actual host audio path.
        // Even when the screen shows C2 or C0, FL C6 must remain MIDI 72.
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0);
        p.processBlock (audio, midi);
        check (midi.getNumEvents() == 1 && (*midi.begin()).getMessage().getNoteNumber() == 72,
               "host MIDI keeps its original C6 note");
        midi.clear();
        check (p.getLastTriggeredChopId() == 73
               && p.getHeldHostMidiNotes().test (72) && ! p.getHeldHostMidiNotes().test (24),
               "incoming FL C6 plays and illuminates CUE C6 while viewing C2");
        const auto observedC6 = p.getLatestHostMidiKeyEvent();
        check (observedC6 != 0 && (observedC6 & 2047u) == 72,
               "audio publishes the real incoming note/channel to the UI without translating MIDI");
        midi.addEvent (juce::MidiMessage::noteOff (1, 72), 0); render();
        check (p.getLatestHostMidiKeyEvent() == observedC6,
               "short host taps remain observable after note-off");
        for (int step = 0; step < 2; ++step) minus->onClick();
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0); render();
        check (p.getLatestHostMidiKeyEvent() != observedC6 && (p.getLatestHostMidiKeyEvent() & 2047u) == 72,
               "repeated host notes receive a fresh UI observation");
        check (p.getKeyboardStartNote() == 0 && (int) pads->getProperties()["padCount"] == 128
               && p.getLastTriggeredChopId() == 73,
               "viewing C0 cannot reinterpret incoming FL C6 as C0");
        midi.addEvent (juce::MidiMessage::noteOff (1, 72), 0); render();
        for (int step = 0; step < 10; ++step) plus->onClick();
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0); render();
        check (p.getKeyboardStartNote() == 120 && ! plus->isEnabled()
               && p.getLastTriggeredChopId() == 73 && p.getChopState() == original,
               "OCT C10 leaves recorded C6 playback and the chop map unchanged");
        midi.addEvent (juce::MidiMessage::noteOff (1, 72), 0); render();
        midi.addEvent (juce::MidiMessage::noteOn (1, 127, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 128 && p.getHeldHostMidiNotes().test (127),
               "absolute G10 reaches chop 128 and illuminates G10");
        midi.addEvent (juce::MidiMessage::noteOff (1, 127), 0); render();
        // Compare every screen note against host playback, including both ends
        // of the MIDI range, while OCT is deliberately on a different page.
        bool sameNotes = true;
        for (int note = 0; note < 128; ++note)
        {
            const auto beforeScreen = p.getLatestHostMidiKeyEvent();
            p.keyboardState.noteOn (1, note, 1.0f); render();
            const int screenChop = p.getLastTriggeredChopId();
            sameNotes &= p.getLatestHostMidiKeyEvent() == beforeScreen;
            p.keyboardState.noteOff (1, note, 0.0f); render();
            midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0); render();
            sameNotes &= screenChop == note + 1 && p.getLastTriggeredChopId() == screenChop;
            midi.addEvent (juce::MidiMessage::noteOff (1, note), 0); render();
        }
        check (sameNotes, "all 128 screen keys and DAW notes trigger the same chops");
        juce::MemoryBlock localSaved; p.getStateInformation (localSaved);
        auto localTree = juce::ValueTree::readFromData ((const char*) localSaved.getData() + 4, localSaved.getSize() - 4);
        check (! (bool) localTree.getProperty ("keyboardMidiNavigation")
               && (int) localTree.getProperty ("keyboardMidiInputBase") == -1,
               "saved state disables learned octave translation for older builds");
        // Projects from the previous build may still contain a learned C6 base.
        localTree.setProperty ("keyboardMidiNavigation", true, nullptr);
        localTree.setProperty ("keyboardMidiInputBase", 72, nullptr);
        P localRecall; localRecall.applyParsedRestoreState (p.parseDeferredRestoreState (localTree));
        std::atomic_store (&localRecall.loadedSample, source); localRecall.publishChopState (std::make_shared<P::ChopState> (*p.getChopState()));
        localRecall.prepareToPlay (8000, 64);
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0);
        localRecall.processBlock (audio, midi); midi.clear();
        check (localRecall.getKeyboardStartNote() == 120 && localRecall.getLastTriggeredChopId() == 73,
               "recall ignores obsolete learned transposition and keeps FL C6 at CUE C6");
        midi.addEvent (juce::MidiMessage::noteOn (1, 79, (juce::uint8) 100), 0); render();
        for (int step = 0; step < 8; ++step) minus->onClick();
        midi.addEvent (juce::MidiMessage::noteOff (2, 79), 0); render();
        check (p.heldMidiNote.load() == 79, "different MIDI channel cannot release the held voice");
        midi.addEvent (juce::MidiMessage::noteOff (1, 79), 0); render();
        check (p.heldMidiNote.load() == -1 && p.getHeldHostMidiNotes().none(),
               "matching note releases after OCT changes and clears its lamp");
        {
            int hostSends = 0;
            cuesampler::LaptopNoteRouter router ([&] (const auto& message)
                {
                    if (message.isNoteOn()) p.keyboardState.noteOn (message.getChannel(), message.getNoteNumber(), message.getFloatVelocity());
                    else p.keyboardState.noteOff (message.getChannel(), message.getNoteNumber(), 0.0f);
                }, [&] (const auto&) { ++hostSends; });
            cuesampler::LaptopMidiInput input ([&] (const auto& message) { router.send (message, false); });
            input.press ('Z', 84, 1); render();
            check (p.getLastTriggeredChopId() == 85 && hostSends == 0,
                   "unconnected laptop input plays its chop through the real UI-to-audio queue without a MIDI device");
            input.releaseKey ('Z'); render();
            check (p.onScreenMidiFifo.getNumReady() == 0 && hostSends == 0,
                   "local laptop release is consumed by processBlock without host routing");
        }
        midi.addEvent (juce::MidiMessage::noteOn (1, 0, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 1, "DAW C0 still plays chop 1 after OCT changes");
        midi.addEvent (juce::MidiMessage::noteOn (1, 12, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 13, "DAW C1 always plays its fixed C1 chop");
        // The keyboard-state listener remains available to local MIDI input;
        // direct pad mouse playback is covered independently in runChopPads.
        const int visibleNote = p.getKeyboardStartNote();
        p.keyboardState.noteOn (1, visibleNote, 1.0f); render();
        check (visibleNote == 24 && p.getLastTriggeredChopId() == 25,
               "local C2 MIDI plays its fixed C2 chop");
        p.keyboardState.noteOff (1, visibleNote, 0.0f); render();
        midi.addEvent (juce::MidiMessage::noteOn (1, 0, (juce::uint8) 100), 0); render();
        minus->onClick();
        midi.addEvent (juce::MidiMessage::noteOff (1, 0), 0); render();
        check (p.voice.envelopeReleaseTriggered || ! p.voice.playbackActive, "held MIDI notes release after octave navigation");
        juce::MemoryBlock saved; p.getStateInformation (saved);
        const auto tree = juce::ValueTree::readFromData ((const char*) saved.getData() + 4, saved.getSize() - 4);
        const auto parsed = p.parseDeferredRestoreState (tree);
        check (parsed.restoredMidiOctaveOffset == 1 && parsed.restoredRecordedMidiRoot == 0,
               "state saves the C1 viewport independently from fixed C0 mapping");
        P recall; recall.applyParsedRestoreState (parsed);
        std::atomic_store (&recall.loadedSample, source); recall.publishChopState (parsed.chopState);
        recall.prepareToPlay (8000, 64);
        midi.addEvent (juce::MidiMessage::noteOn (1, 0, (juce::uint8) 100), 0);
        recall.processBlock (audio, midi); midi.clear();
        check (recall.getKeyboardStartNote() == 12 && recall.getLastTriggeredChopId() == 1,
               "recalled viewport does not transpose DAW C0");
        auto c4Project = tree.createCopy();
        c4Project.setProperty ("recordedMidiRoot", 48, nullptr);
        c4Project.setProperty ("keyboardStartNote", 60, nullptr);
        c4Project.setProperty ("midiChannelBanks", false, nullptr);
        c4Project.removeProperty ("keyboardMidiNavigation", nullptr);
        c4Project.removeProperty ("keyboardMidiInputBase", nullptr);
        const auto c4Restore = p.parseDeferredRestoreState (c4Project);
        P c4Recall; c4Recall.applyParsedRestoreState (c4Restore);
        std::atomic_store (&c4Recall.loadedSample, source); c4Recall.publishChopState (c4Restore.chopState);
        c4Recall.prepareToPlay (8000, 64);
        bool legacyChannels = c4Recall.getMidiRootNote() == 48 && c4Recall.getKeyboardStartNote() == 60
                           && ! c4Recall.isMidiBankModeEnabled();
        for (int channel = 1; channel <= 16; ++channel)
        {
            midi.addEvent (juce::MidiMessage::noteOn (channel, 48, (juce::uint8) 100), 0);
            c4Recall.processBlock (audio, midi); midi.clear();
            legacyChannels &= c4Recall.getLastTriggeredChopId() == 1;
            midi.addEvent (juce::MidiMessage::noteOff (channel, 48), 0);
            c4Recall.processBlock (audio, midi); midi.clear();
        }
        check (legacyChannels, "existing C4 recordings retain their original pitch and all 16 channel aliases");
        check (c4Recall.getAvailableMidiBankCount() == 1,
               "legacy all-channel recordings do not expose a slider that would silently replace their assignments");
        juce::MemoryBlock c4Saved; c4Recall.getStateInformation (c4Saved);
        const auto c4Tree = juce::ValueTree::readFromData ((const char*) c4Saved.getData() + 4, c4Saved.getSize() - 4);
        check ((int) c4Tree.getProperty ("recordedMidiRoot") == 48
               && ! (bool) c4Tree.getProperty ("midiChannelBanks")
               && (int) c4Tree.getProperty ("keyboardStartNote") == 60,
               "saving an existing C4 project preserves its mapping mode and absolute viewport");
        auto bankedC4Project = c4Project.createCopy();
        bankedC4Project.setProperty ("midiChannelBanks", true, nullptr);
        const auto bankedC4Restore = p.parseDeferredRestoreState (bankedC4Project);
        P bankedC4Recall; bankedC4Recall.applyParsedRestoreState (bankedC4Restore);
        bankedC4Recall.publishChopState (bankedC4Restore.chopState);
        check (bankedC4Recall.getMidiRootNote() == 48 && bankedC4Recall.getMidiBankCount() == 2
               && bankedC4Recall.getMidiNoteForChopId (80) == 127
               && bankedC4Recall.getMidiNoteForChopId (81) == 48
               && bankedC4Recall.getMidiChannelForChopId (81) == 2,
               "existing banked C4 projects retain their original 80-note range assignments");
        auto legacy = tree.createCopy(); legacy.removeProperty ("keyboardStartNote", nullptr);
        legacy.setProperty ("recordedMidiRoot", 36, nullptr); legacy.setProperty ("midiOctaveOffset", 2, nullptr);
        const auto migrated = p.parseDeferredRestoreState (legacy);
        check (migrated.restoredRecordedMidiRoot == 36 && migrated.restoredMidiOctaveOffset == 4,
               "older saved assignments survive without treating transposition as a viewport");
        legacy.removeProperty ("recordedMidiRoot", nullptr);
        check (p.parseDeferredRestoreState (legacy).restoredRecordedMidiRoot == 60, "oldest projects retain their historical MIDI root");
        for (int i = 0; i < 12; ++i) plus->onClick();
        check (p.getKeyboardStartNote() == 120 && ! plus->isEnabled(), "OCT + stops at C10");
        for (int i = 0; i < 16; ++i) minus->onClick();
        check (p.getKeyboardStartNote() == 0 && ! minus->isEnabled(), "OCT - reaches C0 and stops only at the MIDI lower limit");
        auto pinned = std::make_shared<P::ChopState> (*layout); pinned->chops[0].assignedMidiNote = 84;
        p.publishChopState (pinned); p.setMidiOctaveOffset (3);
        check (p.getMidiNoteForChopId (1) == 84, "explicit MIDI assignments remain fixed after OCT navigation");
        midi.addEvent (juce::MidiMessage::noteOn (1, 84, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 1, "manual explicit note pins remain playable after octave navigation");
        midi.addEvent (juce::MidiMessage::noteOff (1, 84), 0); render();
        p.keyboardState.noteOn (1, 84, 1.0f); render();
        for (int i = 0; i < 300; ++i) p.enqueueOnScreenMidi (1, 84, 1.0f);
        render();
        check (p.onScreenMidiFifo.getNumReady() == 0 && ! p.voice.playbackActive, "screen MIDI queue overflow cannot strand a held voice");
        check ((int) pads->getProperties()["padCount"] == 128,
               "all pads remain present while local MIDI queue overflow is recovered");
        for (const int count : { 12, 36, 37, 49 })
        {
            auto partial = std::make_shared<P::ChopState> (*layout);
            partial->chops.resize ((size_t) count);
            p.publishChopState (partial); p.setMidiOctaveOffset (0);
            const auto fixedMap = p.getChopState();
            for (int step = 0; step < 10; ++step) plus->onClick();
            check (p.getKeyboardStartNote() == 120 && ! plus->isEnabled(),
                   "OCT reaches C10 even when the sample has fewer mapped chops");
            check (p.getChopState() == fixedMap && ! plus->getTooltip().contains ("CHOPS slider"),
                   "C10 limit preserves MIDI assignments and has no obsolete slider hint");
            check (chopRange == nullptr,
                   "short samples have no chop-range slider");
            midi.addEvent (juce::MidiMessage::noteOn (1, 0, (juce::uint8) 100), 0); render();
            check (p.getLastTriggeredChopId() == 1, "adaptive octave limits preserve recorded C0 playback");
            midi.addEvent (juce::MidiMessage::noteOff (1, 0), 0); render();
        }
        auto thresholdLayout = std::make_shared<P::ChopState>();
        for (int i = 0; i < 257; ++i)
        {
            P::ChopDefinition chop;
            chop.id = i + 1; chop.startSample = i * 128; chop.endSample = (i + 1) * 128;
            thresholdLayout->chops.push_back (chop);
        }
        thresholdLayout->selectedChopId = 1; thresholdLayout->nextChopId = 258;
        for (const int count : { 127, 128, 129, 256, 257 })
        {
            auto boundary = std::make_shared<P::ChopState> (*thresholdLayout);
            boundary->chops.resize ((size_t) count);
            p.publishChopState (boundary);
            p.notifyEditStateChanged();
            const int expectedRanges = (count + 127) / 128;
            check (waitFor ([&] { return p.getMidiBankCount() == expectedRanges && p.getUnreachableChopCount() == 0
                   && chopRange == nullptr; }),
                   "overflow channel mappings remain available without a range slider");
            check (p.getMidiNoteForChopId (count) == (count - 1) % 128
                   && p.getMidiChannelForChopId (count) == (count - 1) / 128 + 1,
                   "each exact range boundary restarts at C0 on the next fixed MIDI channel");
        }
        // Verify all absolute channel/note pairs across the overflow range.
        auto song = std::make_shared<P::ChopState>();
        for (int i = 0; i < 220; ++i)
        {
            P::ChopDefinition chop;
            chop.id = i + 1; chop.startSample = i * 128; chop.endSample = (i + 1) * 128;
            song->chops.push_back (chop);
        }
        song->selectedChopId = 1; song->nextChopId = 221;
        p.publishChopState (song);
        check (p.isMidiBankModeEnabled() && p.getMidiBankCount() == 2 && p.getUnreachableChopCount() == 0,
               "new samples automatically map 220 chops in two fixed MIDI-channel ranges");
        bool complete = true;
        for (int i = 0; i < 220; ++i)
        {
            const int channel = i / 128 + 1, note = i % 128;
            complete &= p.getMidiNoteForChopId (i + 1) == note && p.getMidiChannelForChopId (i + 1) == channel;
            midi.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) 100), 0); render();
            complete &= p.getLastTriggeredChopId() == i + 1;
            midi.addEvent (juce::MidiMessage::noteOff (channel, note), 0); render();
        }
        check (complete, "every one of 220 channel/note pairs triggers its own chop through processBlock");
        const auto bankMap = p.getChopState();
        p.setKeyboardMidiBank (1); p.setMidiOctaveOffset (0);
        for (int step = 0; step < 20; ++step) plus->onClick();
        check (p.getKeyboardMidiBank() == 1 && p.getKeyboardStartNote() == 120 && ! plus->isEnabled(),
               "OCT stops at C10 without automatically changing the chop range");
        check (chopRange == nullptr, "220 chops do not restore the removed range slider");
        p.setKeyboardMidiBank (2); p.setMidiOctaveOffset (0);
        check (waitFor ([&] { return (int) pads->getProperties()["padCount"] == 220; })
               && p.getKeyboardMidiBank() == 2 && p.getKeyboardStartNote() == 0,
               "all 220 pads remain available after restoring a MIDI channel and octave");
        for (int step = 0; step < 16; ++step) minus->onClick();
        check (p.getKeyboardMidiBank() == 2 && p.getKeyboardStartNote() == 0 && ! minus->isEnabled(),
               "OCT reaches C0 in the final chop range without switching ranges");
        for (int step = 0; step < 10; ++step) plus->onClick();
        check (p.getKeyboardStartNote() == 120 && (int) pads->getProperties()["padCount"] == 220,
               "last MIDI octave keeps every chronological pad visible");
        // Pumping the editor may update selection after the last played note;
        // compare the assignments rather than the immutable snapshot pointer.
        check (p.getChopState()->midiMap.channelNoteToChopIndex == bankMap->midiMap.channelNoteToChopIndex,
               "channel and octave navigation preserve the underlying absolute map");
        p.setMidiOctaveOffset (0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 73 && ! p.getHeldHostMidiNotes().test (72),
               "viewing range 2 does not redirect channel-1 C6 or light a channel-2 key");
        midi.addEvent (juce::MidiMessage::noteOff (1, 72), 0); render();
        midi.addEvent (juce::MidiMessage::noteOn (2, 0, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 129 && p.getHeldHostMidiNotes().test (0),
               "DAW channel-2 C0 matches the range-2 screen key");
        midi.addEvent (juce::MidiMessage::noteOff (2, 0), 0); render();
        p.setMidiOctaveOffset (7);
        midi.addEvent (juce::MidiMessage::noteOn (2, 91, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 220, "DAW channel-2 G7 reaches chop 220 at its displayed pitch");
        p.setKeyboardMidiBank (1); p.setMidiOctaveOffset (0);
        midi.addEvent (juce::MidiMessage::noteOff (2, 91), 0); render();
        check (p.heldMidiNote.load() == -1, "note-off releases safely even after range and octave changes");
        p.setKeyboardMidiBank (2); p.setMidiOctaveOffset (10);
        {
            juce::FileOutputStream image (scratch.getParentDirectory().getChildFile ("simple-octave-controls.png"));
            image.setPosition (0); image.truncate();
            juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), image);
        }
        midi.addEvent (juce::MidiMessage::noteOn (1, 0, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 1, "recorded channel-1 C0 keeps chop 1 while viewing range 2");
        midi.addEvent (juce::MidiMessage::noteOn (2, 0, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 129, "recorded channel-2 C0 keeps chop 129 while viewing range 2");
        midi.addEvent (juce::MidiMessage::noteOff (1, 0), 0); render();
        check (p.heldMidiNote.load() == 0 && p.heldMidiChannel == 2,
               "another bank's note-off cannot release the held bank-2 note");
        midi.addEvent (juce::MidiMessage::noteOff (2, 0), 0); render();
        check (p.heldMidiNote.load() == -1, "matching channel/note releases the banked note");
        p.keyboardState.noteOn (2, 91, 1.0f); render();
        check (p.getLastTriggeredChopId() == 220, "range-2 G7 screen key reaches the song's last chop");
        p.keyboardState.noteOff (2, 91, 0.0f); render();
        juce::MemoryBlock bankSaved; p.getStateInformation (bankSaved);
        P bankRecall; bankRecall.setStateInformation (bankSaved.getData(), (int) bankSaved.getSize());
        check (waitFor ([&] { return restored (bankRecall); }) && bankRecall.isMidiBankModeEnabled()
               && bankRecall.getKeyboardMidiBank() == 2 && bankRecall.getMidiChannelForChopId (220) == 2
               && bankRecall.getMidiNoteForChopId (220) == 91,
               "project recall retains all 220 assignments and selected bank");
        auto favoritesSong = std::make_shared<P::ChopState> (*song);
        for (int i = 0; i < 220; ++i)
        {
            favoritesSong->chops[(size_t) i].favorite = true;
            favoritesSong->chops[(size_t) i].favoriteOrder = 220 - i;
        }
        // Favorites use their packed absolute notes even if the underlying
        // chop has a manual pin.
        favoritesSong->chops[147].assignedMidiNote = 72;
        p.publishChopState (favoritesSong); p.setFavoritesViewEnabled (true);
        p.setMidiPerformanceMode (P::MidiPerformanceMode::LegacyFavorites);
        check (p.getMidiChannelForChopId (1) == 2 && p.getMidiNoteForChopId (1) == 91
               && p.getMidiChannelForChopId (220) == 1 && p.getMidiNoteForChopId (220) == 0,
               "Favorites pack across all banks in favorite order");
        p.setKeyboardMidiBank (1); p.setMidiOctaveOffset (0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0); render();
        check (p.getLastTriggeredChopId() == 148, "FL C6 matches packed Favorites C6 regardless of the viewport");
        midi.addEvent (juce::MidiMessage::noteOff (1, 72), 0); render();
        p.setFavoritesViewEnabled (false);
        p.setMidiPerformanceMode (P::MidiPerformanceMode::Legacy);
        auto shortSong = std::make_shared<P::ChopState> (*song); shortSong->chops.resize (12);
        p.publishChopState (shortSong);
        check (p.getKeyboardMidiBank() == 1, "reducing chops clamps an obsolete selected bank");
        auto overflow = std::make_shared<P::ChopState>(); overflow->chops.resize (2049);
        P::rebuildChopMidiMap (*overflow, 0, false, true);
        check (overflow->midiMap.channelForChopIndex[2047] == 16 && overflow->midiMap.noteForChopIndex[2047] == 127
               && overflow->midiMap.noteForChopIndex[2048] == -1,
               "16-bank boundary is checked without overflowing MIDI channels or notes");
        auto sparseSong = std::make_shared<P::ChopState> (*thresholdLayout);
        for (int i = 128; i < 256; ++i) sparseSong->chops[(size_t) i].assignedMidiNote = -2;
        p.publishChopState (sparseSong); p.setKeyboardMidiBank (1); p.setMidiOctaveOffset (9);
        plus->onClick();
        check (p.getKeyboardMidiBank() == 1 && p.getKeyboardStartNote() == 120,
               "C10 limit never changes ranges even when another range is empty");
        p.setKeyboardMidiBank (3); p.setMidiOctaveOffset (0);
        check (waitFor ([&] { return (int) pads->getProperties()["padCount"] == 257; })
               && p.getKeyboardMidiBank() == 3 && p.getKeyboardStartNote() == 0,
               "restored channel retains every pad including chops in an empty middle MIDI range");
        auto bankPins = std::make_shared<P::ChopState> (*song); bankPins->chops[0].assignedMidiNote = 60;
        P::rebuildChopMidiMap (*bankPins, 0, false, true);
        check (bankPins->midiMap.channelNoteToChopIndex[0][60] == 0
               && bankPins->midiMap.noteForChopIndex[60] == -1
               && bankPins->midiMap.channelNoteToChopIndex[1][60] == 188,
               "explicit pins keep channel 1 and do not collide with later banks");
        // The laptop source's output is exercised without a real MIDI device
        // or injected operating-system keys. Feed its events through the host.
        p.publishChopState (song);
        std::vector<juce::MidiMessage> events;
        {
            cuesampler::LaptopMidiInput input ([&] (const auto& message) { events.push_back (message); });
            input.press ('z', 0, 2); input.press ('z', 0, 2);
            check (events.size() == 1 && events[0].getChannel() == 2 && events[0].getNoteNumber() == 0,
                   "laptop key emits bank channel once and ignores OS repeat");
            midi.addEvent (events[0], 0); render();
            check (p.getLastTriggeredChopId() == 129, "laptop virtual-MIDI event plays range-2 C0 through the host MIDI path");
            input.releaseAll();
            check (events.back().isNoteOff() && events.back().getChannel() == 2 && events.back().getNoteNumber() == 0,
                   "bank/focus change sends the original channel/note off");
            const auto releasedCount = events.size();
            input.press ('z', 48, 3);
            check (events.size() == releasedCount, "held-key repeat cannot start a new-bank note before key-up");
            input.releaseMissing ([] (int) { return false; });
            events.clear(); input.press ('Q', 48, 3); input.press (',', 48, 3);
            input.releaseMissing ([] (int key) { return key == ','; });
            check (events.size() == 1, "overlapping laptop rows do not double-trigger or prematurely release a note");
            input.releaseAll(); check (events.size() == 2 && events.back().isNoteOff(), "last overlapping key releases its note");
            events.clear(); input.press ('Z', 120, 1); input.press ('B', 120, 1); input.press (']', 120, 1);
            check (events.size() == 2 && events[1].getNoteNumber() == 127,
                   "upper laptop keys cannot emit notes past MIDI 127");
            input.releaseAll(); events.clear();
            check (! input.press (' ', 48, 1) && events.empty(), "transport keys are not stolen by laptop input");
            input.releaseMissing ([] (int) { return false; });
            input.press ('Z', 0, 1);
            check (! events.empty() && events.back().isNoteOn() && events.back().getNoteNumber() == 0,
                   "laptop input emits absolute C0 MIDI to its sink");
            input.releaseAll();
            input.press ('B', 84, 2);
            check (events.back().getChannel() == 2 && events.back().getNoteNumber() == 91,
                   "range-2 C7 plus B reaches chop 220's G7 note");
        }
        check (events.back().isNoteOff(), "closing laptop input releases its held keys");

        // Render the actual waveform and active pad surfaces for visual QA.
        if (theme != nullptr && theme->onClick)
        {
            const auto previousTheme = theme->getButtonText();
            auto visualChops = std::make_shared<P::ChopState> (*layout);
            visualChops->chops.resize (12);
            visualChops->selectedChopId = 2;
            visualChops->chops[2].favorite = true;
            visualChops->chops[3].favorite = true;
            p.publishChopState (visualChops);
            p.setKeyboardMidiBank (1); p.setMidiOctaveOffset (0);
            auto analysis = std::make_shared<P::TempoAnalysisData>();
            analysis->estimatedBpm = 120; analysis->beatPeriodSeconds = 0.5;
            std::atomic_store (&p.tempoAnalysis, analysis);
            p.setWaveformZoom (0.0f); p.setWaveformScroll (0.0f);
            p.sampleChangeBroadcaster.sendChangeMessage();
            p.notifyEditStateChanged();
            waitFor ([&] { return (int) pads->getProperties()["padCount"] == 12; });
            p.keyboardState.noteOn (1, 3, 1.0f);
            p.keyboardState.noteOn (1, 4, 1.0f);
            for (const auto* themeName : { "dark", "light" })
            {
                if (theme->getButtonText() != juce::String (themeName).toUpperCase()) theme->onClick();
                for (int i = 0; i < 30; ++i) pump();
                juce::FileOutputStream image (scratch.getParentDirectory().getChildFile (
                    "grid-pads-active-" + juce::String (themeName) + ".png"));
                image.setPosition (0); image.truncate();
                juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), image);
            }
            p.keyboardState.noteOff (1, 3, 0.0f);
            p.keyboardState.noteOff (1, 4, 0.0f);
            if (theme->getButtonText() != previousTheme) theme->onClick();
        }
    }

    static void runFavorites()
    {
        P p;
        setup (p);
        auto layout = std::make_shared<P::ChopState>();
        for (int i = 0; i < 4; ++i)
        {
            P::ChopDefinition c;
            c.id = i + 1; c.startSample = i * 16000; c.endSample = (i + 1) * 16000;
            c.cueOffsetSamples = 17; c.gainDecibels = -2; c.pitchSemitones = 1;
            c.assignedMidiNote = i == 0 ? 60 : i == 1 ? 72 : i == 2 ? -2 : -1;
            layout->chops.push_back (c);
        }
        layout->nextChopId = 5;
        p.publishChopState (layout);
        p.setMidiOctaveOffset (1);
        const auto originalMap = p.getChopState()->midiMap.noteToChopIndex;
        const auto source = p.getLoadedSample();
        for (int id : { 3, 1, 4 }) { p.selectChopById (id); p.toggleSelectedChopFavorite(); }
        check (p.getChopState()->favoriteChopIndices == std::vector<int> { 2, 0, 3 },
               "favorites retain click order rather than source order");
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* button = findButton (*editor, "FAVORITES");
        check (button != nullptr, "Favorites button is present");
        if (button == nullptr) return;
        button->onClick();
        check (p.isFavoritesViewEnabled() && button->getToggleState(), "Favorites button enters temporary performance view");
        p.setMidiPerformanceMode (P::MidiPerformanceMode::LegacyFavorites);
        check (p.getMidiNoteForChopId (3) == 48 && p.getMidiNoteForChopId (1) == 49
               && p.getMidiNoteForChopId (4) == 50 && p.getMidiNoteForChopId (2) == -1,
               "only favorites map consecutively from C4, overriding pins temporarily");
        p.setMidiOctaveOffset (-1);
        check (p.getMidiOctaveOffset() == 0 && p.getMidiRootNote() == 48,
               "Favorites keeps fixed C4 notes while OCT navigates the keyboard");
        bool intact = p.getLoadedSample() == source && p.getChopState()->chops.size() == 4;
        for (int i = 0; i < 4; ++i)
        {
            const auto& c = p.getChopState()->chops[(size_t) i];
            intact = intact && c.startSample == i * 16000 && c.endSample == (i + 1) * 16000
                   && c.cueOffsetSamples == 17 && c.assignedMidiNote == layout->chops[(size_t) i].assignedMidiNote
                   && c.gainDecibels == -2 && c.pitchSemitones == 1;
        }
        check (intact, "Favorites preserves audio, bounds, processing and original MIDI assignments");
        p.setChopPlaybackMode (P::ChopPlaybackMode::Gate);
        p.prepareToPlay (8000, 64);
        juce::AudioBuffer<float> output (2, 64);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
        p.processBlock (output, midi);
        check (p.getLastTriggeredChopId() == 3, "real MIDI C4 triggers the first favorited chop");
        const auto revision = p.getChopTriggerRevision();
        midi.clear(); midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0);
        p.processBlock (output, midi);
        check (p.getChopTriggerRevision() == revision, "nonfavorite original MIDI assignment is inactive in Favorites");
        p.selectChopById (3); p.toggleSelectedChopFavorite();
        check (p.getMidiNoteForChopId (1) == 48 && p.getMidiNoteForChopId (4) == 49,
               "removing a favorite closes the MIDI gap");
        midi.clear(); p.processBlock (output, midi);
        check (p.getPlayingChopId() == 3 && p.heldMidiNote.load() == 48,
               "legacy Favorites remapping preserves the sounding chop and its original held note");
        midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0); p.processBlock (output, midi);
        midi.clear(); for (int block = 0; block < 10; ++block) p.processBlock (output, midi);
        check (! p.isPlaying() && p.heldMidiNote.load() == -1,
               "the original note-off releases a Gate after legacy Favorites remapping");
        p.undoLastEdit();
        check (p.getChopState()->favoriteChopIndices == std::vector<int> { 2, 0, 3 }
               && p.getMidiNoteForChopId (3) == 48, "Undo restores favorite order and compact MIDI mapping");
        p.selectChopById (3); p.toggleSelectedChopFavorite();
        p.setFavoritesViewEnabled (false);
        p.setMidiPerformanceMode (P::MidiPerformanceMode::Legacy);
        p.selectChopById (3); p.toggleSelectedChopFavorite();
        p.setFavoritesViewEnabled (true);
        p.setMidiPerformanceMode (P::MidiPerformanceMode::LegacyFavorites);
        check (p.getChopState()->favoriteChopIndices == std::vector<int> { 0, 3, 2 },
               "re-favoriting appends a chop to the end of the lineup");
        juce::MemoryBlock saved; p.getStateInformation (saved);
        P reopened;
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (waitFor ([&] { return restored (reopened); }) && reopened.isFavoritesViewEnabled()
               && reopened.getChopState()->favoriteChopIndices == std::vector<int> { 0, 3, 2 }
               && reopened.getMidiNoteForChopId (1) == 48,
               "project recall restores Favorites view, chronology and MIDI map");
        std::unique_ptr<juce::AudioProcessorEditor> reopenedEditor (reopened.createEditor());
        auto* restoredButton = findButton (*reopenedEditor, "FAVORITES");
        check (restoredButton != nullptr && restoredButton->getToggleState(), "reopened editor shows Favorites enabled");
        reopened.setFavoritesViewEnabled (false);
        reopened.setMidiPerformanceMode (P::MidiPerformanceMode::Legacy);
        check (reopened.getChopState()->midiMap.noteToChopIndex == originalMap && reopened.getMidiRootNote() == 48,
               "leaving Favorites after recall restores exact original mapping and root");
        for (int id : { 1, 3, 4 }) { p.selectChopById (id); p.toggleSelectedChopFavorite(); }
        check (p.getChopState()->favoriteChopIndices.empty() && p.getMappedMidiNotes().none()
               && p.getChopState()->selectedChopId == 4,
               "empty Favorites has no phantom MIDI assignments and retains the actual selected chop");
        p.setFavoritesViewEnabled (false);
        p.setMidiPerformanceMode (P::MidiPerformanceMode::Legacy);
        check (p.getChopState()->midiMap.noteToChopIndex == originalMap, "empty Favorites can return to full original layout");

        juce::MemoryInputStream input (saved.getData(), saved.getSize(), false); input.skipNextBytes (4);
        auto legacy = juce::ValueTree::readFromStream (input);
        legacy.removeProperty ("favoritesViewEnabled", nullptr);
        for (const auto* layerName : { "ChopState", "StashedChopState" })
        {
            auto layer = legacy.getChildWithName (layerName);
            for (const auto* property : { "midiPerformanceMode", "midiMappingLocked", "midiWindowStart", "midiWindowInitialized",
                                          "favoriteMidiSlotsInitialized", "favoriteMidiHighWater" })
                layer.removeProperty (property, nullptr);
            layer.removeChild (layer.getChildWithName ("PerformanceWindowSlots"), nullptr);
            layer.removeChild (layer.getChildWithName ("PerformanceFavoriteSlots"), nullptr);
        }
        for (auto child : legacy.getChildWithName ("ChopState")) child.removeProperty ("favoriteOrder", nullptr);
        juce::MemoryBlock oldData;
        { juce::MemoryOutputStream out (oldData, false); out.write ("CSB2", 4); legacy.writeToStream (out); }
        reopened.setStateInformation (oldData.getData(), (int) oldData.getSize());
        check (waitFor ([&] { return restored (reopened); }) && ! reopened.isFavoritesViewEnabled()
               && reopened.getChopState()->favoriteChopIndices == std::vector<int> { 0, 2, 3 },
               "legacy favorites without chronology use deterministic sample order");
    }

    static void runDoubleTempo (const juce::File& scratch)
    {
        P p;
        setup (p);
        auto analysis = std::make_shared<P::TempoAnalysisData>();
        analysis->estimatedBpm = 60;
        analysis->beatPeriodSeconds = 1;
        analysis->analysisEndSeconds = 8;
        std::atomic_store (&p.tempoAnalysis, analysis);
        p.chopBarsCount.store (1);
        p.publishChopState (std::make_shared<P::ChopState>());
        p.buildChopsFromAnalysis (*analysis);
        p.hostSampleRate.store (8000);
        p.hostBpm.store (120);
        p.setSyncToHost (true);
        const auto originalChops = p.getChopState();
        const auto originalAudio = p.getLoadedSample();
        check (! p.getDoubleTempoEnabled() && originalChops->chops.size() == 2,
               "60 BPM starts with two four-second bars");
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* button = findButton (*editor, "2x");
        check (button != nullptr && button->isEnabled(), "2x button is available for analyzed audio");
        if (button == nullptr) return;
        button->onClick();
        check (button->getToggleState() && p.getDoubleTempoEnabled()
               && std::abs (p.getAdjustedAnalysisBpm (*analysis) - 120) < 1.0e-9,
               "2x button changes detected 60 BPM to 120 BPM");
        check (p.getChopState()->chops.size() == 4
               && p.getChopState()->chops[0].endSample == 16000,
               "2x rebuilds the automatic chop grid to two-second bars");
        check (std::abs (p.timeStretchRatio.load() - 1.0f) < 1.0e-6f
               && std::abs (p.getCurrentGridFingerprint() - 0.5) < 1.0e-9,
               "2x uses corrected tempo for 120 BPM host sync and grid fingerprint");
        p.setWarpDivision (P::WarpDivision_Beat);
        check (std::abs (p.getWarpDivisionSeconds() - 0.5) < 1.0e-9,
               "warp snapping follows doubled tempo");
        check (p.getLoadedSample() == originalAudio && p.getGridBpmTrim() == 0,
               "tempo correction preserves source audio and fine trim");
        const auto exported = readExport (p.renderChopToTempWav (p.getChopState()->chops[0].id, true, scratch), 8000, 16000);
        check (exported.getNumSamples() == 16000, "120 BPM sync export retains a two-second corrected bar");

        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        P reopened;
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (waitFor ([&] { return restored (reopened); }) && reopened.getDoubleTempoEnabled() && reopened.getTempoAnalysis() != nullptr
               && std::abs (reopened.getAdjustedAnalysisBpm (*reopened.getTempoAnalysis()) - 120) < 1.0e-9
               && reopened.getChopState()->chops.size() == 4,
               "2x and corrected chop grid survive portable project recall");
        std::unique_ptr<juce::AudioProcessorEditor> reopenedEditor (reopened.createEditor());
        auto* recalledButton = findButton (*reopenedEditor, "2x");
        check (recalledButton != nullptr && recalledButton->getToggleState(), "reopened editor shows saved 2x state");

        p.undoLastEdit();
        const auto undone = p.getChopState();
        bool restoredLayout = undone->chops.size() == originalChops->chops.size()
                           && undone->selectedChopId == originalChops->selectedChopId
                           && undone->midiMap.noteForChopIndex == originalChops->midiMap.noteForChopIndex;
        for (size_t i = 0; restoredLayout && i < undone->chops.size(); ++i)
            restoredLayout = undone->chops[i].id == originalChops->chops[i].id
                          && undone->chops[i].startSample == originalChops->chops[i].startSample
                          && undone->chops[i].endSample == originalChops->chops[i].endSample;
        check (! p.getDoubleTempoEnabled() && restoredLayout
               && std::abs (p.timeStretchRatio.load() - 0.5f) < 1.0e-6f,
               "undo restores original tempo, chop layout and host sync");
        button->onClick();
        button->onClick();
        check (! p.getDoubleTempoEnabled() && ! button->getToggleState()
               && std::abs (p.getAdjustedAnalysisBpm (*analysis) - 60) < 1.0e-9,
               "second click switches 2x off without cumulative doubling");
        p.setGridBpmTrim (0.5f);
        p.setDoubleTempoEnabled (true);
        check (std::abs (p.getAdjustedAnalysisBpm (*analysis) - 120.5) < 1.0e-9,
               "fine trim stays in BPM units after doubling");
        p.setManualChopModeActive (true);
        const auto manualChops = p.getChopState();
        p.setDoubleTempoEnabled (false);
        check (p.getChopState() == manualChops, "2x preserves manually placed chop boundaries");

        juce::MemoryInputStream input (saved.getData(), saved.getSize(), false);
        input.skipNextBytes (4);
        auto legacy = juce::ValueTree::readFromStream (input);
        legacy.removeProperty ("doubleTempoEnabled", nullptr);
        juce::MemoryBlock legacyData;
        { juce::MemoryOutputStream output (legacyData, false); output.write ("CSB2", 4); legacy.writeToStream (output); }
        reopened.setStateInformation (legacyData.getData(), (int) legacyData.getSize());
        check (waitFor ([&] { return restored (reopened); }) && ! reopened.getDoubleTempoEnabled(),
               "older projects default to normal tempo");

        P empty;
        std::unique_ptr<juce::AudioProcessorEditor> emptyEditor (empty.createEditor());
        const auto* emptyButton = findButton (*emptyEditor, "2x");
        check (emptyButton != nullptr && ! emptyButton->isEnabled(), "2x is disabled without analyzed audio");
        empty.setDoubleTempoEnabled (true);
        check (! empty.getDoubleTempoEnabled(), "tempo correction cannot apply without analysis");
    }

    static void setupManualFixture (P& p)
    {
        auto sample = std::make_shared<P::LoadedSampleData>();
        sample->sampleRate = 8000;
        sample->fileName = "manual chop regression";
        sample->buffer.setSize (1, 64000);
        sample->buffer.clear();
        sample->buffer.setSample (0, 16, 1.0f);
        juce::MemoryBlock data;
        check (p.serializeSampleToStateData (*sample, data), "encode manual fixture");
        sample->serializedStateData = juce::var (data);
        std::atomic_store (&p.loadedSample, sample);
        auto analysis = std::make_shared<P::TempoAnalysisData>();
        analysis->estimatedBpm = 120;
        analysis->beatPeriodSeconds = 0.5;
        std::atomic_store (&p.tempoAnalysis, analysis);
        p.restoredStateReceived.store (true);
        p.waveformZoom.store (0);
        p.waveformScroll.store (0);
        p.prepareToPlay (8000, 128);
        p.setManualChopModeActive (true);
    }

    static void manualBlock (P& p, const juce::MidiMessage& message, int offset = 23, int frames = 128)
    {
        juce::AudioBuffer<float> output (2, frames);
        juce::MidiBuffer midi;
        midi.addEvent (message, offset);
        p.processBlock (output, midi);
    }

    static void beginManualCapture (P& p)
    {
        p.armManualChopCapture (1000);
        manualBlock (p, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 11);
    }

    static void runManualChops (const juce::File& scratch)
    {
        {
            P p; setupManualFixture (p);
            check (p.getNextManualChopMidiNote() == 24, "new manual layout starts at C2");
            const int first = p.addManualChop (1000, 2000, p.getNextManualChopMidiNote());
            check (p.getMidiNoteForChopId (first) == 24 && p.getNextManualChopMidiNote() == 25,
                   "manual chops without a held key receive consecutive notes from C2");
            P legacy; setupManualFixture (legacy); legacy.recordedMidiRoot.store (48);
            check (legacy.getNextManualChopMidiNote() == 48,
                   "existing C4 manual layouts keep their saved allocation root");
        }
        {
            P p; setupManualFixture (p); beginManualCapture (p);
            manualBlock (p, juce::MidiMessage::noteOff (1, 60));
            check (p.getManualChopCaptureEndSample() == 1140, "capture endpoint respects MIDI timestamps");
            juce::MemoryBlock saved; p.getStateInformation (saved);
            const auto tree = juce::ValueTree::readFromData ((const char*) saved.getData() + 4, saved.getSize() - 4);
            int capturedChops = 0;
            for (auto child : tree.getChildWithName ("ChopState")) if (child.hasType ("Chop")) ++capturedChops;
            check (capturedChops == 1,
                   "immediate save retains completed capture without an editor timer");
            p.commitPendingManualChopCaptures();
            check (p.getChopState()->chops.size() == 1 && p.getChopState()->chops[0].startSample == 1000
                   && p.getChopState()->chops[0].endSample == 1140, "completed capture commits exactly once");
            beginManualCapture (p);
            manualBlock (p, juce::MidiMessage::noteOff (1, 60));
            p.undoLastEdit();
            check (p.getChopState()->chops.size() == 1 && p.getChopState()->chops[0].assignedMidiNote == 60,
                   "immediate Undo consumes and undoes the latest completed capture");
        }
        {
            P p; setupManualFixture (p); beginManualCapture (p);
            manualBlock (p, juce::MidiMessage::noteOff (2, 60));
            check (p.getManualChopCaptureHeldNote() == 60, "another MIDI channel cannot finish capture");
            manualBlock (p, juce::MidiMessage::noteOff (1, 60), 0, 0);
            p.commitPendingManualChopCaptures();
            check (p.getChopState()->chops.size() == 1 && p.getChopState()->chops[0].endSample == 1245,
                   "zero-frame note-off commits the held capture");
        }
        for (int action = 0; action < 6; ++action)
        {
            P p; setupManualFixture (p); beginManualCapture (p);
            if (action == 0) p.stopPlayback();
            if (action == 1) p.pausePlayback();
            if (action == 2) manualBlock (p, juce::MidiMessage::allNotesOff (1));
            if (action == 3) manualBlock (p, juce::MidiMessage::allSoundOff (1));
            if (action == 4) p.prepareToPlay (8000, 128);
            if (action == 5) p.releaseResources();
            manualBlock (p, juce::MidiMessage::noteOff (1, 60));
            p.commitPendingManualChopCaptures();
            check (p.getChopState()->chops.empty() && p.getManualChopCaptureHeldNote() < 0 && ! p.isPlaying(),
                   "transport, panic and audio-device resets cancel capture without creating a backwards chop");
        }
        {
            P p; setupManualFixture (p);
            const auto oldSample = std::atomic_load (&p.loadedSample);
            std::atomic_store (&p.loadedSample, std::make_shared<P::LoadedSampleData> (*oldSample));
            p.armManualChopCapture (1000);
            p.handleMidiEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0,
                               oldSample, std::atomic_load (&p.chopState), 8000, false, false, 0);
            check (p.getManualChopCaptureHeldNote() < 0, "an old audio-block snapshot cannot begin capture for a replacement source");
        }
        {
            P p; setupManualFixture (p);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            beginManualCapture (p);
            manualBlock (p, juce::MidiMessage::noteOff (1, 60));
            editor.reset();
            check (waitFor ([&] { return p.getChopState()->chops.size() == 1; }),
                   "processor commits released capture after editor closure");
        }
        {
            P p; setupManualFixture (p); beginManualCapture (p);
            manualBlock (p, juce::MidiMessage::noteOff (1, 60));
            p.clearAllChops();
            p.commitPendingManualChopCaptures();
            check (p.getChopState()->chops.empty(), "clear removes completed captures still awaiting publication");
            p.undoLastEdit();
            check (p.getChopState()->chops.size() == 1, "undo clear restores the completed capture");
        }
        {
            P p; setupManualFixture (p);
            const int id = p.addManualChop (1000, 2000, 60);
            p.setChopBounds (id, 1100, 1900);
            p.setManualChopModeActive (false);
            p.setChopBarsCount (2);
            p.setManualChopModeActive (true);
            check (p.canUndoEdit(), "switching layers retains manual undo history");
            p.undoLastEdit();
            check (p.getChopState()->chops.size() == 1 && p.getChopState()->chops[0].startSample == 1000
                   && p.getChopBarsCount() == 2, "manual undo restores only its chop edit, retaining current grid settings");
            beginManualCapture (p);
            manualBlock (p, juce::MidiMessage::noteOff (1, 60));
            // A replacement sample must invalidate both layers and any completion
            // still waiting in the audio-to-message-thread queue.
            const auto sampleFile = scratch.getChildFile ("replacement.wav");
            auto sample = std::make_shared<P::LoadedSampleData>();
            sample->sampleRate = 8000; sample->buffer.setSize (1, 8000); sample->buffer.clear();
            {
                // State audio uses a private float container. Import needs a
                // real audio file rather than that container with a WAV suffix.
                auto output = sampleFile.createOutputStream();
                auto writer = std::unique_ptr<juce::AudioFormatWriter> (juce::WavAudioFormat().createWriterFor (
                    output.get(), sample->sampleRate, 1, 24, {}, 0));
                check (writer != nullptr, "create replacement WAV fixture");
                if (writer == nullptr) return;
                output.release();
                check (writer->writeFromAudioSampleBuffer (sample->buffer, 0, sample->buffer.getNumSamples()),
                       "write replacement WAV fixture");
            }
            check (p.loadAudioFile (sampleFile) == P::SampleLoadResult::loaded, "replacement sample loads");
            p.commitPendingManualChopCaptures();
            check (p.getChopState()->chops.empty() && ! p.canUndoEdit(), "replacement clears active chops, history and stale capture");
            p.setManualChopModeActive (false);
            check (p.getChopState()->chops.empty() && ! p.canUndoEdit(), "replacement also clears inactive chops and history");
        }

        const auto findWave = [] (juce::Component& root)
        {
            std::function<juce::Component* (juce::Component&)> find = [&] (juce::Component& c) -> juce::Component*
            {
                if (juce::String (typeid (c).name()).contains ("WaveformDisplayComponent")) return &c;
                for (auto* child : c.getChildren()) if (auto* found = find (*child)) return found;
                return nullptr;
            };
            return find (root);
        };
        {
            P p; setupManualFixture (p);
            const int id = p.addManualChop (16000, 32000, 60);
            juce::MemoryBlock manualState; p.getStateInformation (manualState);
            p.setManualChopModeActive (false);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            p.setStateInformation (manualState.getData(), (int) manualState.getSize());
            auto* add = findButton (*editor, "ADD CHOP");
            auto* edit = findButton (*editor, "EDIT");
            check (waitFor ([&] { return restored (p) && add != nullptr && add->isVisible(); }),
                   "open-editor restore synchronizes waveform manual tools");
            auto* wave = findWave (*editor);
            if (wave == nullptr || add == nullptr || edit == nullptr) { check (false, "manual UI controls exist"); return; }
            const float width = (float) wave->getWidth() - 58.0f;
            const auto xFor = [width] (int sample) { return 27.0f + width * (float) sample / 64000.0f; };
            const auto event = [wave] (float x, float y = 150.0f)
            {
                const juce::Point<float> point (x, y);
                return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
                    juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
                    wave, wave, juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(), 1, false);
            };
            const auto click = [&] (int sample)
            {
                auto e = event (xFor (sample)); wave->mouseDown (e); wave->mouseUp (e);
            };
            // Force the saved zoom to finish synchronizing while hidden.
            editor->setVisible (false);
            for (int i = 0; i < 10; ++i) pump();
            edit->onClick();
            auto press = event (xFor (16000) + 3.0f);
            wave->mouseDown (press); wave->mouseUp (press);
            check (p.getChopState()->chops[0].startSample == 16000, "pressing an edge without dragging does not move it");
            p.setChopBounds (id, 16000, 16080);
            const auto shortPress = event (xFor (16080) + 3.0f);
            wave->mouseDown (shortPress);
            wave->mouseDrag (event (shortPress.position.x + 3.1f));
            wave->mouseUp (shortPress);
            const auto shortChop = p.getChopState()->chops[0];
            check (shortChop.endSample - shortChop.startSample < 400,
                   "trimming a short chop does not impose a 50ms minimum");
            add->onClick(); click (40000); click (48000);
            const auto chops = p.getChopState();
            check (chops->chops.size() == 2 && chops->chops[1].startSample == 40000 && chops->chops[1].endSample == 48000,
                   "two single clicks create an independent chop");
            check (edit->getToggleState(), "finishing a chop returns to audition/edit");
            p.setChopPlaybackMode (P::ChopPlaybackMode::OneShot);
            click (44000);
            manualBlock (p, juce::MidiMessage::controllerEvent (1, 1, 0));
            check (p.voice.playbackTriggeredByMouse && p.voice.playbackActive, "clicking a completed manual chop auditions it");
            add->onClick(); click (52000);
            check (wave->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)), "Escape cancels pending manual placement");
            beginManualCapture (p); p.cancelManualChopCapture();
            check (p.getChopState()->chops.size() == 2, "cancelled placement does not create a chop");
            auto* warp = findButton (*editor, "WARP");
            check (warp != nullptr, "warp control exists");
            if (warp != nullptr)
            {
                const auto before = p.getChopState();
                warp->setToggleState (true, juce::dontSendNotification); warp->onClick();
                check (p.isManualChopModeActive() && p.isWarpModeActive() && p.getChopState() == before,
                       "WARP edits the current manual layout instead of switching layers");
            }
        }
        {
            P p; setupManualFixture (p); p.waveformZoom.store (1.0f);
            p.addManualChop (16, 24, 60);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto* wave = findWave (*editor);
            if (wave == nullptr) { check (false, "precision waveform exists"); return; }
            wave->resized();
            const auto image = wave->createComponentSnapshot (wave->getLocalBounds());
            const int markerX = 27 + (wave->getWidth() - 58) / 2;
            const int y = wave->getHeight() / 2 - 25;
            check (image.getPixelAt (markerX, y).getRed() > 140 && image.getPixelAt (43, y).getRed() < 100,
                   "deep-zoom impulse aligns with its sample-16 chop marker");
            juce::FileOutputStream output (scratch.getSiblingFile ("manual-chop-precision.png"));
            output.setPosition (0); output.truncate();
            juce::PNGImageFormat().writeImageToStream (image, output);
            juce::FileOutputStream editorOutput (scratch.getSiblingFile ("manual-chop-editor.png"));
            editorOutput.setPosition (0); editorOutput.truncate();
            juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), editorOutput);
        }
    }

    static void runWaveformColour (const juce::File& scratch)
    {
        using Analysis = cuesampler::WaveformColourAnalysis;
        for (const int rate : { 8000, 44100, 48000, 96000 })
        {
            const std::array<double, 3> frequencies { 80.0, 1000.0, juce::jmin (10000.0, rate * 0.46) };
            for (size_t band = 0; band < frequencies.size(); ++band)
            {
                juce::AudioBuffer<float> tone (2, rate / 2);
                for (int i = 0; i < tone.getNumSamples(); ++i)
                {
                    const auto x = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * frequencies[band] * i / rate));
                    tone.setSample (0, i, x); tone.setSample (1, i, -x);
                }
                const auto data = Analysis::analyse (tone, rate);
                const auto energy = data->bands (rate / 4, rate / 3);
                check (energy[band] > 2.0 * energy[(band + 1) % 3] && energy[band] > 2.0 * energy[(band + 2) % 3],
                       "known tones map to low/mid/high bands across sample rates, including inverted stereo");
            }
        }
        juce::AudioBuffer<float> audio (1, 48000); audio.clear();
        auto data = Analysis::analyse (audio, 48000);
        check (data->attacks.empty() && data->bands (0, 48000) == std::array<double, 3> {}, "silence has no false colours or transients");
        for (const int sample : { 2400, 12000, 36000 }) audio.setSample (0, sample, 1.0f);
        data = Analysis::analyse (audio, 48000);
        check (data->attacks == std::vector<int> { 2400, 12000, 36000 }, "isolated attacks retain their source-sample positions");
        int polls = 0;
        check (Analysis::analyse (audio, 48000, [&] { return ++polls > 2; }) == nullptr,
               "obsolete analysis cancels within a short analysis frame");
        for (int i = 0; i < audio.getNumSamples(); ++i) audio.setSample (0, i, 0.5f);
        check (Analysis::analyse (audio, 48000)->attacks.size() == 1, "sustained energy does not generate repeated attack markers");

        P p; setupManualFixture (p);
        check (p.getWaveformColourMode() == 0, "colour display is optional and defaults to Classic");
        p.setWaveformColourMode (3);
        juce::MemoryBlock saved; p.getStateInformation (saved);
        P reopened; reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (waitFor ([&] { return restored (reopened); }) && reopened.getWaveformColourMode() == 3,
               "waveform colour preference survives project restore");
        juce::MemoryInputStream input (saved.getData(), saved.getSize(), false); input.skipNextBytes (4);
        auto legacy = juce::ValueTree::readFromStream (input); legacy.removeProperty ("waveformColourMode", nullptr);
        juce::MemoryBlock legacyData;
        { juce::MemoryOutputStream out (legacyData, false); out.write ("CSB2", 4); legacy.writeToStream (out); }
        reopened.setStateInformation (legacyData.getData(), (int) legacyData.getSize());
        check (waitFor ([&] { return restored (reopened); }) && reopened.getWaveformColourMode() == 0,
               "older projects retain the Classic waveform");

        auto source = std::make_shared<P::LoadedSampleData>();
        source->sampleRate = 48000; source->fileName = "Waveform colour: low / mid / high / attacks";
        source->buffer.setSize (2, 192000); source->buffer.clear();
        for (int region = 0; region < 3; ++region)
            for (int i = 0; i < 38400; ++i)
            {
                const double envelope = juce::jmin (1.0, i / 400.0) * juce::jmin (1.0, (38400 - i) / 2000.0);
                const double hz = region == 0 ? 80.0 : region == 1 ? 1000.0 : 10000.0;
                const float x = (float) (0.72 * envelope * std::sin (juce::MathConstants<double>::twoPi * hz * i / 48000));
                source->buffer.setSample (0, region * 48000 + i, x);
                source->buffer.setSample (1, region * 48000 + i, -x);
            }
        for (const int begin : { 148800, 163200, 177600 })
            for (int i = 0; i < 5000; ++i)
            {
                const float x = (float) (std::exp (-i / 700.0) * std::cos (i * 0.8));
                source->buffer.setSample (0, begin + i, x); source->buffer.setSample (1, begin + i, -x);
            }
        std::atomic_store (&p.loadedSample, source);
        p.setWaveformColourMode (3);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* mode = findButton (*editor, "WAVE: BOTH");
        check (mode != nullptr, "waveform display selector reflects the saved preference");
        juce::Component* wave = mode != nullptr ? mode->getParentComponent() : nullptr;
        if (wave == nullptr) return;
        const auto sampleColour = [&] (int sample)
        {
            const auto image = wave->createComponentSnapshot (wave->getLocalBounds());
            const int x = 27 + (wave->getWidth() - 58) * sample / source->buffer.getNumSamples();
            return image.getPixelAt (x, wave->getHeight() / 2 - 30);
        };
        check (waitFor ([&]
        {
            const auto low = sampleColour (24000), mid = sampleColour (72000), high = sampleColour (120000);
            return low.getRed() > low.getBlue() * 1.5 && mid.getGreen() > mid.getRed() * 1.5 && high.getBlue() > high.getRed() * 1.5;
        }), "background analysis paints distinct frequency colours without stereo cancellation");
        {
            juce::FileOutputStream out (scratch.getSiblingFile ("waveform-colour-both.png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), out);
        }
        p.setWaveformColourMode (0);
        check (waitFor ([&] { return findButton (*editor, "WAVE: CLASSIC") != nullptr; }), "Classic switch updates an open editor");
        const auto classic = sampleColour (24000);
        check (classic.getRed() < classic.getBlue() * 1.5, "Classic removes the frequency colour overlay");
        p.setWaveformColourMode (3);
        check (waitFor ([&] { return findButton (*editor, "WAVE: BOTH") != nullptr; }), "colour mode can reuse completed analysis");
        auto warpedLayout = std::make_shared<P::ChopState>();
        P::ChopDefinition warpedChop;
        warpedChop.id = 1; warpedChop.startSample = 0; warpedChop.endSample = 144000;
        warpedChop.warpMarkers.push_back ({ 48000, 1.5, false, 0.0 });
        warpedLayout->chops.push_back (warpedChop); warpedLayout->nextChopId = 2;
        p.publishChopState (warpedLayout);
        const auto warpedColour = sampleColour (48000);
        check (warpedColour.getRed() > warpedColour.getGreen() * 1.5,
               "warped waveform colours follow the source-to-playback time map");
        auto movedWarp = std::make_shared<P::ChopState> (*warpedLayout);
        movedWarp->chops[0].warpMarkers[0].localTimeSeconds = 0.5;
        p.publishChopState (movedWarp);
        const auto movedColour = sampleColour (48000);
        check (movedColour.getGreen() > movedColour.getRed() * 1.5,
               "moving a warp anchor invalidates cached waveform pixels immediately");
        auto cueEdit = std::make_shared<P::ChopState> (*movedWarp);
        cueEdit->chops[0].setCueNormalized (0.1);
        p.publishChopState (cueEdit);
        check (sampleColour (48000) == movedColour, "cue edits preserve the cached source waveform");
        auto removedWarp = std::make_shared<P::ChopState> (*warpedLayout);
        removedWarp->chops[0].warpMarkers.clear();
        p.publishChopState (removedWarp);
        const auto unwarpedColour = sampleColour (48000);
        check (unwarpedColour.getGreen() > unwarpedColour.getRed() * 1.5,
               "removing warp anchors reveals the original waveform without stale cache pixels");
        p.publishChopState (warpedLayout);
        check (sampleColour (48000) == warpedColour, "restoring warp anchors restores their waveform");
        auto replacement = std::make_shared<P::LoadedSampleData> (*source);
        replacement->buffer.clear();
        std::atomic_store (&p.loadedSample, replacement);
        p.sampleChangeBroadcaster.sendChangeMessage();
        check (waitFor ([&] { const auto colour = sampleColour (24000); return colour.getRed() < 80 && colour.getGreen() < 80; }),
               "sample replacement cannot retain the previous coloured waveform");
    }

    static void runCueRecall()
    {
        P original;
        auto source = std::make_shared<P::LoadedSampleData>();
        source->sampleRate = 8000;
        source->fileName = "cue recall fixture";
        source->buffer.setSize (1, 64000);
        source->buffer.clear();
        // Different silent lead-ins make unwanted onset detection observable.
        for (int i = 0; i < 4; ++i)
            for (int frame = i * 16000 + (i + 1) * 160; frame < (i + 1) * 16000; ++frame)
                source->buffer.setSample (0, frame, 0.5f);
        juce::MemoryBlock encoded;
        check (original.serializeSampleToStateData (*source, encoded), "encode cue recall fixture");
        source->serializedStateData = juce::var (encoded);
        std::atomic_store (&original.loadedSample, source);
        auto analysis = std::make_shared<P::TempoAnalysisData>();
        analysis->estimatedBpm = 120;
        analysis->beatPeriodSeconds = 0.5;
        analysis->analysisEndSeconds = 8;
        std::atomic_store (&original.tempoAnalysis, analysis);
        original.chopBarsCount.store (1);
        original.buildChopsFromAnalysis (*analysis);
        const auto generated = original.getChopState();
        check (generated->chops.size() == 4
               && std::all_of (generated->chops.begin(), generated->chops.end(),
                               [] (const auto& chop) { return chop.cueOffsetSamples == 0; }),
               "new chops start at zero even with different silent lead-ins");
        if (generated->chops.size() != 4) return;

        const std::array<float, 4> cueValues { 0.0f, 0.125f, 0.333f, 1.0f };
        for (size_t i = 0; i < cueValues.size(); ++i)
        {
            original.selectChopById (generated->chops[i].id);
            original.setSelectedChopCueNormalized (cueValues[i]);
        }
        const auto expected = original.getChopState();
        const auto cuesMatch = [&] (const std::shared_ptr<const P::ChopState>& actual)
        {
            if (actual == nullptr || actual->chops.size() != expected->chops.size()) return false;
            for (size_t i = 0; i < expected->chops.size(); ++i)
                if (actual->chops[i].startSample != expected->chops[i].startSample
                    || actual->chops[i].endSample != expected->chops[i].endSample
                    || actual->chops[i].cueOffsetSamples != expected->chops[i].cueOffsetSamples)
                    return false;
            return true;
        };
        std::atomic_store (&original.stashedChopState, std::make_shared<P::ChopState> (*expected));

        P reopened;
        for (bool manual : { false, true })
        {
            original.manualChopModeActive.store (manual);
            juce::MemoryBlock saved;
            original.getStateInformation (saved);
            for (int recall = 0; recall < 2; ++recall)
            {
                reopened.setStateInformation (saved.getData(), (int) saved.getSize());
                check (waitFor ([&] { return restored (reopened); }), "cue fixture async recall completes");
                check (cuesMatch (reopened.getChopState()), "zero, edited and end cues survive repeated recall in either layer");
                check (cuesMatch (std::atomic_load (&reopened.stashedChopState)), "inactive layer cue positions survive recall");
                check (reopened.manualChopModeActive.load() == manual, "cue recall preserves active layer");
                reopened.getStateInformation (saved);
            }
        }

        original.manualChopModeActive.store (false);
        original.buildChopsFromAnalysis (*analysis);
        check (cuesMatch (original.getChopState()), "grid rebuild preserves explicitly zeroed cues");

        // Moving boundaries must not turn a start cue into an absolute audio
        // anchor. Silent lead-ins also expose unwanted auto-cue fallback.
        const auto beforeZeroing = original.getChopState();
        for (const auto& chop : beforeZeroing->chops)
        {
            original.selectChopById (chop.id);
            original.setSelectedChopCueNormalized (0.0f);
        }
        const auto allCuesAtStart = [&]
        {
            const auto state = original.getChopState();
            return state != nullptr && ! state->chops.empty()
                && std::all_of (state->chops.begin(), state->chops.end(),
                                [] (const auto& chop) { return chop.cueOffsetSamples == 0; });
        };
        for (float trim : { 1.0f, 2.0f, -1.0f, 0.0f })
        {
            original.setGridBpmTrim (trim);
            check (allCuesAtStart(), "tempo nudges in either direction keep zero cues at the new chop starts");
        }
        for (float offset : { 0.01f, -0.01f, 0.02f, 0.0f })
        {
            original.setGridStartOffset (offset);
            check (allCuesAtStart(), "grid nudges and reset never invent cues or rerun onset detection");
        }
        original.setDoubleTempoEnabled (true);
        check (allCuesAtStart(), "double tempo keeps split chops at zero cue");
        original.setDoubleTempoEnabled (false);
        original.setChopBarsCount (2);
        check (allCuesAtStart(), "merging bars keeps zero cues");
        original.setChopBarsCount (1);
        check (allCuesAtStart(), "splitting bars keeps zero cues");
        const auto beforeResize = original.getChopState();
        original.resizeChopBoundaryAndTempo (beforeResize->chops[0].id, 80, 16000);
        check (allCuesAtStart(), "shift-resize keeps zero cues while changing tempo and grid together");
        original.undoLastEdit();
        check (allCuesAtStart() && original.getGridBpmTrim() == 0.0f
               && original.getGridStartOffset() == 0.0f,
               "undo restores cue and grid state after shift-resize");

        original.publishChopState (std::make_shared<P::ChopState> (*expected));
        const auto knobsMatch = [&] (const std::shared_ptr<const P::ChopState>& state)
        {
            if (state == nullptr || state->chops.empty()) return false;
            for (size_t i = 0; i < state->chops.size(); ++i)
            {
                const double wanted = i < cueValues.size() ? (double) cueValues[i] : 0.0;
                if (std::abs (state->chops[i].getCueNormalized() - wanted) > 1.0e-12) return false;
                const int offset = (int) std::round (wanted * juce::jmax (0, state->chops[i].endSample - state->chops[i].startSample - 1));
                if (state->chops[i].cueOffsetSamples != offset) return false;
            }
            return true;
        };
        bool stable = true;
        for (int i = 0; i < 20; ++i)
        {
            original.setGridBpmTrim (i % 2 == 0 ? 1.3f : -2.1f);
            stable = stable && knobsMatch (original.getChopState());
            // Keep four slots alive so this checks edits, rather than deleting/recreating the last chop.
            original.setGridStartOffset (i % 2 == 0 ? 0.013f : 0.02f);
            stable = stable && knobsMatch (original.getChopState());
        }
        check (stable, "repeated tempo/grid edits preserve exact cue knob percentages without rounding drift");
        original.setGridBpmTrim (0); original.setGridStartOffset (0);
        original.setDoubleTempoEnabled (true);
        check (knobsMatch (original.getChopState()), "2x preserves existing knob values and starts new slots at zero");
        original.setDoubleTempoEnabled (false);
        original.setChopBarsCount (2);
        check (knobsMatch (original.getChopState()), "bar changes preserve surviving numbered chop knob settings");
        original.undoLastEdit();
        check (knobsMatch (original.getChopState()), "undo restores the cue percentages of removed slots");

        const auto before = original.getChopState();
        const auto id = before->chops[2].id;
        original.resizeChopBoundaryAndTempo (id, 32000, 47000);
        check (knobsMatch (original.getChopState()), "shift-resize preserves cue percentages while moving both grid and tempo");
        original.undoLastEdit();
        original.setChopBounds (id, 32000, 32002);
        check (knobsMatch (original.getChopState()), "shrinking a chop to two samples preserves its exact knob setting");
        juce::MemoryBlock tinySaved; original.getStateInformation (tinySaved);
        reopened.setStateInformation (tinySaved.getData(), (int) tinySaved.getSize());
        check (waitFor ([&] { return restored (reopened); }) && knobsMatch (reopened.getChopState()),
               "saved knob percentage survives recall even when the sample offset cannot represent it exactly");
        original.setChopBounds (id, 32000, 48000);
        check (knobsMatch (original.getChopState()), "expanding the chop restores its cue offset without percentage drift");

        // Legacy sessions have offsets but no normalized knob property. Preserve
        // those offsets in both layers, then freeze the inferred knob percentages.
        original.publishChopState (std::make_shared<P::ChopState> (*expected));
        juce::MemoryBlock modern; original.getStateInformation (modern);
        juce::MemoryInputStream input (modern.getData(), modern.getSize(), false); input.skipNextBytes (4);
        auto legacy = juce::ValueTree::readFromStream (input);
        for (const auto* name : { "ChopState", "StashedChopState" })
            for (auto chop : legacy.getChildWithName (name)) chop.removeProperty ("cuePositionNormalized", nullptr);
        juce::MemoryBlock oldData;
        { juce::MemoryOutputStream output (oldData, false); output.write ("CSB2", 4); legacy.writeToStream (output); }
        reopened.setStateInformation (oldData.getData(), (int) oldData.getSize());
        check (waitFor ([&] { return restored (reopened); }) && cuesMatch (reopened.getChopState())
               && cuesMatch (std::atomic_load (&reopened.stashedChopState)),
               "legacy active and inactive layers preserve every saved cue offset");
        const auto legacyCues = reopened.getChopState();
        reopened.setGridBpmTrim (1.7f);
        bool legacyStable = true;
        for (size_t i = 0; i < legacyCues->chops.size(); ++i)
            legacyStable = legacyStable && std::abs (reopened.getChopState()->chops[i].getCueNormalized()
                                                    - legacyCues->chops[i].getCueNormalized()) < 1.0e-12;
        check (legacyStable, "legacy cue values remain stable when tempo changes after recall");
        check (original.chopAtTransients (P::TransientSensitivity::Fine) > 0 && allCuesAtStart(),
               "transient-generated chops also start at zero cue");
    }

    static void run (bool testSeparation)
    {
        {
            P codec;
            P::LoadedSampleData highRate;
            highRate.sampleRate = 96000.0;
            highRate.buffer.setSize (2, 9600);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 9600; ++i)
                    highRate.buffer.setSample (ch, i, 0.4f * std::sin ((float) (juce::MathConstants<double>::twoPi * 1000.0 * i / 96000.0)));
            juce::MemoryBlock compact;
            const auto encoded = codec.serializeSampleToStateData (highRate, compact);
            const auto decoded = encoded ? codec.createLoadedSampleDataFromStateData (compact, "96k") : nullptr;
            check (decoded != nullptr && decoded->sampleRate == 96000.0
                   && decoded->buffer.getNumSamples() == 9600
                   && correlation (highRate.buffer, decoded->buffer) > 0.98,
                   "96 kHz source state retains its sample clock and chop coordinates");
            check ((juce::int64) compact.getSize() < (juce::int64) 9600 * 2 * (juce::int64) sizeof (float) / 2,
                   "embedded MP3 source is substantially smaller than float PCM");
        }

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
        check (correlation (originalStems->source->buffer, restoredStems->source->buffer) > 0.95,
               "source restores from compact MP3 without timing drift");
        check (correlation (originalStems->drums, restoredStems->drums) > 0.95
            && correlation (originalStems->bass, restoredStems->bass) > 0.95
            && correlation (originalStems->vocals, restoredStems->vocals) > 0.95,
               "all stems restore from compact MP3 without timing drift");
        check (correlation (originalMix->buffer, reopened.getLoadedSample()->buffer) > 0.90,
               "saved mute mix restores from compact MP3");
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
        check (correlation (originalStems->source->buffer, reopened.getLoadedSample()->buffer) > 0.95,
               "unmuting restores the MP3 source without double subtraction");

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
               && correlation (originalMix->buffer, reopened.getLoadedSample()->buffer) > 0.90,
               "legacy mute mix restored");
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
            // Keep the real editor's timers running during model loading and
            // inference, and measure message dispatch throughout the pass.
            std::unique_ptr<juce::AudioProcessorEditor> separatingEditor (original.createEditor());
            struct Heartbeat final : juce::Timer
            {
                Heartbeat() { startTimer (20); }
                ~Heartbeat() override { stopTimer(); }
                void timerCallback() override
                {
                    recordGap();
                    ++callbacks;
                }
                void recordGap()
                {
                    const auto now = juce::Time::getMillisecondCounterHiRes();
                    worstGap = juce::jmax (worstGap, now - lastTick);
                    lastTick = now;
                }
                double lastTick = juce::Time::getMillisecondCounterHiRes();
                double worstGap = 0.0;
                int callbacks = 0;
            } heartbeat;
            // Deliberately stall optional persistence: readiness, mute remix and
            // an immediate portable save/restore must still complete.
            auto cacheGate = std::make_shared<juce::WaitableEvent>();
            original.stemCacheWriteThreadPool.addJob ([cacheGate] { cacheGate->wait (-1); });
            const auto readyStart = juce::Time::getMillisecondCounterHiRes();
            original.requestStemSeparation();
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 180000.0;
            // Publication clears the busy flag before committing the active mix.
            // Wait for the complete job before snapshotting audio for comparison.
            while (original.stemThreadPool.getNumJobs() != 0
                   && juce::Time::getMillisecondCounterHiRes() < deadline) pump();
            heartbeat.stopTimer();
            heartbeat.recordGap();
            std::cout << "STEM UI callbacks=" << heartbeat.callbacks
                      << " max dispatch gap ms=" << heartbeat.worstGap << std::endl;
            check (heartbeat.callbacks > 0 && heartbeat.worstGap < 1000.0,
                   "editor message dispatch remains responsive during real separation");
            const auto actualStems = std::atomic_load (&original.stemSet);
            std::cout << "STEM READY ms=" << juce::Time::getMillisecondCounterHiRes() - readyStart << std::endl;
            check (! original.stemSeparationInProgress.load() && original.areStemsReady() && actualStems != nullptr
                   && actualStems != originalStems && actualStems->serializedStemData.getBinaryData() != nullptr,
                   "real separation publishes portable stem data before cache write");
            if (actualStems != nullptr)
            {
                const auto actualMix = original.getLoadedSample();
                juce::MemoryBlock actualSaved;
                original.getStateInformation (actualSaved);
                check (! cuesampler::StemCache::contains (actualStems->cacheKey), "stems are ready while disk cache is blocked");
                original.setMuteDrums (false); original.setMuteVocals (false);
                check (waitFor ([&] { return original.appliedStemMask.load() == 0; }), "mute remix completes while disk cache is blocked");
                check (equal (actualStems->source->buffer, original.getLoadedSample()->buffer), "blocked cache cannot interfere with unmuted audio");
                reopened.setStateInformation (actualSaved.getData(), (int) actualSaved.getSize());
                check (waitFor ([&] { return restored (reopened); }), "real separation project reopens without cache/model");
                check (correlation (actualMix->buffer, reopened.getLoadedSample()->buffer) > 0.90,
                       "real separated mix restores from compact MP3");
            }
            cacheGate->signal();
            check (waitFor ([&] { return original.stemCacheWriteThreadPool.getNumJobs() == 0; }), "optional cache writing finishes after release");
            cuesampler::StemCache::Entry cached;
            check (actualStems != nullptr && cuesampler::StemCache::load (actualStems->cacheKey, cached)
                   && correlation (cached.drums, actualStems->drums) > 0.90,
                   "deferred cache write retains aligned MP3 stems");
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
    const auto filter = argc > 2 ? juce::String (argv[2]) : juce::String();
    const bool full = filter.isEmpty() || filter == "--separate";
    if (filter == "--ui-benchmark") CueSamplerStateTests::runUiBenchmark();
    if (filter == "--sample-import") CueSamplerStateTests::runSampleImport (scratch);
    if (filter == "--import-benchmark") CueSamplerStateTests::runImportBenchmark (scratch);
    if (full || filter == "--midi-performance") CueSamplerStateTests::runMidiPerformance (scratch);
    if (full || filter == "--chop-pads") CueSamplerStateTests::runChopPads (scratch);
    if (full || filter == "--octave-banking") CueSamplerStateTests::runOctaveBanking (scratch);
    if (full || filter == "--favorites-audio") CueSamplerStateTests::runFavoritesAudio();
    if (full || filter == "--waveform-zoom") CueSamplerStateTests::runWaveformZoom (scratch);
    if (full || filter == "--performance-ui") CueSamplerStateTests::runPerformanceUi (scratch);
    if (full || filter == "--cue-recall") CueSamplerStateTests::runCueRecall();
    if (full || filter == "--manual-chops") CueSamplerStateTests::runManualChops (scratch);
    if (full || filter == "--waveform-colour") CueSamplerStateTests::runWaveformColour (scratch);
    if (filter == "--mp3-exports")
        CueSamplerStateTests::runExports (scratch);
    if (full || filter == "--state-only")
    {
        if (filter != "--state-only")
        {
            CueSamplerStateTests::runExports (scratch);
            CueSamplerStateTests::runMousePlaybackModes();
            CueSamplerStateTests::runExportHandle();
        }
        CueSamplerStateTests::runFavorites();
        CueSamplerStateTests::runDoubleTempo (scratch);
        CueSamplerStateTests::run (argc > 2 && juce::String (argv[2]) == "--separate");
    }
    scratch.deleteRecursively();
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
