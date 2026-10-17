# BPM analysis integration

CueSampler analyzes loaded audio in `TempoAnalysisJob`, outside the audio callback.
The fast classical detector checks representative windows and half/double-tempo
hypotheses. Beat This! checks uncertain results and provides beat/downbeat timing.
All inference, resampling, feature extraction, and result construction remain on
the analysis worker; the existing generation guard publishes the result.

## Reference behavior

Serato is a behavioral reference, not an implementation dependency. Its public
documentation does not disclose the detector's model or signal processing.

- [Analysis settings](https://support.serato.com/hc/en-us/articles/14361435885327-Analysis-setting)
  describes BPM ranges as a way to choose between half/double interpretations.
- [Serato DJ 5.0 beta Lucid announcement](https://the-drop.serato.com/announcements/lucid-beatgrids-just-dropped-in-the-serato-dj-5-0-beta/)
  describes separate fixed and flexible analysis, with bar/beat tempo-change
  resolution. This announcement concerns DJ, not Serato Sample's detector.
- [Serato Sample beatgrids](https://support.serato.com/hc/en-us/articles/360002025835-Beatgrids)
  documents manual BPM, half/double, grid slip, and local tempo markers.

CueSampler's current chop/grid UI still uses one fixed BPM and four beats per bar.
Preserving detected beat times and identifying drift does not implement flexible
tempo maps, odd-meter chopping, or automatic time warping.

## Beat This! compatibility

The frontend follows the official
[preprocessing](https://github.com/CPJKU/beat_this/blob/main/beat_this/preprocessing.py):
22,050 Hz mono, a 1,024-point periodic Hann window, 441-sample hop, centered reflect
padding, magnitude normalization by sqrt(1,024), and 128 continuous Slaney mel
filters from 30 to 11,000 Hz, followed by log1p(1,000 * magnitude).
Very short clips use repeated reflection rather than failing the padding step.

Offline sample-rate conversion uses an anti-aliased windowed-sinc resampler with
no extra runtime library. It is not bit-identical to the reference SoXR resampler.

Inference follows the reference
[Spect2Frames](https://github.com/CPJKU/beat_this/blob/main/beat_this/inference.py):
1,500-frame chunks, six frames of zero-padded border context, discarded output
borders, and first-prediction retention in overlaps. The output length remains
exactly equal to the spectrogram length.

Decoding uses the reference minimal peak-picking approach (seven-frame local
maxima above 0.5 probability). The displayed BPM summarizes observed beat
intervals without a hidden 70–160 BPM search or a four-beat downbeat divisor.
Missing beats are not synthesized. Genuine downbeats remain independent of the
fixed four-beat UI. Confidence discounts short evidence and unusably close peaks.

The classical search covers 40–240 BPM. Only strong, steady results with direct
support for the final tempo family across the chosen windows skip neural analysis.
Selection uses confidence and evidence duration; more beat/bar markers or less
expressive timing do not automatically improve a candidate's score. A clearly
stronger neural result may replace the classical result; closely agreeing results
can refine timing while retaining the classical BPM. If the detectors agree only
at half/double tempo, preserve the classical metrical interpretation and beat
markers while using neural confidence/downbeats as corroboration. Saturated
neural probabilities alone cannot decide which metrical level a listener wants.

## Validation

Configure and run without installing plugin bundles:

```sh
cmake -S . -B build -DCUE_COPY_PLUGIN_AFTER_BUILD=OFF -DCUE_BUILD_TEMPO_TEST=ON
cmake --build build --target test_tempo_analysis
ctest --test-dir build --output-on-failure -R '^tempo_analysis$'
./build/test_tempo_analysis_artefacts/Release/test_tempo_analysis --model-check assets/beat_this.onnx
```

The model-free regressions cover 48–230 BPM, swung subdivisions, 3/4, compound and
odd beat groupings, missing beats, crop offsets, tempo changes, silence, short and
non-finite inputs, false-peak confidence, classical/neural selection, independent
torchaudio feature values, and resampling passband/alias rejection. The optional
model check verifies exact output length and finite probabilities around chunk
boundaries. `--analyze audio.wav [model]` prints a real inference result and beat
times; `--features audio.wav output.txt` exports the frontend for comparison.

Manual listening should include steady electronic loops, swung jazz, shuffled
blues, loose funk/soul, sparse acoustic music, and changing-tempo performances.
Compare BPM level, first beat/downbeat, accumulated fixed-grid drift, and analysis
time separately. Half/double interpretations and short ambiguous samples remain
possible; the existing BPM correction controls remain useful.

Synthetic and numerical regressions validate integration correctness. They do not
establish an accuracy percentage across genres. Historical BPM corrections also
need fresh listening/annotation before being treated as benchmark ground truth.
