#!/usr/bin/env python3
"""
export_htdemucs.py
------------------
Exports the HTDemucs-FT (htdemucs_ft) source-separation model to ONNX for use in
the CUE SAMPLER JUCE plugin's StemSeparator (Phase 2).

Mirrors export_beat_this.py. Uses the `demucs-onnx` package (StemSplit), which
solves the four htdemucs ONNX-export blockers (complex STFT tensors,
fractions.Fraction in model.segment, random.randrange in the cross-transformer,
and the fused aten::_native_multi_head_attention kernel).

────────────────────────────────────────────────────────────────────────────────
IMPORTANT — STFT lives INSIDE the graph (waveform -> waveform)
────────────────────────────────────────────────────────────────────────────────
The Phase 0/1 brief referenced the *sevagh/demucs.onnx* approach of keeping
STFT/iSTFT OUTSIDE the graph and feeding spectrograms. The `demucs-onnx` package
we were told to use does the OPPOSITE, and the two are mutually exclusive: it
solves the "complex STFT tensor" export blocker by replacing torch.stft/istft
with in-graph Conv1d/ConvTranspose1d layers over precomputed sin/cos DFT bases
(`RealSTFT`, n_fft=4096, hop=1024, hann, normalized=True). The exported graph is
therefore END-TO-END: it takes raw stereo audio and returns separated stereo
audio. Phase 2's C++ does NOT compute any STFT — it just feeds samples in and
reads samples out. (Confirmed against the package README; see the spec printed at
the end of this run.)

────────────────────────────────────────────────────────────────────────────────
Run once, in a venv with the export extras (this repo's normal .venv has no torch):
    python3 -m venv .venv && source .venv/bin/activate
    pip install "demucs-onnx[export]"
    .venv/bin/python export_htdemucs.py

(Inference-only `pip install demucs-onnx` is ~50 MB and CANNOT export — the
[export] extra pulls in torch + the official demucs checkpoints.)

Outputs (each ~316 MB, opset 17, weights embedded — NO .onnx.data sidecar):
    assets/htdemucs_ft/drums.onnx
    assets/htdemucs_ft/bass.onnx
    assets/htdemucs_ft/vocals.onnx
    assets/htdemucs_ft/other.onnx     ← exported for parity/completeness only; the
                                        plugin's subtraction model does NOT bundle
                                        or run it ("other" = original − drums −
                                        bass − vocals). See CMakeLists.txt.

Total on disk: 4 files ≈ 1.26 GB exported; 3 bundled (drums/bass/vocals) ≈ 948 MB.

NO-DOWNLOAD ALTERNATIVE: the identical parity-verified models are published at
    https://huggingface.co/StemSplitio/htdemucs-ft-onnx
Download htdemucs_ft_<stem>.onnx and rename to assets/htdemucs_ft/<stem>.onnx.
"""

import sys
from pathlib import Path

import numpy as np

# ─────────────────────────────────────────────────────────────────────────────
# 0. Config — values baked into the exported graph by demucs-onnx. Printed at the
#    end so Phase 2 (StemSeparator) can match the audio I/O exactly.
# ─────────────────────────────────────────────────────────────────────────────
MODEL        = "htdemucs_ft"
STEMS        = ["drums", "bass", "vocals", "other"]  # standard Demucs source order
                                                     # is [drums, bass, other,
                                                     # vocals]; we export all four
                                                     # FT specialists.
OPSET        = 17
OUT_DIR      = Path("assets/htdemucs_ft")
PARITY_TOL   = 1e-3

# Reference constants used by the published htdemucs_ft ONNX models. n_fft/hop are
# informational for C++ (STFT is in-graph, so C++ never uses them directly).
N_FFT           = 4096
HOP             = 1024
SAMPLE_RATE     = 44100
SEGMENT_SECONDS = 39 / 5      # Fraction(39, 5) == 7.8 s (model.segment)
SEGMENT_SAMPLES = 343980      # 7.8 s @ 44.1 kHz, as used by the published models


# ─────────────────────────────────────────────────────────────────────────────
# 1. Import the exporter
# ─────────────────────────────────────────────────────────────────────────────
print("Loading demucs-onnx exporter …")
try:
    # API per the demucs-onnx README:
    #   export_to_onnx(checkpoint, output, *, stem=None, stems=None, opset=17,
    #                  parity_check=True, parity_tolerance=1e-3, ...)
    #   -> dict[str, Path]
    from demucs_onnx.export import export_to_onnx
except ImportError as e:
    print(f"ERROR: could not import demucs_onnx.export: {e}")
    print('Install with:  pip install "demucs-onnx[export]"')
    sys.exit(1)
print("demucs-onnx loaded OK")


# ─────────────────────────────────────────────────────────────────────────────
# 2. Export each FT specialist (export_to_onnx runs an ORT-vs-PyTorch parity
#    check internally and raises if max-abs-error exceeds parity_tolerance).
# ─────────────────────────────────────────────────────────────────────────────
OUT_DIR.mkdir(parents=True, exist_ok=True)

exported: dict[str, Path] = {}
failures: dict[str, str] = {}

for stem in STEMS:
    out_path = OUT_DIR / f"{stem}.onnx"
    print(f"\n── Exporting {MODEL} :: {stem}  →  {out_path}")
    try:
        result = export_to_onnx(
            MODEL,
            str(out_path),
            stem=stem,
            opset=OPSET,
            parity_check=True,
            parity_tolerance=PARITY_TOL,
        )
        # result is dict[str, Path]; print whatever it reports for transparency.
        print(f"   export_to_onnx returned: {result}")
        if not out_path.exists():
            raise RuntimeError(f"expected output not found: {out_path}")

        exported[stem] = out_path
        size_mb = out_path.stat().st_size / 1e6
        print(f"   ✅ {stem}.onnx written: {size_mb:.1f} MB  (parity < {PARITY_TOL})")

        # External-weights sidecar only appears if a single file would exceed the
        # 2 GB ONNX limit. htdemucs_ft (~316 MB) stays well under, so none is
        # expected — but report it if the toolchain produced one.
        sidecar = Path(str(out_path) + ".data")
        if sidecar.exists():
            print(f"   + sidecar: {sidecar.name} ({sidecar.stat().st_size/1e6:.1f} MB)")
    except Exception as e:  # noqa: BLE001 — report per-stem, keep going
        failures[stem] = str(e)
        print(f"   ❌ {stem} export FAILED: {e}")

if not exported:
    print("\nNo models exported successfully — aborting.")
    sys.exit(1)


# ─────────────────────────────────────────────────────────────────────────────
# 3. Probe an exported graph for the AUTHORITATIVE I/O spec (resolves any
#    doc ambiguity — Phase 2 should use exactly what this prints).
# ─────────────────────────────────────────────────────────────────────────────
probe_stem = "drums" if "drums" in exported else next(iter(exported))
probe_path = exported[probe_stem]

print("\n" + "=" * 70)
print(f"ONNX I/O SPEC  (probed from {probe_path})")
print("=" * 70)

try:
    import onnx  # noqa: F401  (checker import; optional)
    import onnxruntime as ort
except ImportError as e:
    print(f"WARNING: onnx/onnxruntime not importable, cannot probe: {e}")
    ort = None

if ort is not None:
    onnx.checker.check_model(onnx.load(str(probe_path)))
    print("onnx.checker: OK")

    sess = ort.InferenceSession(str(probe_path), providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name
    for i in sess.get_inputs():
        print(f"  input :  '{i.name}'   shape={i.shape}   dtype={i.type}")
    for o in sess.get_outputs():
        print(f"  output:  '{o.name}'   shape={o.shape}   dtype={o.type}")

    # Smoke run on one 7.8 s segment of silence so shapes resolve to concrete ints.
    dummy = np.zeros((1, 2, SEGMENT_SAMPLES), dtype=np.float32)
    outs = sess.run(None, {in_name: dummy})
    print("  --- concrete shapes from a 7.8 s silent segment ---")
    for o, arr in zip(sess.get_outputs(), outs):
        print(f"  run   :  '{o.name}' -> {tuple(arr.shape)} {arr.dtype}"
              f"   min={arr.min():.5f} max={arr.max():.5f}")


# ─────────────────────────────────────────────────────────────────────────────
# 4. Summary for Phase 2
# ─────────────────────────────────────────────────────────────────────────────
print("\n" + "=" * 70)
print("PHASE 2 REFERENCE")
print("=" * 70)
print(f"  Model           : {MODEL}  (bag of 4 fine-tuned specialists)")
print(f"  Exported OK     : {sorted(exported)}")
if failures:
    print(f"  Exported FAILED : {failures}")
print(f"  Opset           : {OPSET}")
print(f"  Parity          : ORT-vs-PyTorch max-abs-err < {PARITY_TOL} (enforced by export)")
print( "  Graph I/O       : input 'mix' float32 [1, 2, S]  →  output float32 [1, 2, S]")
print( "                    (raw stereo waveform in and out; STFT/iSTFT are IN-GRAPH —")
print( "                     C++ does NOT compute STFT. Each specialist returns its own stem.)")
print(f"  Audio config    : {SAMPLE_RATE} Hz, stereo; segment {SEGMENT_SECONDS:.2f} s "
      f"= {SEGMENT_SAMPLES} samples; overlap-add for longer audio (Demucs uses 0.25).")
print(f"  In-graph STFT   : n_fft={N_FFT}, hop={HOP}, hann, normalized=True (RealSTFT)")
print( "  Stem ordering   : standard Demucs source order is [drums, bass, other, vocals];")
print( "                    each FT specialist file returns ONLY its own stem.")
print( "  Bundled in app  : drums.onnx, bass.onnx, vocals.onnx  (other = subtraction).")
print("\n✅  Export step complete.")
print("    Next: re-run CMake configure+build; the POST_BUILD step copies the three")
print("    bundled models into the .vst3 / .component Contents/Resources/htdemucs_ft/.")
