# Stem Separation — Implementation Plan (SAMPLERv3 / CueSampler)

**Owner:** Jerry · **Date:** 2026-06-17 · **Status:** Approved scope, ready for build

This is the "perfect plan" half of the deliverable. The executable half — the
phased prompts you feed Claude Code one at a time — lives in
`STEM_SEPARATION_CC_PROMPTS.md` in this folder.

---

## 1. Goal

When a sample is loaded, automatically separate it into stems using **HTDemucs-FT**
(Meta's fine-tuned hybrid-transformer Demucs v4) running through the **ONNX Runtime
that is already bundled in this project**. Add a new rack panel directly **beneath
the waveform** (condensing the waveform to make room) with **three mute buttons:
BASS, DRUMS, VOCALS**. The remaining "other" content always plays. Muting a stem
removes it from playback instantly.

### Locked decisions (from requirements Q&A)

| Question | Decision |
|---|---|
| When does separation run? | **Automatically in the background on every sample load** |
| Which model? | **HTDemucs-FT** (max quality; ~4 ONNX models, larger + slower) |
| Stem controls? | **3 mute buttons** — BASS, DRUMS, VOCALS. "Other" always plays |
| Claude Code deliverable? | **Phased prompt series** (one prompt per phase) |

---

## 2. Why this fits SAMPLERv3 unusually well

- **The ML runtime is already here.** The project bundles ONNX Runtime 1.20.1 and
  already runs a neural model (`BeatThisAnalyzer`, the beat tracker). Stem
  separation reuses the *same* runtime, the *same* model-bundling CMake recipe, and
  the *same* background-job pattern. No new heavyweight dependency.
- **A sampler pre-loads its audio.** Demucs is an *offline* pass over a whole file,
  not a live per-block effect. That is awkward for a live DJ deck but perfect for a
  sampler: separate once on load, cache the stems, then muting is just arithmetic.
- **A proven C++/ONNX path exists.** The Mixxx DJ app shipped Demucs v4 → ONNX → C++
  ONNX Runtime as a 2025 project; `sevagh/demucs.onnx` is a working C++ ORT inference
  implementation; `StemSplit/demucs-onnx` exports htdemucs_ft with PyTorch-parity
  checks. We are not breaking new research ground.

### The one hard constraint

HTDemucs-FT is **not real-time**. It runs as a one-time analysis pass. Base HTDemucs
is ~0.2× real-time on Apple Silicon CPU (≈ a few seconds for a short loop); **the FT
variant runs four specialized models, so it is ~4× that** (tens of seconds for a full
song). Therefore: separation happens on a background thread after load, the UI shows
a progress/"separating…" state, and the mute buttons stay disabled until stems are
ready. This is designed into the plan, not worked around.

---

## 3. Research: open-source options

| Model | Stems | Quality (MUSDB SDR) | Speed | Size | Notes |
|---|---|---|---|---|---|
| **HTDemucs-FT** ✅ | 4 (drums/bass/vocals/other) | **Highest** (~9+ dB, best published OSS) | Slowest (4-model bag) | ~320 MB | Fine-tuned bag of 4; our choice |
| HTDemucs | 4 | ~9.0 dB | Moderate | ~80 MB | Single model; good fallback if size/time hurt |
| MDX-Net | 4 | ~8 dB | Fast | ~50–100 MB | Used by Ultimate Vocal Remover |
| Open-Unmix | 4 | ~6.3 dB | Fast | Small | Clean PyTorch reference, older quality |
| Spleeter | 2/4/5 | ~5.9 dB | Very fast | Small | Fast but noticeably more bleed |

**Decision: HTDemucs-FT**, per the max-quality requirement.

**Tooling we'll lean on (Phase 1):**
- `StemSplit/demucs-onnx` (PyPI `demucs-onnx`) — exports htdemucs_ft to ONNX with
  numpy/ORT parity checks; handles the four known export blockers (complex STFT
  tensors, `fractions.Fraction` in `model.segment`, `random.randrange` in the
  cross-transformer, fused MHA kernel).
- `sevagh/demucs.onnx` — reference **C++ ONNX Runtime** inference; key idea: keep
  **STFT/iSTFT outside** the ONNX graph (compute spectrogram in C++, feed the net,
  inverse-transform the output).

**Sources:**
- [Mixxx GSoC 2025 — Demucs v4 → ONNX](https://mixxx.org/news/2025-10-27-gsoc2025-demucs-to-onnx-dhunstack/)
- [sevagh/demucs.onnx (C++ ORT inference)](https://github.com/sevagh/demucs.onnx)
- [StemSplit/demucs-onnx (htdemucs_ft export)](https://github.com/StemSplit/demucs-onnx)
- [demucs-onnx on PyPI](https://pypi.org/project/demucs-onnx/)
- [HT-Demucs FT → ONNX export notes (2026)](https://stemsplit.io/blog/htdemucs-ft-onnx-export)
- [Demucs vs Spleeter quality comparison](https://stemsplit.io/blog/spleeter-vs-demucs)

---

## 4. Pros & cons of implementing stem separation

### Pros
- **Marquee creative feature.** Instant instrumentals (mute vocals), isolated drums
  for chopping, basslines for practice — directly multiplies what the sampler is for.
- **Low marginal architecture cost.** Reuses ONNX Runtime, the model-bundling recipe,
  and the background-analysis pattern already in the codebase.
- **Instant, glitch-free muting** once stems exist (see §5 — muting is a buffer
  subtraction, and "nothing muted" reproduces the original audio bit-for-bit).
- **Best-in-class quality** with HTDemucs-FT; competitive with paid cloud services.
- **Offline/local.** No cloud, no per-track fees, no network dependency, private.

### Cons / costs
- **Bundle size** grows by ~320 MB (four FT models). Affects installer/download size.
- **Processing latency on load.** Tens of seconds for a full song on CPU; the user
  waits (with progress UI) before mutes are usable.
- **CPU & memory spike** during separation; **RAM** to hold stems scales with sample
  length (§7 risk + mitigation).
- **Integration care needed.** Stems must coexist with the existing chop / warp /
  pitch / prepared-cache pipeline without breaking it (addressed in §5).
- **Quality varies** on dense or unusual material; some bleed is inherent to source
  separation.
- **Build complexity.** A Python export step (Phase 1) is added to produce the ONNX
  models, plus per-format (VST3/AU/Windows) bundling.

---

## 5. Architecture & integration design

### 5.1 Inference core — `StemSeparator` (mirror `BeatThisAnalyzer`)
New `StemSeparator.h/.cpp`. Same shape as the existing analyzer:
- Constructed with the resolved model path(s); holds `Ort::Env / SessionOptions /
  Session` (one session per FT sub-model); `isReady()`.
- `separate(buffer, sampleRate, progressFn) -> StemResult` runs entirely on a
  background thread. Internally: resample to **44100 Hz stereo**, segment (~7.8 s with
  0.25 overlap), compute **STFT in C++** (n_fft 4096 / hop 1024), run each FT model,
  inverse-STFT, overlap-add, resample stems back to the source rate.
- **Output mapping** (verify against the exported graph): standard Demucs source order
  is `[drums, bass, other, vocals]`; the FT bag runs each specialized model and keeps
  its specialty stem.

### 5.2 Storage & the muting model — **subtraction, not 4 full stems**
We only ever mute drums/bass/vocals, so we **store just those three stems** plus the
original buffer that already exists. Then:

```
activeMix = original − Σ(muted stems)        // muted ⊆ {drums, bass, vocals}
```

Why this is the right design:
- **Nothing muted ⇒ activeMix == original, bit-for-bit.** Zero separation artifacts
  in the default state.
- "Other always plays" falls out for free — `other` is just whatever isn't subtracted.
- Memory is `original + 3 stems` (4× source) instead of 5×, and one model's worth of
  output (`other`) never needs storing.

A `StemSet` (shared, owns the 3 stem buffers) is referenced by `LoadedSampleData`.

### 5.3 Auto-on-load hook (mirror `launchTempoAnalysis`)
Add `launchStemSeparation(sampleData)`, called right after `launchTempoAnalysis` in
`loadAudioFile` (≈ PluginProcessor.cpp:3695) and the restore/reload paths
(≈ 3512, 5008, 5021). It uses a dedicated single-thread `juce::ThreadPool`, a
generation counter, and a generation-guarded `publishStems()` that `atomic_store`s the
`StemSet` into the live sample — exactly the pattern `publishTempoAnalysis` already
uses. Separation runs **below** the realtime audio thread priority, so it cannot glitch
playback.

### 5.4 Applying a mute — instant, reuses the existing swap
Three atomic flags: `muteDrums / muteBass / muteVocals`. On toggle, rebuild
`activeMix` (an O(N) subtraction on a background thread), wrap it in a new
`LoadedSampleData` that **shares the same `StemSet`**, and `atomic_store(&loadedSample,
…)` — the identical mechanism the code already uses to swap samples. Because every
downstream reader (voices, warp bake, prepared-cache) reads `loadedSample->buffer`,
they automatically play the muted mix with **no changes to the audio/DSP path**.
Warp/prepared caches are refreshed via the existing async generation mechanism
(`warpRenderGeneration`, `prepareWarmGeneration`).

### 5.5 UI — condense waveform, add `StemRackComponent`
Mirror `EffectsRackComponent`'s skeuomorphic styling. Three toggle buttons
(BASS / DRUMS / VOCALS) plus a small status line that shows "Separating…" / progress
while the job runs and enables the buttons when stems are ready. Exact geometry in §6.

### 5.6 Build & bundling
Mirror the `beat_this.onnx` recipe in `CMakeLists.txt`: copy the FT model files (and
any `.onnx.data` external-weight sidecars) into `Contents/Resources/` for VST3 & AU
and next to the binary for Windows. Models are **too large for `juce_add_binary_data`**
— use the resource-copy path, like the beat model already does.

### 5.7 Persistence & failure
- Persist the three mute flags in `getStateInformation` / `setStateInformation`.
- On reload, **re-run separation** rather than serializing ~hundreds of MB of stems
  into the plugin state (note this as a deliberate tradeoff; revisit if undesirable).
- If a model is missing or inference fails, log and **fall back to the original
  buffer** with mutes disabled — the sampler keeps working exactly as today.

---

## 6. UI layout math (exact, from current `resized()`)

Current left column (PluginEditor.cpp ≈ 6574–6575):

```
waveformDisplayComponent : (96, 133, 782, 411)
transportSectionComponent: (96, 552, 782, 236)
```

Proposed (insert a 73 px stem strip; transport unchanged):

```
waveformDisplayComponent : (96, 133, 782, 330)   // 411 → 330
stemRackComponent        : (96, 471, 782,  73)   // NEW  (471 = 133+330+8)
transportSectionComponent: (96, 552, 782, 236)   // unchanged (471+73+8 = 552)
helpOverlayComponent     : (96, 133, 782, 655)    // unchanged (overlays whole column)
```

8 px gaps match the existing rhythm. `utilityStripComponent` and
`effectsRackComponent` (right side) are untouched.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| **RAM for long samples** (3 st*stereo* stem buffers at source rate ≈ hundreds of MB for a full song) | Store only 3 stems (subtraction model, §5.2); document a max-length guard; option to free stems when the panel is unused |
| **Separation time** (FT bag is slow) | Background thread + progress UI; CoreML execution provider on macOS with CPU fallback; offer base HTDemucs as a build-time switch |
| **Stem ↔ chop/warp/pitch interaction** | Mute swaps `loadedSample->buffer` and refreshes existing caches via existing generation counters; no DSP-path rewrite |
| **ONNX export blockers** | Use `demucs-onnx` which already solves them + parity-checks vs PyTorch |
| **Output stem ordering wrong** | Phase 2 acceptance test asserts `Σ stems ≈ original` and that each stem is plausibly its instrument |
| **Bundle size** | Accepted per max-quality choice; document; base-model fallback available |
| **Model not found / inference failure** | Graceful fallback to original buffer, mutes disabled, logged |

---

## 8. Phase map (full prompts in `STEM_SEPARATION_CC_PROMPTS.md`)

0. **Context block** — shared preamble pasted atop every phase.
1. **Model export + bundling** — Python export of htdemucs_ft → ONNX; CMake copies models into Resources/.
2. **`StemSeparator` inference core** — C++ ORT class + offline test in `tools/`.
3. **Processor integration** — `StemSet` storage, auto-on-load job, mute flags + remix swap, state persistence.
4. **UI** — condense waveform, `StemRackComponent` with 3 mute buttons, progress state.
5. **Polish & verification** — fallback, edge cases, acceptance matrix, manual test script.
