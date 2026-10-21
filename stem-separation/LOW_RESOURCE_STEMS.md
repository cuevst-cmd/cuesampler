# Stem separation on Intel and Apple Silicon Macs

The separator keeps the base HTDemucs network and 10% overlap. CPU inference is
the default; CoreML remains opt-in. The audio callback does not load models,
allocate inference buffers, or wait for separation.

## Compact model

The published fixed-shape graph exports the iSTFT window-normalization loop as
more than 21,000 nodes. Its output is constant: it depends on the fixed segment
length and Hann window, never on audio. Precomputing that curve removes the
load-time graph-expansion cost without changing the trained network, its dense
STFT/iSTFT transforms, or its waveform input/output.

Generate the compact model from the verified original:

```sh
.venv/bin/python tools/optimize_stem_model.py \
  assets/htdemucs/htdemucs.onnx assets/htdemucs/htdemucs-compact.onnx
cmake -S . -B build
cmake --build build --target CueSampler_Standalone CueSampler_AU CueSampler_VST3 -j 4
```

The optimizer requires NumPy and ONNX. It accepts only the pinned source model
SHA-256 and refuses unsupported exports. The original is preserved for A/B
validation. Both model files are ignored build assets; generate the compact file
on each Mac release machine before configuring CMake. macOS CMake prefers it when present
and stages it under the existing `htdemucs/htdemucs.onnx` bundle name. Without it,
the original remains the fallback. Development-tree lookup and the offline test
also prefer the compact model. The configure log prints the selected model.
Windows packaging continues to use its pinned original model until its compact
graph has been validated on Windows and added to the installer integrity gate.

## CPU and buffer work

- Inference defaults to at most four intra-op threads and half the logical CPUs.
  On Apple Silicon the budget also stays within the reported performance-core
  count. A two-logical-CPU Intel Mac uses one inference thread. An unknown CPU
  count safely falls back to one.
- ORT worker spinning is disabled on macOS as well as Windows. This avoids busy
  waiting between operators; it does not change model precision. See the
  [ORT threading documentation](https://onnxruntime.ai/docs/performance/tune-performance/threading.html).
- On macOS, memory patterns are disabled while CPU arena reuse stays enabled.
  With ORT 1.18.1, the packed memory-pattern buffer otherwise raises RAM on the
  second window while earlier arena allocations remain resident.
- Compatible 44.1 kHz stereo input is borrowed, and matching output stems are
  moved into the result. Unused channels are dropped before resampling.
- Cache recall reuses the validated encoded stem container. It no longer runs
  another MP3 encode just to prepare portable project state.

`CUE_STEM_THREADS=1`, `2`, etc. remains a support/benchmark override, bounded to
32. The default budget applies per plugin instance. Multiple instances still
have separate sessions, and simultaneous separation increases total CPU/RAM.

## Validation and limits

Measured on the development arm64 Mac using the bundled macOS ORT 1.18.1, four
intra-op threads, CPU only. Synthetic fixtures establish numerical parity and
resource costs; they are not Intel/M1/M2 hardware or listening benchmarks.

Final C++ pipeline comparison, 15-second stereo/44.1 kHz input, three windows:

| Metric | Before | Final compact model + runtime policy |
|---|---:|---:|
| Session load | 1.59 s | 0.30 s |
| Separation wall time | 7.31 s | 6.22 s |
| Separation process CPU time | 24.44 s | 19.42 s |
| Peak process RSS including load | 4,430.5 MiB | 2,791.5 MiB |

This run used 21% less CPU time and 37% less peak RAM, with about 15% less
separation wall time. All three final float WAV stems were bit-identical to the
original build. Mono/48 kHz and four-channel/96 kHz comparisons were also exact.
Results are individual runs; CPU seconds are summed across threads, not elapsed
seconds or a battery/energy measurement.

| Controlled single-window native test | Original graph | Compact graph |
|---|---:|---:|
| Session load | 1.472 s | 0.300 s |
| Peak RSS through load | 4,644.8 MiB | 881.1 MiB |
| Peak RSS through inference | 4,644.8 MiB | 2,771.1 MiB |
| Inference wall time | 2.095 s | 2.101 s |

All 2,751,840 float outputs in that comparison were bit-identical. Compaction
reduces initialization time and peak memory; it does not reduce the neural
network's activation cost or claim a sustained inference throughput gain.

Three consecutive compact-model windows with the default memory pattern raised
peak/current RSS from 2,800 MiB after window one to 4,842 MiB after window two.
Disabling memory patterns kept all three windows at 2,782 MiB (43% less peak
RAM), with 6.301 vs 6.239 seconds total wall time and 19.578 vs 19.609 seconds
CPU time. Every output remained bit-identical. The CPU arena remains enabled to
reuse buffers across windows.

In a separate 15-second stereo/three-window C++ test using the original model,
the thread/copy changes reduced separation wall time from 7.31 to 6.33 seconds
and process CPU time from 24.44 to 19.69 seconds. All three returned stems were
bit-identical. These are individual runs, not a broad performance guarantee.

The offline harness reports session/separation CPU time and process peak RSS on
POSIX systems, and can write lossless float WAV stems for listening or numerical
comparison:

```sh
cmake -S . -B build -DCUE_BUILD_STEM_TEST=ON -DCUE_BUILD_STEM_CACHE_TEST=ON -DCUE_BUILD_STATE_TEST=ON
cmake --build build --target test_stem_separator test_stem_cache test_project_restore -j 4
.venv/bin/python -m unittest discover -s tools -p test_optimize_stem_model.py
build/test_stem_separator_artefacts/Release/test_stem_separator \
  /absolute/path/music.wav assets/htdemucs/htdemucs.onnx /tmp/stems-original
build/test_stem_separator_artefacts/Release/test_stem_separator \
  /absolute/path/music.wav assets/htdemucs/htdemucs-compact.onnx /tmp/stems-compact
ctest --test-dir build -R '^(stem_cache|stem_state|mp3_exports)$' --output-on-failure
build/test_project_restore_artefacts/Release/test_project_restore /tmp --stem-separation
```

For release acceptance, repeat on an Intel Mac and an M1/M2 with representative
music, a full-length song, and active DAW playback at a low audio-buffer size.
Check stem boundaries, mute/remix behavior, project recall and UI response.
Even the compact graph still needs substantial activation memory while running.

Completed checks for this change: nine optimizer unit tests; `stem_cache`,
`stem_state`, and `mp3_exports` CTest targets; focused `--stem-separation` real
inference/save/recall/mute integration (maximum UI dispatch gap 24.3 ms); and
x86_64 cache and real-separation checks under Rosetta. Universal arm64+x86_64 AU,
VST3 and standalone builds completed, passed strict/deep signature verification,
and bundled the compact model with SHA-256
`c5ba957ab054c6f2940663dd0b1efc1cf1b63e2dee902b361cb982743185d7d5`.

The broader `project_restore` UI suite still aborts in its unrelated export-drag
test: JUCE `AlertWindow::updateLayout()` calls `Component::getParentMonitorArea()`
with no display returned in this environment. The dedicated stem-state and real
separation paths pass; the broad UI suite is not claimed as passing.

A prototype replacing the dense transforms with native ONNX FFT operations
reduced file size/CPU further but increased wall time on the pinned runtime. It
is not part of the production graph.
