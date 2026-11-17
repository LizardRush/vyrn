"""Golden parity: vyrn.runtime (numpy) against the C++ brain (docs/dev/standalone.md section 5).

tests/golden/standalone_golden.cpp writes the fixtures: for an MLP, a small CNN and a DER brain the exported .vrl,
random network inputs and their Q-values, and a played sequence (raw observations -> preprocessing and frame stack
or number statistics -> Q-values); and to_frame() of random pictures of 1x1, 10x10, 32x32, 33x33, 640x480 and 9x5.
Preprocessing must match byte for byte, Q-values within 1e-4 (relative to the row's largest Q-value).

ctest runs this as python_standalone_golden after standalone_golden_fixtures (VYRN_GOLDEN_DIR). Run by hand, it makes
the fixtures itself with build/tests/standalone_golden (or VYRN_GOLDEN_EXE), or skips when that isn't built.
"""

import json
import os
import random
import subprocess
from pathlib import Path

import numpy as np
import pytest

import testkit
from vyrn.runtime import _brain, _preprocess, _vrl

RTOL = 1e-4


@pytest.fixture(scope="module")
def golden(tmp_path_factory):
    folder = os.environ.get("VYRN_GOLDEN_DIR")
    if folder and (Path(folder) / "golden.json").is_file():
        root = Path(folder)
    else:
        exe = Path(os.environ.get("VYRN_GOLDEN_EXE") or testkit.REPO / "build" / "tests" / "standalone_golden")
        if not exe.is_file():
            pytest.skip("the C++ fixture generator isn't built (cmake --build build --target standalone_golden)")
        root = tmp_path_factory.mktemp("golden")
        subprocess.run([str(exe), str(root)], check=True, capture_output=True, timeout=300)
    index = json.loads((root / "golden.json").read_text())
    return root, index


def load(root, name, dtype, shape):
    return np.fromfile(root / name, dtype=dtype).reshape(shape)


def assert_q_close(actual, expected, what):
    actual, expected = np.asarray(actual, dtype=np.float64), np.asarray(expected, dtype=np.float64)
    assert actual.shape == expected.shape, what
    for row_a, row_e in zip(actual.reshape(-1, actual.shape[-1]), expected.reshape(-1, expected.shape[-1])):
        scale = max(float(np.abs(row_e).max()), 1e-6)
        error = float(np.abs(row_a - row_e).max()) / scale
        assert error <= RTOL, f"{what}: relative error {error:.2e} (python {row_a}, C++ {row_e})"


def test_preprocessing_is_byte_exact(golden):
    root, index = golden
    sizes = set()
    for case in index["frames"]:
        h, w = case["h"], case["w"]
        picture = load(root, case["picture"], np.uint8, (h, w, 3))
        geo = _preprocess.Geometry(case["small"], case["gh"], case["gw"])
        expected = load(root, case["frame"], np.uint8, (3, geo.h, geo.w))
        frame = _preprocess.to_frame(picture, geo)
        assert frame.dtype == np.uint8 and frame.shape == expected.shape
        diff = np.argwhere(frame != expected)
        assert diff.size == 0, (f"{w}x{h} -> {geo}: {len(diff)} bytes differ, first at {diff[0].tolist()}: "
                                f"python {frame[tuple(diff[0])]}, C++ {expected[tuple(diff[0])]}")
        sizes.add((h, w))
    assert {(1, 1), (10, 10), (32, 32), (33, 33), (480, 640)} <= sizes


@pytest.mark.parametrize("net", ["mlp", "small_cnn", "der"])
def test_q_values_match_the_cpp_network(golden, net):
    root, index = golden
    fixture = next(n for n in index["nets"] if n["name"] == net)
    brain = _brain.Brain(_vrl.read_vrl(root / fixture["vrl"]))
    assert brain.file.net == net
    shape = tuple(fixture["input_shape"])
    dtype = np.float32 if net == "mlp" else np.uint8
    inputs = load(root, fixture["inputs"], dtype, shape)
    expected = load(root, fixture["q"], np.float32, (shape[0], fixture["n"]))
    assert_q_close(brain.net.forward(inputs), expected, f"{net} Q-values")


@pytest.mark.parametrize("net", ["mlp", "small_cnn", "der"])
def test_a_played_sequence_matches(golden, net):
    """Raw observations -> preprocessing, frame stack (segment starts repeat the first frame) or number statistics
    -> network input (exact) -> Q-values, step by step, like core/src/run/brain_host.cpp."""
    root, index = golden
    fixture = next(n for n in index["nets"] if n["name"] == net)
    seq = fixture["sequence"]
    brain = _brain.Brain(_vrl.read_vrl(root / fixture["vrl"]))
    steps, n = seq["steps"], fixture["n"]
    expected_q = load(root, seq["q"], np.float32, (steps, n))
    if net == "mlp":
        d = brain.shape["d"]
        raw = load(root, seq["raw"], np.float32, (steps, d))
        expected_input = load(root, seq["net_input"], np.float32, (steps, d))
        observations, starts = list(raw), {0}
    else:
        ph, pw, _ = seq["picture_shape"]
        observations = list(load(root, seq["pictures"], np.uint8, (steps, ph, pw, 3)))
        expected_input = load(root, seq["net_input"], np.uint8, (steps,) + brain.stack.stacked().shape)
        starts = set(seq["starts"])
    for step, obs in enumerate(observations):
        (brain.start if step in starts else brain.push)(obs)
        np.testing.assert_array_equal(brain.net_input(), expected_input[step], err_msg=f"{net} step {step}")
        assert_q_close(brain.q_values(), expected_q[step], f"{net} step {step}")
        greedy = brain.act(0.0, random.Random(0))
        assert greedy == int(np.argmax(expected_q[step])), f"{net} step {step}: another greedy choice"
