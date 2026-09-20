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
        check (findButton (*editor, "SAVE WAV...") == nullptr, "preparation click does not open the ADSR menu");

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
        check (findButton (*editor, "SAVE WAV...") != nullptr, "ADSR opens the envelope menu with a separate Save WAV action");
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
        check (p.getMidiNoteForChopId (3) == 36 && p.getMidiNoteForChopId (1) == 37
               && p.getMidiNoteForChopId (4) == 38 && p.getMidiNoteForChopId (2) == -1,
               "only favorites map consecutively from C2, overriding pins temporarily");
        p.setMidiOctaveOffset (-1);
        check (p.getMidiOctaveOffset() == 1 && p.getMidiRootNote() == 36,
               "Favorites keeps C2 without overwriting normal octave setting");
        bool intact = p.getLoadedSample() == source && p.getChopState()->chops.size() == 4;
        for (int i = 0; i < 4; ++i)
        {
            const auto& c = p.getChopState()->chops[(size_t) i];
            intact = intact && c.startSample == i * 16000 && c.endSample == (i + 1) * 16000
                   && c.cueOffsetSamples == 17 && c.assignedMidiNote == layout->chops[(size_t) i].assignedMidiNote
                   && c.gainDecibels == -2 && c.pitchSemitones == 1;
        }
        check (intact, "Favorites preserves audio, bounds, processing and original MIDI assignments");
        p.prepareToPlay (8000, 64);
        juce::AudioBuffer<float> output (2, 64);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
        p.processBlock (output, midi);
        check (p.getLastTriggeredChopId() == 3, "real MIDI C2 triggers the first favorited chop");
        const auto revision = p.getChopTriggerRevision();
        midi.clear(); midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100), 0);
        p.processBlock (output, midi);
        check (p.getChopTriggerRevision() == revision, "nonfavorite original MIDI assignment is inactive in Favorites");
        p.selectChopById (3); p.toggleSelectedChopFavorite();
        check (p.getMidiNoteForChopId (1) == 36 && p.getMidiNoteForChopId (4) == 37,
               "removing a favorite closes the MIDI gap");
        midi.clear(); p.processBlock (output, midi);
        check (! p.voice.playbackActive, "remapping favorites silences a held voice safely on the audio thread");
        p.undoLastEdit();
        check (p.getChopState()->favoriteChopIndices == std::vector<int> { 2, 0, 3 }
               && p.getMidiNoteForChopId (3) == 36, "Undo restores favorite order and compact MIDI mapping");
        p.selectChopById (3); p.toggleSelectedChopFavorite();
        p.setFavoritesViewEnabled (false);
        p.selectChopById (3); p.toggleSelectedChopFavorite();
        p.setFavoritesViewEnabled (true);
        check (p.getChopState()->favoriteChopIndices == std::vector<int> { 0, 3, 2 },
               "re-favoriting appends a chop to the end of the lineup");
        juce::MemoryBlock saved; p.getStateInformation (saved);
        P reopened;
        reopened.setStateInformation (saved.getData(), (int) saved.getSize());
        check (waitFor ([&] { return restored (reopened); }) && reopened.isFavoritesViewEnabled()
               && reopened.getChopState()->favoriteChopIndices == std::vector<int> { 0, 3, 2 }
               && reopened.getMidiNoteForChopId (1) == 36,
               "project recall restores Favorites view, chronology and MIDI map");
        std::unique_ptr<juce::AudioProcessorEditor> reopenedEditor (reopened.createEditor());
        auto* restoredButton = findButton (*reopenedEditor, "FAVORITES");
        check (restoredButton != nullptr && restoredButton->getToggleState(), "reopened editor shows Favorites enabled");
        reopened.setFavoritesViewEnabled (false);
        check (reopened.getChopState()->midiMap.noteToChopIndex == originalMap && reopened.getMidiRootNote() == 48,
               "leaving Favorites after recall restores exact original mapping and root");
        for (int id : { 1, 3, 4 }) { p.selectChopById (id); p.toggleSelectedChopFavorite(); }
        check (p.getChopState()->favoriteChopIndices.empty() && p.getMappedMidiNotes().none()
               && p.getChopState()->selectedChopId == -1, "empty Favorites has no phantom MIDI assignments or hidden selection");
        p.setFavoritesViewEnabled (false);
        check (p.getChopState()->midiMap.noteToChopIndex == originalMap, "empty Favorites can return to full original layout");

        juce::MemoryInputStream input (saved.getData(), saved.getSize(), false); input.skipNextBytes (4);
        auto legacy = juce::ValueTree::readFromStream (input);
        legacy.removeProperty ("favoritesViewEnabled", nullptr);
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
        const auto exported = readExport (p.renderChopToTempWav (p.getChopState()->chops[0].id, true, scratch), 8000);
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
            // Deliberately stall optional persistence: readiness, mute remix and
            // an immediate portable save/restore must still complete.
            auto cacheGate = std::make_shared<juce::WaitableEvent>();
            original.stemCacheWriteThreadPool.addJob ([cacheGate] { cacheGate->wait (90000); });
            const auto readyStart = juce::Time::getMillisecondCounterHiRes();
            original.requestStemSeparation();
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 60000.0;
            while (original.stemSeparationInProgress.load() && juce::Time::getMillisecondCounterHiRes() < deadline) pump();
            const auto actualStems = std::atomic_load (&original.stemSet);
            std::cout << "STEM READY ms=" << juce::Time::getMillisecondCounterHiRes() - readyStart << std::endl;
            check (! original.stemSeparationInProgress.load() && actualStems != nullptr
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
                check (equal (actualMix->buffer, reopened.getLoadedSample()->buffer), "real separated mix restores bit-exactly");
            }
            cacheGate->signal();
            check (waitFor ([&] { return original.stemCacheWriteThreadPool.getNumJobs() == 0; }), "optional cache writing finishes after release");
            cuesampler::StemCache::Entry cached;
            check (actualStems != nullptr && cuesampler::StemCache::load (actualStems->cacheKey, cached)
                   && equal (cached.drums, actualStems->drums), "deferred cache write retains exact stems");
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
    CueSamplerStateTests::runMousePlaybackModes();
    CueSamplerStateTests::runExportHandle();
    CueSamplerStateTests::runFavorites();
    CueSamplerStateTests::runDoubleTempo (scratch);
    CueSamplerStateTests::run (argc > 2 && juce::String (argv[2]) == "--separate");
    scratch.deleteRecursively();
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
