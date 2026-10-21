#!/usr/bin/env python3
"""Small offline tests; no downloaded model or inference runtime required.

Run: .venv/bin/python -m unittest discover -s tools -p test_optimize_stem_model.py
"""

import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto

import optimize_stem_model as optimizer


def tiny_model():
    graph = helper.make_graph(
        [helper.make_node("Identity", ["mix"], ["unused"], name="unused"),
         helper.make_node("Div", ["mix", "old_envelope"], ["stems"], name="/real_istft/Div")],
        "test graph",
        [helper.make_tensor_value_info("mix", TensorProto.FLOAT, [2])],
        [helper.make_tensor_value_info("stems", TensorProto.FLOAT, [2])],
        [numpy_helper.from_array(np.ones(2, np.float32), "old_envelope"),
         numpy_helper.from_array(np.ones(3, np.float32), "unused_initializer")])
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=8)


class EnvelopeTests(unittest.TestCase):
    def test_recorded_window_overlap_crop_and_clamp(self):
        result = optimizer.build_envelope(np.array([0, .25, 1, .25], np.float64), 2, 3, 1, 1e-11)
        np.testing.assert_array_equal(result, np.array([1.25, 1.25, .25], np.float32))
        clamped = optimizer.build_envelope(np.zeros(4, np.float64), 1, 2, 1, 1e-11)
        np.testing.assert_array_equal(clamped, np.full(2, 1e-11, np.float32))

    def test_rejects_invalid_window_and_dimensions(self):
        for window, frames, length in ((np.ones(4, np.float32), 1, 2),
                                       (np.array([0, np.nan], np.float64), 1, 2),
                                       (np.ones(4, np.float64), 10, 2)):
            with self.subTest(window=window, frames=frames):
                with self.assertRaises(optimizer.OptimizationError):
                    optimizer.build_envelope(window, frames, length, 1, 1e-11)

    def test_pruning_keeps_live_nodes_and_constants_in_order(self):
        model = tiny_model()
        self.assertEqual(optimizer.prune_unused_graph_nodes(model), 1)
        self.assertEqual([node.name for node in model.graph.node], ["/real_istft/Div"])
        self.assertEqual([value.name for value in model.graph.initializer], ["old_envelope"])
        onnx.checker.check_model(model)

    def test_static_input_validation_is_explicit(self):
        with self.assertRaisesRegex(optimizer.OptimizationError, "static float32"):
            optimizer._validate_and_make_envelope(tiny_model())


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="cue-stem-optimizer-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "original.onnx"
        self.output = self.root / "compact.onnx"
        self.source_bytes = tiny_model().SerializeToString()
        self.source.write_bytes(self.source_bytes)

    def optimize_fixture(self, **kwargs):
        # Exercise real pruning, checking, serialization, and publication with
        # a tiny graph; only the pinned model admission/shape guard is replaced.
        with mock.patch.object(optimizer, "SOURCE_SHA256", hashlib.sha256(self.source_bytes).hexdigest()), \
             mock.patch.object(optimizer, "_validate_and_make_envelope", return_value=np.ones(2, np.float32)):
            return optimizer.optimize_model(self.source, self.output, **kwargs)

    def test_unknown_source_rejected_before_parsing_or_writing(self):
        self.output.write_bytes(b"existing output")
        with mock.patch.object(optimizer.onnx, "load") as load:
            with self.assertRaisesRegex(optimizer.OptimizationError, "Unsupported source SHA256"):
                optimizer.optimize_model(self.source, self.output, force=True)
            load.assert_not_called()
        self.assertEqual(self.output.read_bytes(), b"existing output")
        self.assertEqual(self.source.read_bytes(), self.source_bytes)

    def test_cannot_overwrite_original_even_with_force(self):
        with self.assertRaisesRegex(optimizer.OptimizationError, "different files"):
            optimizer.optimize_model(self.source, self.source, force=True)
        self.assertEqual(self.source.read_bytes(), self.source_bytes)

    def test_publication_is_idempotent_and_preserves_source(self):
        first = self.optimize_fixture()
        original_stat = self.output.stat()
        second = self.optimize_fixture()
        self.assertFalse(first["unchanged"])
        self.assertTrue(second["unchanged"])
        self.assertEqual(first["sha256"], second["sha256"])
        self.assertEqual(original_stat.st_mtime_ns, self.output.stat().st_mtime_ns)
        self.assertEqual(self.source.read_bytes(), self.source_bytes)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), ["compact.onnx", "original.onnx"])
        self.assertEqual(self.output.stat().st_mode & 0o444, 0o444)

    def test_different_output_requires_force(self):
        self.output.write_bytes(b"existing output")
        with self.assertRaisesRegex(optimizer.OptimizationError, "Refusing to overwrite"):
            self.optimize_fixture()
        self.assertEqual(self.output.read_bytes(), b"existing output")
        result = self.optimize_fixture(force=True)
        self.assertFalse(result["unchanged"])
        onnx.checker.check_model(onnx.load(str(self.output)))
        self.assertEqual(self.source.read_bytes(), self.source_bytes)

    def test_failed_publication_removes_temporary_file(self):
        with mock.patch.object(optimizer.os, "link", side_effect=PermissionError("publication denied")):
            with self.assertRaises(PermissionError):
                self.optimize_fixture()
        self.assertEqual([path.name for path in self.root.iterdir()], ["original.onnx"])


if __name__ == "__main__":
    unittest.main()
