# Stem Separation — Speed Optimization Plan (SAMPLERv3 / CueSampler)

**Owner:** Jerry · **Date:** 2026-06-19 · **Status:** Draft for review

This is the follow-on to `STEM_SEPARATION_PLAN.md`. That doc got stem separation
*working* (base HTDemucs → ONNX → bundled ONNX Runtime). This doc is about making
it **fast** across **macOS (Apple Silicon + Intel) and Windows**, accepting **minor
quality loss for major speed** where the trade is worth it.

The headline conclusion, up front, because it changes the shape of the work:

> **Don't hand-write a bespoke Demucs engine.** Profile first, then keep ONNX
> Runtime as the portable math core and attack speed in layers. The single biggest
> win — running on the **GPU** — is roughly **10×** and is nearly free on Windows
> (DirectML execution provider). On Mac it's the hard part, because ORT's only
> non-CPU path is CoreML, which currently chokes on this model's tensor sizes. That
> Mac-GPU problem — not the whole architecture — is the one place a *targeted*
> custom path (MLX/Metal) may eventually earn its keep.

---

## 1. Current baseline (measured, from the repo)

| Fact | Value | Source in repo |
|---|---|---|
| Model | Base `htdemucs` (single model, all 4 stems in one pass) | `export_htdemucs.py`, `StemSeparator.h` |
| Runtime | ONNX Runtime **1.20.1**, universal2 (Mac) / win-x64 | `CMakeLists.txt`, `libs/onnxruntime/VERSION_NUMBER` |
| Graph I/O | waveform → waveform; **STFT/iSTFT baked in** as Conv1d/ConvTranspose1d (`RealSTFT`, n_fft 4096, hop 1024, Hann, normalized) | `StemSeparator.h` header comment, `export_htdemucs.py` |
| Input / output | `[1, 2, S]` → `[1, 4, 2, S]`, S = 343980 (7.8 s @ 44.1 kHz) | `export_htdemucs.py` |
| Compute | **CPU only**. CoreML disabled — it produced NaNs and ran ~25× slower (561 s vs 22 s on an 18 s clip) because full-segment tensors exceed CoreML's 16384-dim cap and force ~106 graph partitions | `StemSeparator.cpp` lines ~100–128 |
| Speed | ~**0.8× realtime** on Apple Silicon CPU (≈1.25 s of compute per 1 s of audio) | `StemSeparator.cpp` comment |
| Overlap | already lowered 0.25 → **0.10** to cut segment count ~17% | `StemSeparator.h` `kOverlap` |
| Prior change | dropped the `htdemucs_ft` 4-model bag (~0.2× RT) for single-pass base (~3× faster) | `export_htdemucs.py` comment |

So the cheap wins **inside the current design** are already taken. Going faster means
changing **where the math runs** (hardware path) and **how precise it is**
(quantization) — not rewriting the model.

### What "slow" costs today
At ~0.8× realtime, a 3.5-minute song ≈ **~4–4.5 minutes** of separation on load.
That's the pain we're cutting.

---

## 2. Where the time actually goes (and why GPU is the prize)

The Mixxx project (GSoC 2025) benchmarked **this same model** (HTDemucs → ONNX → C++
ORT) on 50 MusDB tracks. Their numbers are the best external anchor we have:

| Path | Time per 1 min of audio | Notes |
|---|---|---|
| PyTorch, CPU | 25.89 s | reference |
| **C++ ONNX Runtime, CPU** | **21.24 s** | 17.9% *faster* than PyTorch CPU |
| PyTorch, GPU (CUDA) | 1.70 s | |
| **ONNX Runtime, GPU** | **1.86 s** | only 8.4% slower than hand-tuned CUDA |

Two conclusions that steer everything below:

1. **GPU ≈ 11× faster than CPU** for this model (21.24 → 1.86 s/min). This is the
   dominant lever by an order of magnitude. Quantization, threading, and STFT tricks
   are all single-digit-to-2× wins; the GPU is ~10×.
2. **ORT on GPU is within ~8% of hand-written PyTorch CUDA.** A from-scratch C++
   engine has almost no ceiling to beat here — the heavy ops (conv, matmul,
   attention) are already near-optimal in ORT/MLAS/cuDNN/DirectML. Confirmed from the
   other direction by `sevagh/demucs.cpp`, a real C++17 Demucs v4 port, which states
   it **sacrifices speed** for low memory. So "bespoke for speed" is the wrong bet on
   any platform where ORT can already reach the GPU.

Quality cost of the ONNX path is negligible: Mixxx measured **< 0.1 dB SI-SDR**
difference vs PyTorch across all stems (overall 7.44 → 7.43 dB).

---

## 3. The cross-platform catch: Mac and Windows accelerate differently

This is the crux. ORT reaches the GPU through **execution providers (EPs)**, and the
available EPs are *not* symmetric across our targets:

| Platform | CPU EP | GPU / accelerator EP in ORT | Reality for us |
|---|---|---|---|
| **Windows** | MLAS (AVX2/AVX-512) | **DirectML** (any DX12 GPU: NVIDIA/AMD/Intel/Qualcomm) + **CUDA** (NVIDIA) | GPU win (~10×) is **available and portable** across GPU vendors. DirectML supports opset ≤ 20; our model is opset 17 ✓. |
| **macOS** (AS + Intel) | MLAS (NEON / AVX) | **CoreML only** (ANE/GPU/CPU) — no Metal/MPS EP exists in ORT | CoreML currently **fails** on this model (16384-dim cap → partition storm → NaN + 25× slower). So today, **Mac = CPU-only via ORT.** |

The uncomfortable truth: the GPU win is **easy on Windows and hard on the Mac you
actually develop on.** The plan treats those as two separate tracks.

Why CoreML chokes: the raw-waveform segment is a single tensor dimension of **343980
samples** — far past CoreML's 16384 limit — and the in-graph STFT conv produces more
oversized tensors. CoreML can't place those, so ORT shatters the graph into ~100
CoreML/CPU partitions. Fixing this means **shrinking the tensors CoreML sees**
(Section 5) and/or going outside ORT on Mac (Section 6).

---

## 4. Optimization levers, ranked by payoff ÷ effort

| # | Lever | Est. speedup | Quality cost | Effort | Platforms | Verdict |
|---|---|---|---|---|---|---|
| 1 | **Profile first** (ORT per-op JSON, per-segment timing) | — (informs all) | none | XS | all | **Do first.** Confirms the hotspot on *your* hardware before spending effort. |
| 2 | **Windows DirectML EP** | **~8–11×** | none | S–M | Win | **Biggest win, lowest effort.** Bundle `onnxruntime-directml`, add EP, fall back to CPU. |
| 3 | **Thread / segment / overlap tuning** | 1.2–2× | none–tiny | S | all | Your 0.8× vs Mixxx's ~3×-faster CPU suggests real headroom (core under/over-subscription, graph-opt, batch). |
| 4 | **fp16 model variant** | ~free on CPU*; enables fp16 on GPU/ANE | ~none (max abs diff 6e-5, SDR unchanged) | S | all | StemSplit publishes an fp16 htdemucs with verified parity. *Storage/precision win; CPU compute speedup is small — the payoff is on GPU/ANE.* |
| 5 | **Move STFT/iSTFT out of the graph** (real FFT in C++) | 1.1–1.4× CPU + shrinks tensors | none if parity-checked | M | all | Replaces a big dense conv with an O(N log N) FFT (`juce::dsp::FFT` → vDSP on Apple). Also a **prerequisite** for making CoreML viable. Needs a re-export (sevagh-style) + exact parity. |
| 6 | **CoreML MLProgram re-export (Mac ANE/GPU)** | potentially large | measure | M–L | Mac | Retry CoreML the *right* way: MLProgram format (no silent fp16 cast), `MLComputeUnits`, **after** STFT is out and tensors are shrunk/segmented under 16384. Measure whether the time-domain branch still partitions. |
| 7 | **int8 quantization** | uncertain (sometimes *slower*) | **risky** on the transformer branch | M | all | Static int8 badly hurt Whisper-class transformer audio models; conv branches tolerate it. **Measure, apply selectively or skip.** Not a safe "max speed" default. |
| 8 | **Mac GPU via MLX / Metal** (targeted custom path) | ~10× on Mac | none–tiny | **L** | Mac (AS) | The *only* clear route to a Mac GPU win if CoreML won't cooperate. This is where a "customized C++ implementation" pays off — but scoped to Mac GPU, not a full rewrite. |
| 9 | **Lighter / distilled model** | model-dependent | quality cost | L | all | Last resort for "max speed": a smaller separation net or fewer transformer layers. Revisit only if 1–8 fall short. |
| 10 | **Intel OpenVINO path** (Intel CPU/GPU/NPU) | hardware-dependent | ~none | M | Win + Intel Mac | `Intel/demucs-openvino` exists. Optional second runtime for Intel silicon; adds bundling complexity. |

\* "fp16 weights" models decompress to fp32 at runtime on CPU, so they shrink the
download but don't speed up CPU math. True fp16 *compute* speedups come from GPU/ANE
EPs that execute in half precision.

---

## 5. What changes in *our* code

The good news: this layers onto the existing `StemSeparator` cleanly — same
background-job shape, same bundling recipe.

### 5.1 `StemSeparator` — execution-provider selection
`loadModel()` already tries CoreML then falls back to CPU. Generalize that into an
ordered EP list chosen per platform at runtime, each with graceful fallback:

```
Windows : [ DirectML(deviceId) ] → [ CUDA ] → [ CPU ]
macOS   : [ CoreML(MLProgram, ComputeUnits=All) ] → [ CPU ]    // once §5.3 lands
both    : env override (CUE_STEM_EP=cpu|dml|cuda|coreml) for A/B + support
```

Keep the existing "session failed to build → retry next EP" pattern; just widen it
and log which EP actually bound. Add a one-line timing log per separation
(wall-clock + s/min-of-audio) so we always have a field metric.

### 5.2 fp16 model variants in the bundle
Mirror the current `htdemucs.onnx` copy step in `CMakeLists.txt`. Ship an fp16 model
for the GPU/ANE EPs (the storage cost is ~half). Keep an fp32 CPU model as the
universal fallback. A build flag selects which variants are bundled per format
(VST3/AU/Windows). Re-use the existing POST_BUILD copy-into-Resources/ machinery and
the codesign-ordering note already documented in CMake.

### 5.3 STFT/iSTFT out of the graph (the re-export)
Today's graph is end-to-end waveform→waveform (`demucs-onnx` bakes STFT in). To move
it out we need the **other** export style (`sevagh/demucs.onnx`): a graph whose
spectral branch takes a spectrogram and returns one, with STFT/iSTFT done in C++.

- **C++ FFT:** use `juce::dsp::FFT` (already a dependency; wraps vDSP/Accelerate on
  Apple, has a portable path on Windows). No new library.
- **Exact parity required:** n_fft 4096, hop 1024, **Hann**, `normalized=True`,
  matching center/padding and the real/imag packing the export expects. Build a
  numpy/PyTorch-vs-C++ parity test (max-abs-err < 1e-4) like the export scripts
  already do for the model.
- **Payoff:** ~10–40% CPU + materially smaller tensors → the thing that makes the
  CoreML retry (§5.4) worth attempting.

### 5.4 Mac GPU track (only if profiling says CPU isn't enough)
1. **CoreML MLProgram retry** with STFT out and segments restructured so no tensor
   dim exceeds 16384. Measure partition count and NaN-freedom. If it places cleanly
   on ANE/GPU, this is the cheap Mac win.
2. **If CoreML still won't cooperate:** scope an **MLX** (Apple's array framework,
   Mac-GPU, has a C++ API) or Metal/MPS implementation of the conv+transformer core,
   Mac-only, sitting behind the same `StemSeparator` interface. This is the *targeted*
   bespoke effort — not a cross-platform rewrite.

### 5.5 Everything downstream is unchanged
The subtraction muting model, `StemSet` storage, auto-on-load job, and UI from
`STEM_SEPARATION_PLAN.md` don't change. We're swapping the engine under
`separate()`, not the integration.

---

## 6. Why *not* a full bespoke C++ engine (since it was the original idea)

Stating this plainly because it was the starting instinct:

- **No speed ceiling to gain.** ORT-on-GPU is within ~8% of hand-tuned CUDA; ORT CPU
  beats PyTorch CPU. The heavy kernels are already optimal.
- **The real bottleneck is hardware access, not kernel quality.** A bespoke engine
  still has to reach the GPU — meaning you'd hand-write **Metal _and_ DirectX/CUDA
  _and_ CPU SIMD** kernels and keep them at PyTorch parity. That's most of ORT's job,
  re-done, ×3 platforms.
- **Prior art confirms the trap:** `demucs.cpp` is a complete, correct C++17 Demucs v4
  port — and it's *slower* than Torch by design.
- **Where custom code _is_ justified:** the C++ STFT/iSTFT (§5.3) and, if needed, a
  **Mac-only** MLX/Metal core for the one platform where ORT can't reach the GPU
  (§5.4). Both are scoped, both hide behind the existing interface.

---

## 7. Profiling methodology (Phase 0 — do this before writing optimization code)

1. **Enable ORT profiling:** `SessionOptions::EnableProfiling()` → emits a per-op JSON
   timeline. Aggregate by op type to see the conv / matmul / attention / STFT-conv
   split on *your* Mac.
2. **Per-segment wall-clock** already partly exists; add total + s/min-of-audio.
3. **Sweep the cheap knobs** and record a table: intra-op threads {½, all cores},
   graph-opt level, overlap {0.10, 0.05, 0.0}, segment length.
4. **A/B harness** in `tools/`: same input file, swap EP via `CUE_STEM_EP`, log time +
   write stems, compute SI-SDR vs the fp32/CPU reference. This becomes the regression
   gate for every later change.
5. Capture the machine (chip, core counts) next to every number — Apple Silicon
   generation matters a lot.

**Decision gate:** the Phase-0 table tells us whether tuning+STFT-out (Section 4 #3,5)
already hits the target on CPU, or whether we must chase the GPU on Mac (#6/#8).

---

## 8. Phased roadmap

| Phase | Goal | Key deliverables | Exit criterion |
|---|---|---|---|
| **0. Profile** | Know the real hotspot on your hardware | ORT profiling JSON, knob-sweep table, `tools/` A/B + SI-SDR harness | A baseline table + a target number (e.g. "≤ 30 s for a 3.5-min song") |
| **1. CPU wins (all platforms)** | Bank the free, portable gains | EP-selection refactor, thread/overlap/graph-opt tuning, timing log | Measurable CPU speedup, **zero** quality regression vs reference |
| **2. fp16 + STFT-out** | Precision + tensor-size prep | fp16 bundle variant; C++ `juce::dsp::FFT` STFT/iSTFT with parity test; re-export (sevagh-style) | Parity < 1e-4; CPU faster; tensors shrunk |
| **3. Windows GPU** | The ~10× win where it's easy | DirectML EP wired + bundled (`onnxruntime-directml`), CPU fallback | ~5–10× on a DX12 test box; SI-SDR within tolerance |
| **4. Mac GPU** | Close the gap on the primary platform | CoreML MLProgram retry **→** (fallback) scoped MLX/Metal core | Either a clean CoreML placement or an MLX path beating tuned CPU ≥ 3× |
| **5. Verify & ship** | Trust + safety | Parity matrix (SI-SDR, Σstems≈original), per-EP fallback tests, max-length guard, docs | All EPs fall back safely; quality within agreed tolerance; timings documented |

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| **CoreML still won't place the model** (Mac GPU blocked) | Accept tuned-CPU on Mac for v1; pursue MLX/Metal (§5.4) as a separate, optional track. Windows still gets its GPU win. |
| **DirectML is "sustained engineering"** (MS now steers to Windows ML) | DirectML EP still ships in ORT and supports opset ≤20 (we're 17). Note Windows ML / WinML as the forward path; revisit if we bump ORT. |
| **int8 hurts quality** (hybrid transformer is sensitive) | Don't make int8 a default. Gate behind the SI-SDR harness; apply selectively to conv layers only, or skip. fp16 is the safe "max speed" precision lever. |
| **STFT-out parity drift** (clicks/phase artifacts) | Dedicated parity test vs PyTorch (< 1e-4) before it touches audio; keep the in-graph model as fallback. |
| **Bundle size** (extra fp16 + per-platform models) | Bundle only the variants each platform uses via build flags; fp16 is ~half size. |
| **GPU contention with the host DAW** | It's a brief one-shot pass on load, off the realtime thread; expose a CPU-only preference for users who want zero GPU use. |
| **ORT version drift** (1.20.1 → newer for better EPs) | Treat an ORT bump as its own small phase; re-run the parity + timing harness as the gate. |

---

## 10. Acceptance tests

- **Quality:** per-stem SI-SDR within an agreed tolerance of the fp32/CPU reference
  (Mixxx saw < 0.1 dB ONNX-vs-PyTorch; fp16 added ~6e-5 abs). Pick a budget, e.g.
  "≤ 0.2 dB overall regression."
- **Correctness:** Σ(drums+bass+other+vocals) ≈ original within float tolerance; each
  stem is plausibly its instrument.
- **Fallback:** every EP, when forced to fail, falls back to CPU and still produces
  valid stems (extend the existing missing-model fallback).
- **Speed:** record s/min-of-audio per EP per machine; compare to the Phase-0 target.

---

## 11. Open questions for you

1. **Target number?** What's "fast enough" — e.g. *≤ 30 s for a 3.5-min song*, or
   *near-instant*? It decides whether tuned-CPU suffices or we must chase Mac GPU.
2. **Which Mac are you on?** Apple Silicon generation (M1/M2/M3/M4, base vs Pro/Max)
   sets the realistic CPU ceiling and whether MLX is worth it.
3. **Is brief GPU use on load acceptable** in a plugin context, or do you want a
   CPU-only default with GPU opt-in?
4. **Windows GPU spread** — are your Windows users likely on discrete GPUs (DirectML
   shines) or integrated only (smaller win)?

---

## 12. Sources

- [Mixxx GSoC 2025 — Demucs v4 → ONNX (benchmarks: CPU/GPU timing + SI-SDR)](https://mixxx.org/news/2025-10-27-gsoc2025-demucs-to-onnx-dhunstack/)
- [StemSplit `demucs-onnx` (current export; STFT in-graph)](https://github.com/StemSplit/demucs-onnx)
- [StemSplitio/htdemucs-ft-onnx (fp16 variant: ~6e-5 abs diff, SDR unchanged)](https://huggingface.co/StemSplitio/htdemucs-ft-onnx)
- [sevagh/demucs.onnx (STFT-outside-the-graph C++ reference)](https://github.com/sevagh/demucs.onnx)
- [sevagh/demucs.cpp (C++17 Demucs v4; trades speed for low memory)](https://github.com/sevagh/demucs.cpp)
- [ONNX Runtime — DirectML execution provider](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html)
- [ONNX Runtime — CoreML execution provider (MLProgram, MLComputeUnits)](https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html)
- [ONNX Runtime — quantization guide](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)
- [Intel/demucs-openvino (Intel CPU/GPU/NPU path)](https://huggingface.co/Intel/demucs-openvino)
- [Défossez, "Hybrid Transformers for Music Source Separation" (arXiv:2211.08553)](https://arxiv.org/abs/2211.08553)
