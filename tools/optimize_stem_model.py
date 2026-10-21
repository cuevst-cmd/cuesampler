#!/usr/bin/env python3
"""Compact the pinned HTDemucs model without changing its inference operators.

The exporter records the fixed iSTFT normalization envelope as thousands of
constant operations. Replace only that envelope with its exact float32 values,
then remove the graph nodes that no longer contribute to the output.

Requires numpy and onnx (the existing model export environment supplies both):
    .venv/bin/python tools/optimize_stem_model.py \
        assets/htdemucs/htdemucs.onnx assets/htdemucs/htdemucs-compact.onnx

Only the SHA-pinned original export is accepted. The original is never changed;
an existing, different output is refused unless --force is explicitly supplied.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile

import numpy as np
import onnx
from onnx import numpy_helper


SOURCE_SHA256 = "68d0bf16428ef66e692cdff8a9ccf28f1ef3f69440d57e58605a4cc55fcc5e74"
ENVELOPE_SHA256 = "fef7c2f298d361e1cb988580ef989a568594329db8404315772764626104e1ab"
ENVELOPE_NAME = "cue/stem_normalization_envelope"
INPUT_SHAPE = (1, 2, 343980)
N_FFT = 4096
HOP = 1024


class OptimizationError(ValueError):
    """The input or requested output is unsuitable for this exact optimization."""


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _constant(model: onnx.ModelProto, output_name: str) -> np.ndarray:
    producers = [node for node in model.graph.node if output_name in node.output]
    if len(producers) != 1 or producers[0].op_type != "Constant":
        raise OptimizationError(f"Expected a single Constant producing {output_name}")
    values = [attribute.t for attribute in producers[0].attribute
              if attribute.name == "value"
              and attribute.type == onnx.AttributeProto.TENSOR]
    if len(values) != 1:
        raise OptimizationError(f"Expected a tensor Constant for {output_name}")
    return numpy_helper.to_array(values[0])


def _node(model: onnx.ModelProto, name: str, op_type: str) -> onnx.NodeProto:
    matches = [node for node in model.graph.node if node.name == name]
    if len(matches) != 1 or matches[0].op_type != op_type:
        raise OptimizationError(f"Expected one {op_type} node named {name}")
    return matches[0]


def build_envelope(window_squared: np.ndarray, frames: int, length: int,
                   hop: int, minimum: float) -> np.ndarray:
    """Use the export's exact float64 constants and original addition order."""
    if (window_squared.ndim != 1 or window_squared.dtype != np.float64
            or not np.all(np.isfinite(window_squared))
            or np.any(window_squared < 0)):
        raise OptimizationError("The normalization window must be finite float64 values")
    n_fft = len(window_squared)
    if n_fft == 0 or n_fft % 2 or frames <= 0 or length <= 0 or hop <= 0:
        raise OptimizationError("Invalid normalization envelope dimensions")
    if not np.isfinite(minimum) or minimum <= 0:
        raise OptimizationError("The envelope clamp must be positive and finite")
    if (frames - 1) * hop + n_fft > length + n_fft:
        raise OptimizationError("Normalization frames exceed the exported envelope")
    envelope = np.zeros(length + n_fft, dtype=np.float64)
    for frame in range(frames):
        start = frame * hop
        envelope[start:start + n_fft] += window_squared
    envelope = envelope[n_fft // 2:n_fft // 2 + length]
    return np.maximum(envelope, minimum).astype(np.float32)


def _validate_and_make_envelope(model: onnx.ModelProto) -> np.ndarray:
    if model.ir_version != 8:
        raise OptimizationError("Only the pinned IR version 8 export is supported")
    if [(entry.domain, entry.version) for entry in model.opset_import] != [("", 17)]:
        raise OptimizationError("Only the pinned ONNX opset 17 export is supported")
    if len(model.graph.input) != 1 or model.graph.input[0].name != "mix":
        raise OptimizationError("Expected the single waveform input 'mix'")
    tensor = model.graph.input[0].type.tensor_type
    dimensions = tensor.shape.dim
    if (tensor.elem_type != onnx.TensorProto.FLOAT
            or any(dim.dim_param or not dim.HasField("dim_value") for dim in dimensions)
            or tuple(dim.dim_value for dim in dimensions) != INPUT_SHAPE):
        raise OptimizationError(f"Expected static float32 input shape {INPUT_SHAPE}")
    if len(model.graph.output) != 1 or model.graph.output[0].name != "stems":
        raise OptimizationError("Expected the single output 'stems'")
    if any(item.data_location == onnx.TensorProto.EXTERNAL
           for item in model.graph.initializer):
        raise OptimizationError("External tensor data is unsupported")
    if any(item.name == ENVELOPE_NAME for item in model.graph.initializer):
        raise OptimizationError("The original graph already contains the compact envelope")

    for name, op_type in (("/real_stft/Conv", "Conv"),
                          ("/real_stft/Conv_1", "Conv"),
                          ("/real_istft/ConvTranspose", "ConvTranspose"),
                          ("/real_istft/ConvTranspose_1", "ConvTranspose")):
        node = _node(model, name, op_type)
        if len(node.input) != 2:
            raise OptimizationError(f"Unexpected transform inputs for {name}")
        kernel = _constant(model, node.input[1])
        strides = [list(attribute.ints) for attribute in node.attribute
                   if attribute.name == "strides"]
        if kernel.shape != (2049, 1, N_FFT) or kernel.dtype != np.float32 or strides != [[HOP]]:
            raise OptimizationError(f"Unexpected transform kernel or stride for {name}")

    divide = _node(model, "/real_istft/Div", "Div")
    if list(divide.input) != ["/real_istft/Slice_output_0", "/real_istft/Cast_1_output_0"]:
        raise OptimizationError("The inverse transform normalization inputs changed")

    steps = (INPUT_SHAPE[-1] + HOP - 1) // HOP
    frames = steps + 4
    length = steps * HOP + 3 * HOP
    window_squared = _constant(model, "/real_istft/Constant_19_output_0")
    if window_squared.shape != (N_FFT,) or window_squared.dtype != np.float64:
        raise OptimizationError("Unexpected inverse transform normalization window")
    # Each frame adds the same recorded float64 window. Do not recompute a Hann
    # window with a platform-dependent trigonometric library.
    for frame in range(frames):
        addition = _node(model, f"/real_istft/Add_{frame + 2}", "Add")
        if len(addition.input) != 2:
            raise OptimizationError("Unexpected normalization frame inputs")
        value = _constant(model, addition.input[1])
        if value.dtype != np.float64 or not np.array_equal(value, window_squared):
            raise OptimizationError("Normalization windows differ between frames")
    minimum = _constant(model, "/real_istft/Constant_8520_output_0")
    if minimum.shape != () or minimum.dtype != np.float64 or float(minimum) != 1e-11:
        raise OptimizationError("Unexpected normalization clamp")
    envelope = build_envelope(window_squared, frames, length, HOP, float(minimum))
    if hashlib.sha256(envelope.tobytes()).hexdigest() != ENVELOPE_SHA256:
        raise OptimizationError("The computed normalization envelope differs from the pinned export")
    return envelope


def prune_unused_graph_nodes(model: onnx.ModelProto) -> int:
    """Keep the original order of all nodes reachable from graph outputs."""
    producers: dict[str, int] = {}
    for index, node in enumerate(model.graph.node):
        for output in node.output:
            if output in producers:
                raise OptimizationError(f"Duplicate tensor producer: {output}")
            producers[output] = index
    live_values = {output.name for output in model.graph.output}
    stack = list(live_values)
    live_nodes: set[int] = set()
    while stack:
        value = stack.pop()
        index = producers.get(value)
        if index is None or index in live_nodes:
            continue
        live_nodes.add(index)
        for source in model.graph.node[index].input:
            if source and source not in live_values:
                live_values.add(source)
                stack.append(source)
    before = len(model.graph.node)
    kept_nodes = [node for index, node in enumerate(model.graph.node) if index in live_nodes]
    kept_initializers = [item for item in model.graph.initializer if item.name in live_values]
    kept_value_info = [item for item in model.graph.value_info if item.name in live_values]
    del model.graph.node[:]
    model.graph.node.extend(kept_nodes)
    del model.graph.initializer[:]
    model.graph.initializer.extend(kept_initializers)
    del model.graph.value_info[:]
    model.graph.value_info.extend(kept_value_info)
    return before - len(kept_nodes)


def optimize_model(source: Path, output: Path, *, force: bool = False) -> dict:
    source, output = Path(source), Path(output)
    if source.resolve() == output.resolve():
        raise OptimizationError("Source and output must be different files; the original is preserved")
    source_hash = file_sha256(source)
    if source_hash != SOURCE_SHA256:
        raise OptimizationError(f"Unsupported source SHA256 {source_hash}; expected {SOURCE_SHA256}")
    if not output.parent.is_dir():
        raise OptimizationError(f"Output directory does not exist: {output.parent}")
    if output.is_dir():
        raise OptimizationError(f"Output is a directory: {output}")
    model = onnx.load(str(source), load_external_data=False)
    envelope = _validate_and_make_envelope(model)
    _node(model, "/real_istft/Div", "Div").input[1] = ENVELOPE_NAME
    model.graph.initializer.append(numpy_helper.from_array(envelope, ENVELOPE_NAME))
    removed = prune_unused_graph_nodes(model)
    onnx.checker.check_model(model)

    # A same-directory temporary file allows atomic publication. A hard link
    # avoids the check-then-replace race when overwrite was not authorized.
    temporary: Path | None = None
    try:
        descriptor, name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
        temporary = Path(name)
        with os.fdopen(descriptor, "wb") as destination:
            os.chmod(temporary, 0o644)
            destination.write(model.SerializeToString())
            destination.flush()
            os.fsync(destination.fileno())
        output_hash = file_sha256(temporary)
        unchanged = output.is_file() and file_sha256(output) == output_hash
        if unchanged:
            pass
        elif force:
            os.replace(temporary, output)
        else:
            try:
                os.link(temporary, output)
            except FileExistsError as error:
                raise OptimizationError(f"Refusing to overwrite a different output: {output}; use --force") from error
        return {"output": str(output), "sha256": output_hash,
                "bytes": output.stat().st_size, "nodes": len(model.graph.node),
                "removed_nodes": removed, "unchanged": unchanged}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="The original SHA-pinned htdemucs.onnx")
    parser.add_argument("output", type=Path, help="A separate compact model path")
    parser.add_argument("--force", action="store_true", help="Atomically replace an existing, different output")
    arguments = parser.parse_args(argv)
    try:
        result = optimize_model(arguments.source, arguments.output, force=arguments.force)
    except (OptimizationError, OSError, onnx.checker.ValidationError) as error:
        print(f"Stem model optimization failed: {error}", file=sys.stderr)
        return 1
    action = "Unchanged" if result["unchanged"] else "Wrote"
    print(f"{action}: {result['output']} ({result['bytes']:,} bytes, {result['nodes']:,} nodes; "
          f"removed {result['removed_nodes']:,} unused nodes)")
    print(f"SHA256: {result['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
