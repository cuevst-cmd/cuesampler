#!/usr/bin/env python3
"""
export_beat_this.py
-------------------
Exports the beat_this pretrained beat-tracking model to ONNX format
for use in the CUE SAMPLER JUCE plugin.

Run once with:
    .venv/bin/python export_beat_this.py

Outputs:
    assets/beat_this.onnx
"""

import os
import sys
import torch
import torch.nn as nn
import onnx
import onnxruntime as ort
import numpy as np

# -----------------------------------------------------------------
# 1. Load the pretrained beat_this model
# -----------------------------------------------------------------
print("Loading beat_this model …")

try:
    from beat_this.inference import load_model
except ImportError as e:
    print(f"ERROR: Could not import beat_this: {e}")
    sys.exit(1)

# Already cached from previous run
model = load_model("final0", device="cpu")
model.eval()
print("Model loaded OK")

# -----------------------------------------------------------------
# 2. Probe output format
# -----------------------------------------------------------------
TEST_FRAMES = 1500
MEL_BINS = 128
dummy_input = torch.zeros(1, TEST_FRAMES, MEL_BINS)

print(f"Dummy input shape: {dummy_input.shape}")
with torch.no_grad():
    raw_output = model(dummy_input)

print(f"Raw output type: {type(raw_output)}")
if isinstance(raw_output, dict):
    print(f"Dict keys: {list(raw_output.keys())}")
    for k, v in raw_output.items():
        print(f"  '{k}': {v.shape}")
    OUTPUT_KEYS = list(raw_output.keys())
elif isinstance(raw_output, (tuple, list)):
    for i, v in enumerate(raw_output):
        print(f"  [{i}]: {v.shape}")
    OUTPUT_KEYS = None
else:
    print(f"  tensor: {raw_output.shape}")
    OUTPUT_KEYS = None

# -----------------------------------------------------------------
# 3. Wrap model so ONNX always gets a tuple output
# -----------------------------------------------------------------
class BeatThisWrapper(nn.Module):
    def __init__(self, inner):
        super().__init__()
        self.inner = inner

    def forward(self, mel):
        out = self.inner(mel)
        if isinstance(out, dict):
            # Return tensors in a fixed order matching OUTPUT_KEYS
            return tuple(out[k] for k in OUTPUT_KEYS)
        if isinstance(out, (tuple, list)):
            return tuple(out)
        return (out,)

wrapper = BeatThisWrapper(model)
wrapper.eval()

# Verify wrapper output
with torch.no_grad():
    wrapped_out = wrapper(dummy_input)
print(f"\nWrapped output: {len(wrapped_out)} tensor(s)")
output_names = OUTPUT_KEYS if OUTPUT_KEYS else [f"output_{i}" for i in range(len(wrapped_out))]
for name, t in zip(output_names, wrapped_out):
    print(f"  '{name}': {t.shape}")

# -----------------------------------------------------------------
# 4. Export to ONNX
# -----------------------------------------------------------------
os.makedirs("assets", exist_ok=True)
onnx_path = "assets/beat_this.onnx"
print(f"\nExporting to {onnx_path} …")

# Build dynamic axes: time_frames dimension is dynamic for both input and outputs
dynamic_axes = {"mel_spectrogram": {0: "batch", 1: "time_frames"}}
for name in output_names:
    dynamic_axes[name] = {0: "batch", 1: "time_frames"}

with torch.no_grad():
    torch.onnx.export(
        wrapper,
        dummy_input,
        onnx_path,
        export_params=True,
        opset_version=17,
        do_constant_folding=True,
        input_names=["mel_spectrogram"],
        output_names=output_names,
        dynamic_axes=dynamic_axes,
        verbose=False,
    )

size_mb = os.path.getsize(onnx_path) / 1e6
print(f"ONNX model written: {size_mb:.1f} MB")

# -----------------------------------------------------------------
# 5. Validate with ONNX checker
# -----------------------------------------------------------------
print("\nValidating ONNX model …")
onnx_model = onnx.load(onnx_path)
onnx.checker.check_model(onnx_model)
print("ONNX check: OK")

# -----------------------------------------------------------------
# 6. Quick onnxruntime inference test
# -----------------------------------------------------------------
print("\nRunning onnxruntime inference test …")
sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
inp = np.zeros((1, TEST_FRAMES, MEL_BINS), dtype=np.float32)
ort_outs = sess.run(None, {"mel_spectrogram": inp})
for name, o in zip(output_names, ort_outs):
    print(f"  '{name}': {o.shape}  min={o.min():.4f}  max={o.max():.4f}")

# -----------------------------------------------------------------
# 7. Print C++ usage info
# -----------------------------------------------------------------
print("\n✅  Export complete!")
print(f"   Model path : {os.path.abspath(onnx_path)}")
print(f"   File size  : {size_mb:.1f} MB")
print(f"\nC++ input  : \"mel_spectrogram\"  shape [1, T, 128]  float32")
print(f"C++ outputs:")
for name, o in zip(output_names, ort_outs):
    print(f"   \"{name}\"  shape {o.shape}  float32")
print("\nNote: 'beat' output = beat probabilities per frame (50 fps)")
print("      'downbeat' output = downbeat probabilities per frame (50 fps)")
print("\nNext: run cmake and build. Make sure libs/onnxruntime/ is populated.")
