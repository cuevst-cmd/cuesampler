# Stem Separation — Claude Code Prompt Series

A phased prompt series for implementing stem separation in **SAMPLERv3 / CueSampler**.
Feed these to Claude Code **one phase at a time**, in order. Verify each phase builds
and passes its acceptance check before moving on.

## How to use
1. Open Claude Code in the `SAMPLERv3` repo root.
2. **Paste PHASE 0 (Context) first** in a fresh session — it primes Claude Code with
   the architecture so later phases stay grounded.
3. Then paste **Phase 1**. When it's done and the acceptance check passes, paste
   **Phase 2**, and so on. Keep the same session so context carries over.
4. If you start a new session mid-way, re-paste PHASE 0 first.

The design rationale for all of this is in `STEM_SEPARATION_PLAN.md` (same folder).

---

## PHASE 0 — CONTEXT (paste once at the start of the session)

```
We are adding audio stem separation to this JUCE/C++ audio plugin (CueSampler,
a chopping sampler). Read these files before doing anything so you understand the
existing patterns; mirror them rather than inventing new ones:

- PluginProcessor.h / PluginProcessor.cpp  — audio engine, sample loading, background jobs
- PluginEditor.h / PluginEditor.cpp        — fixed-pixel rack UI
- BeatThisAnalyzer.h / BeatThisAnalyzer.cpp — EXISTING ONNX Runtime model integration (our template)
- CMakeLists.txt                            — how onnxruntime + the beat_this.onnx model are bundled
- export_beat_this.py                       — how the existing ONNX model was exported (template for ours)

Key existing facts to reuse (do not re-derive, do not break):
- ONNX Runtime 1.20.1 is already bundled and links via @loader_path. `BeatThisAnalyzer`
  is constructed with an absolute path to a .onnx file, holds Ort::Env/SessionOptions/
  Session, exposes isReady(), and runs analyze() on a background thread.
- Model path resolution pattern lives in PluginProcessor.cpp (~line 2005): it checks
  Contents/Resources/<model>, then next to the binary, then the dev-tree assets/<model>.
- Background analysis pattern: loadAudioFile() (~3660) calls launchTempoAnalysis()
  (~4774), which clears its thread pool, bumps a generation counter, and adds a job to a
  single-thread juce::ThreadPool. Results are published via a generation-guarded
  publishTempoAnalysis() that does std::atomic_store(&tempoAnalysis, …).
- The live sample is std::shared_ptr<LoadedSampleData> loadedSample, swapped with
  std::atomic_store(&loadedSample, …). All playback/warp/pitch readers read
  loadedSample->buffer. LoadedSampleData holds `juce::AudioBuffer<float> buffer`,
  sampleRate, etc.
- The editor is fixed-pixel. Top-level layout is in AudioPluginAudioProcessorEditor::
  resized() (~6567). Left column today:
      waveformDisplayComponent : (96, 133, 782, 411)
      transportSectionComponent: (96, 552, 782, 236)
  Right column (utilityStrip, effectsRackComponent) is untouched by this work.
- EffectsRackComponent is the styling template for our new panel.

The feature we are building (locked decisions):
1. Model: HTDemucs-FT (max quality). Separation runs AUTOMATICALLY in the background
   when a sample loads. It is an OFFLINE pass (tens of seconds), never a per-block effect.
2. Storage uses SUBTRACTION: store only the drums, bass, and vocals stems. The played
   buffer = original − Σ(muted stems). So "nothing muted" == original audio bit-for-bit,
   and the "other" stem is implicit and always plays.
3. UI: a NEW rack panel directly under the waveform with exactly three mute toggle
   buttons — BASS, DRUMS, VOCALS. The waveform is condensed to make room.
4. Muting must be instant once stems exist; it works by rebuilding the mix buffer and
   atomic-swapping loadedSample, reusing the existing swap path. Do NOT rewrite the DSP
   path. If stems aren't ready yet, buttons are disabled and the original plays.
5. Never break existing chop / warp / pitch / prepared-cache behavior. Fall back to the
   original buffer on any model/inference failure.

Don't write code yet. Confirm you've read the files and briefly restate the plan and
the specific functions/lines you'll mirror. Then wait for the Phase 1 prompt.
```

---

## PHASE 1 — Model export + bundling

```
PHASE 1: Produce the HTDemucs-FT ONNX models and bundle them, mirroring how
beat_this.onnx is handled.

Tasks:
1. Create export_htdemucs.py (sibling of export_beat_this.py). It must export the
   htdemucs_ft model to ONNX suitable for C++ ONNX Runtime inference. Use the
   `demucs-onnx` package (StemSplit) which already solves the known export blockers
   (complex STFT tensors, fractions.Fraction in model.segment, random.randrange in the
   cross-transformer, fused multi-head-attention). Critically, keep STFT/iSTFT OUTSIDE
   the ONNX graph (the sevagh/demucs.onnx approach) so the net takes spectrogram input
   and returns spectrogram output. The script should:
     - download/load htdemucs_ft,
     - export the four fine-tuned sub-models (drums, bass, vocals, other) to
       assets/htdemucs_ft/<stem>.onnx (+ .onnx.data sidecars if weights exceed the
       2GB single-file limit, exactly like beat_this.onnx.data),
     - run a numpy/ORT-vs-PyTorch parity check and print max abs error,
     - print the expected input/output tensor names, shapes, dtypes, and the STFT
       params (n_fft, hop) so Phase 2 can match them.
2. Update CMakeLists.txt to copy the htdemucs_ft model files into the bundle, mirroring
   the existing beat_this.onnx copy commands for: VST3 (Contents/Resources/), AU
   (Contents/Resources/), and Windows (next to the binary). Include any .onnx.data
   sidecars. Do NOT use juce_add_binary_data for these (too large) — use the same
   add_custom_command COPY pattern already used for beat_this.onnx.
3. Document in a comment the four model filenames and total on-disk size.

Acceptance:
- `python export_htdemucs.py` produces assets/htdemucs_ft/*.onnx and prints a parity
  max-abs-error below a small threshold (e.g. < 1e-3).
- A clean CMake configure+build copies the models into the built VST3/AU Resources dir
  (verify with `ls` on the built bundle).
- No change yet to runtime behavior; the plugin still builds and runs as before.

Print the input/output tensor spec you discovered — Phase 2 depends on it.
```

---

## PHASE 2 — `StemSeparator` inference core (+ offline test)

```
PHASE 2: Implement the C++ ONNX Runtime inference class, mirroring BeatThisAnalyzer.

Create StemSeparator.h / StemSeparator.cpp:
- Constructor takes the resolved path(s) to the htdemucs_ft ONNX model(s). Open one
  Ort::Session per sub-model. Forward-declare Ort types in the header (as
  BeatThisAnalyzer.h does). Expose isReady().
- Public API:
    struct StemResult {
        bool valid = false;
        juce::AudioBuffer<float> drums, bass, vocals;   // at the SOURCE sample rate, source length/channels
    };
    StemResult separate(const juce::AudioBuffer<float>& buffer,
                        double sampleRate,
                        std::function<void(float)> progress = {}) const;   // progress in [0,1]
  (We only need drums/bass/vocals — "other" is original minus these, computed later.)
- Internals (match Demucs preprocessing exactly, using the params Phase 1 printed):
    - resample input to 44100 Hz stereo,
    - segment into ~7.8 s windows with 0.25 overlap,
    - compute STFT in C++ (JUCE dsp FFT is available; n_fft/hop from Phase 1),
    - run each FT sub-model, take its specialty stem,
    - inverse-STFT + overlap-add,
    - resample stems back to the source rate and match source channel count/length,
    - call progress() across segments; make it abort-friendly.
- VERIFY the output stem ordering against the exported graph (standard Demucs source
  order is [drums, bass, other, vocals]); assert it in the test below.

Add an offline test in tools/ (mirror tools/test_warp_map.cpp): load a short stereo WAV,
run separate(), and assert:
  (a) each returned stem has source length & channel count,
  (b) drums+bass+vocals+other ≈ original within tolerance, where other = original −
      (drums+bass+vocals) (i.e. the residual reconstructs), and
  (c) energy sanity per stem (non-silent).
Note: tools/test_warp_map.cpp is a STANDALONE file (it is not wired into CMakeLists.txt
— only WarpMap.cpp is part of the main target). Build your stem test the same way:
either compile it standalone against StemSeparator.cpp + onnxruntime, or add an OPTIONAL
CMake test target guarded behind an off-by-default option. Document the exact build/run
command at the top of the test file.

Do NOT touch PluginProcessor/PluginEditor yet. Keep everything offline and testable.

Acceptance: the tools test builds and passes on a sample WAV; reconstruction error is
small; ordering assertion holds.
```

---

## PHASE 3 — Processor integration (storage, auto-on-load, mute remix)

```
PHASE 3: Wire StemSeparator into PluginProcessor with auto-on-load separation and
instant mute via buffer subtraction. Mirror the launchTempoAnalysis job pattern.

1. Storage (subtraction model):
   - Add a StemSet type holding shared drums/bass/vocals buffers:
       struct StemSet { juce::AudioBuffer<float> drums, bass, vocals; };
   - Hold the current stems as std::shared_ptr<const StemSet> stemSet (atomic_load/store).
   - Add std::atomic<bool> muteDrums{false}, muteBass{false}, muteVocals{false}.
   - Add progress/ready state: std::atomic<bool> stemsReady{false},
     stemSeparationInProgress{false}, std::atomic<float> stemProgress{0}.

2. Model loading: add a StemSeparator member; resolve model path(s) using the SAME
   helper/pattern as beat_this (Contents/Resources → binary dir → assets/). Construct
   it once (like beatThisAnalyzer); log ready=YES/NO.

3. Auto-on-load: add launchStemSeparation(std::shared_ptr<const LoadedSampleData>)
   mirroring launchTempoAnalysis: a dedicated single-thread juce::ThreadPool
   (stemThreadPool), a generation counter (stemGeneration), removeAllJobs on relaunch,
   and a generation-guarded publishStems(). Call launchStemSeparation(sampleData) right
   after launchTempoAnalysis in loadAudioFile (~3695) AND in the restore/reload paths
   (~3512, ~5008, ~5021). Run below realtime priority.

4. publishStems(stemSet, generation): if generation is current, atomic_store the
   StemSet, set stemsReady=true, stemSeparationInProgress=false, then call
   rebuildActiveMix().

5. rebuildActiveMix(): build activeMix = original − Σ(muted of {drums,bass,vocals}),
   on a background thread (O(N) per-sample subtract; clamp lengths/channels). Wrap it in
   a NEW LoadedSampleData that copies the metadata of the current loadedSample but uses
   activeMix as `buffer`, then std::atomic_store(&loadedSample, newSample) — the exact
   swap mechanism already used elsewhere. Then bump warpRenderGeneration /
   prepareWarmGeneration so the warp + prepared caches refresh, like a sample change
   does. IMPORTANT: keep a pristine copy of the original buffer (the loaded sample's
   buffer before any muting) to subtract against — store it in LoadedSampleData or
   alongside the StemSet so repeated mutes don't accumulate error.

6. Public setters/getters: setMuteDrums/Bass/Vocals(bool) (each sets the flag and calls
   rebuildActiveMix), getters for the flags, isSeparatingStems(), areStemsReady(),
   getStemProgress().

7. Persistence: save the three mute flags in getStateInformation and restore them in
   setStateInformation. On restore, re-run separation (do NOT serialize stem audio);
   apply saved mute flags once stems are ready.

8. Failure handling: if the separator isn't ready or returns invalid, leave the original
   buffer in place, stemsReady=false, mutes disabled. The plugin must behave exactly as
   today when separation is unavailable.

Acceptance: load a sample → stems compute in the background (log progress) → toggling
muteVocals/Drums/Bass changes the audible playback within a moment; with all three
false, playback equals the original; chops/warp/pitch still work.
```

---

## PHASE 4 — UI: condense waveform + StemRackComponent (3 mute buttons)

```
PHASE 4: Add the stem mute panel under the waveform and condense the waveform.

1. New StemRackComponent (in PluginEditor.cpp alongside the other rack components, or a
   new file consistent with how EffectsRackComponent is organized). Style it to match
   EffectsRackComponent (same glass/metal look, screws/slots, fonts). It contains:
   - Three toggle buttons labeled BASS, DRUMS, VOCALS (toggled = muted; show a clear
     active/muted state, e.g. lit when muted).
   - A small status line: shows "SEPARATING…" with progress while
     isSeparatingStems() is true; "READY" (or hidden) when areStemsReady(); the three
     buttons are DISABLED until stems are ready.

2. Wire buttons to the processor:
     bassBtn.onClick   -> processorRef.setMuteBass(bassBtn.getToggleState());
     drumsBtn.onClick  -> processorRef.setMuteDrums(...);
     vocalsBtn.onClick -> processorRef.setMuteVocals(...);
   Initialize toggle states from the processor getters (for state recall). Poll
   progress/ready on the existing editor timer and update enablement + status text.

3. Layout — edit AudioPluginAudioProcessorEditor::resized() (~6574):
     waveformDisplayComponent : (96, 133, 782, 330)   // was 411
     stemRackComponent        : (96, 471, 782,  73)   // NEW
     transportSectionComponent: (96, 552, 782, 236)   // unchanged
   Keep helpOverlayComponent at (96, 133, 782, 655). Construct/own stemRackComponent in
   the editor like the other sub-components (unique_ptr, addAndMakeVisible, declared in
   PluginEditor.h).

4. Confirm the condensed waveform still renders and interacts correctly (zoom/scroll/
   chop selection/warp markers) at the new height.

Acceptance: the panel appears directly under the waveform with three working mute
buttons; buttons are disabled with a "separating" status until stems are ready, then
enabled; toggling them mutes/unmutes; the waveform and all its interactions still work
at 330px tall.
```

---

## PHASE 5 — Polish & verification

```
PHASE 5: Harden and verify. No new features — correctness, edges, and tests.

Edge cases & polish:
- No sample loaded: panel shows a neutral disabled state.
- Loading a new sample while a previous separation is running: old job is abandoned via
  the generation guard; progress resets.
- Separation failure / model missing: buttons stay disabled, original plays, a log line
  explains why; no crash.
- Very long samples: add a sane max-length guard for separation (configurable constant);
  above it, skip separation and disable the panel with a brief reason.
- Mute + half-time + host-sync + warp all interacting: confirm rebuildActiveMix +
  cache-refresh keeps them consistent (no stale warp bakes after a mute).
- Thread safety: all loadedSample/stemSet swaps via atomic_store; no audio-thread
  allocation; rebuildActiveMix runs off the audio thread.
- macOS acceleration: enable the CoreML execution provider for the StemSeparator
  sessions with CPU fallback (ONNX Runtime + coreml_provider_factory.h are already
  bundled); verify it still works CPU-only.

Verification:
- Add/extend the tools test to cover the subtraction mix: assert
  activeMix == original when no mutes, and activeMix == original − vocals when only
  vocals muted, within tolerance.
- Write a short MANUAL_TEST.md checklist: load WAV → wait for READY → mute each stem and
  confirm audibly → save/reload project and confirm mute flags persist and stems
  re-separate → load a second sample → confirm panel resets.
- Run a quick self-review for memory growth across repeated loads (stems freed when a
  new sample loads — confirm the old StemSet is released).

Acceptance: all items above verified; the tools tests pass; MANUAL_TEST.md checklist
passes end to end; existing functionality is unchanged when stems are disabled.
```

---

### Build/run reminders for each phase
- Configure & build the existing CMake project the way the repo already documents.
- After Phases 3–4, load the plugin in your DAW (or pluginval/AudioPluginHost), drop in
  a WAV, and watch the logs for `StemSeparator: ready=…` and progress.
- Keep changes additive; if a phase risks regressions, branch first.
