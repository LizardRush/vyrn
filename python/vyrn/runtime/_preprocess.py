"""What see() returns, turned into network input exactly like the C++ brain (core/src/brain/preprocess.cpp).

- Pictures: uint8 RGB [h, w, 3] -> one frame uint8 [3, H, W] (CHW). A small brain (max side <= 32) keeps the size
  it learned on: copied, or nearest-resized when the picture's size differs. A big brain sees 84 x 84: each axis is
  area-averaged when shrinking and bilinear-interpolated when growing, rows first, then columns.
- Frames are stacked oldest -> newest; a segment's first frame fills every slot (AAAA -> AAAB -> AABC).
- Numbers: clip((x - mean) / sqrt(var + 1e-8), -10, 10) with the brain's saved statistics.

Bit-exact on purpose (the golden test compares bytes with the C++). The C++ resampling accumulates in float32 with
one multiply-add per tap, and the compiler fuses each into an FMA (a single rounding): fma32() reproduces that,
and linear taps use the fused double x = (i + 0.5) * scale - 0.5 the same way.
"""

import math
from collections import namedtuple
from fractions import Fraction

import numpy as np

SMALL_PICTURE_MAX_SIDE = 32     # pictures up to this size keep their native size (small CNN)
BIG_PICTURE_SIDE = 84           # bigger pictures are resized to 84 x 84 (DER CNN)

Geometry = namedtuple("Geometry", "small h w")

_LOW29 = np.uint64((1 << 29) - 1)
_HALF = np.uint64(1 << 28)


def choose_geometry(h, w):
    """The frame size the network sees for a picture of h x w pixels."""
    if h < 1 or w < 1:
        raise ValueError(f"A picture must be at least 1x1 pixels (got {w}x{h}).")
    if max(h, w) <= SMALL_PICTURE_MAX_SIDE:
        return Geometry(True, h, w)
    return Geometry(False, BIG_PICTURE_SIDE, BIG_PICTURE_SIDE)


def fma32(a, b, c):
    """float32 a * b + c with ONE rounding (what an FMA instruction computes), elementwise, for float32 inputs.

    a * b is exact in float64 (24 + 24 bits). The float64 sum can round, and rounding it again to float32 is wrong
    only when it lands exactly halfway between two float32 values: then the sum's exact error (TwoSum) decides.
    """
    p = np.multiply(a, b, dtype=np.float64)
    c64 = np.asarray(c, dtype=np.float64)
    s = p + c64
    r = s.astype(np.float32)
    tie = (s.view(np.uint64) & _LOW29) == _HALF
    if tie.any():
        p, c64 = np.broadcast_to(p, s.shape)[tie], np.broadcast_to(c64, s.shape)[tie]
        st, rt = s[tie], r[tie]
        bv = st - p
        err = (p - (st - bv)) + (c64 - bv)
        rt64 = rt.astype(np.float64)
        rt = np.where((err > 0) & (rt64 < st), np.nextafter(rt, np.float32(np.inf)), rt)
        rt = np.where((err < 0) & (rt64 > st), np.nextafter(rt, np.float32(-np.inf)), rt)
        r[tie] = rt
    return r


def _area_taps(n_in, n_out):
    """Shrinking: output i covers input [i*s, (i+1)*s), each input pixel weighted by its overlap."""
    taps = []
    scale = n_in / n_out
    for i in range(n_out):
        start, end = i * scale, (i + 1) * scale
        first = math.floor(start)
        last = min(n_in - 1, math.ceil(end) - 1)
        weights = []
        for j in range(first, last + 1):
            overlap = min(end, j + 1.0) - max(start, float(j))
            weights.append(np.float32(max(0.0, overlap) / scale))
        taps.append((first, weights))
    return taps


def _fused(a, b, c):
    """a * b + c for Python floats with one rounding (C++'s fused multiply-add on doubles)."""
    return float(Fraction(a) * Fraction(b) + Fraction(c))


def _linear_taps(n_in, n_out):
    """Growing (bilinear), pixel centres aligned: x = (i + 0.5) * in / out - 0.5, clamped to the edges."""
    taps = []
    scale = n_in / n_out
    for i in range(n_out):
        x = min(max(_fused(i + 0.5, scale, -0.5), 0.0), float(n_in - 1))
        j0 = math.floor(x)
        f = x - j0
        if j0 + 1 < n_in and f > 0:
            taps.append((j0, [np.float32(1.0 - f), np.float32(f)]))
        else:
            taps.append((j0, [np.float32(1.0)]))
    return taps


def _tap_arrays(n_in, n_out):
    """(first [n_out], weights [n_out, most taps]) with zero weights after each output's own taps."""
    taps = _area_taps(n_in, n_out) if n_in > n_out else _linear_taps(n_in, n_out)
    width = max(len(w) for _, w in taps)
    first = np.array([f for f, _ in taps], dtype=np.int64)
    weights = np.zeros((n_out, width), dtype=np.float32)
    for row, (_, w) in enumerate(taps):
        weights[row, :len(w)] = w
    return first, weights


def _nearest(n_out, n_in):
    """Nearest pixel, centre-aligned: input i = floor((o + 0.5) * in / out)."""
    o = np.arange(n_out, dtype=np.int64)
    return np.minimum(n_in - 1, ((2 * o + 1) * n_in) // (2 * n_out))


_TAPS_CACHE = {}


def _taps(n_in, n_out):
    key = (n_in, n_out)
    if key not in _TAPS_CACHE:
        if len(_TAPS_CACHE) > 64:
            _TAPS_CACHE.clear()
        _TAPS_CACHE[key] = _tap_arrays(n_in, n_out)
    return _TAPS_CACHE[key]


def to_frame(rgb, geo):
    """One RGB picture (uint8 [h, w, 3]) as a network frame (uint8 [3, geo.h, geo.w])."""
    rgb = np.ascontiguousarray(rgb, dtype=np.uint8)
    if rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError("to_frame needs an RGB picture of shape [height, width, 3]")
    h, w = rgb.shape[:2]
    if h < 1 or w < 1 or geo.h < 1 or geo.w < 1:
        raise ValueError("to_frame: empty picture or frame")
    if geo.small or (h == geo.h and w == geo.w):
        rows = np.arange(geo.h) if h == geo.h else _nearest(geo.h, h)
        cols = np.arange(geo.w) if w == geo.w else _nearest(geo.w, w)
        return np.ascontiguousarray(rgb[rows][:, cols].transpose(2, 0, 1))

    # Separable resampling: rows first (width), then columns (height), accumulating in float32.
    first, weights = _taps(w, geo.w)
    tmp = np.zeros((h, geo.w, 3), dtype=np.float32)
    for i in range(weights.shape[1]):
        pixels = rgb[:, np.minimum(first + i, w - 1), :].astype(np.float32)
        tmp = fma32(weights[None, :, i, None], pixels, tmp)
    first, weights = _taps(h, geo.h)
    acc = np.zeros((geo.h, geo.w, 3), dtype=np.float32)
    for i in range(weights.shape[1]):
        acc = fma32(weights[:, i, None, None], tmp[np.minimum(first + i, h - 1)], acc)
    out = np.clip(acc + np.float32(0.5), np.float32(0.0), np.float32(255.0)).astype(np.uint8)
    return np.ascontiguousarray(out.transpose(2, 0, 1))


class FrameStack:
    """The last k frames, oldest first. reset() starts a segment: every slot holds its first frame."""

    def __init__(self, k, frame_shape):
        if k < 1:
            raise ValueError("FrameStack needs k >= 1")
        self.k = k
        self.data = np.zeros((k,) + tuple(frame_shape), dtype=np.uint8)

    def reset(self, frame):
        self.data[:] = frame

    def push(self, frame):
        if self.k > 1:
            self.data[:-1] = self.data[1:]
        self.data[-1] = frame

    def stacked(self):
        """The network input: uint8 [3k, H, W], the oldest frame's RGB first."""
        k, c, h, w = self.data.shape
        return self.data.reshape(k * c, h, w)

    def newest(self):
        return self.data[-1]


class Normalizer:
    """The brain's saved number statistics (frozen): clip((x - mean) / sqrt(var + 1e-8), -10, 10) as float32."""

    def __init__(self, d, stats=None):
        self.d = d
        self.mean = np.zeros(d, dtype=np.float64)
        self.var = np.ones(d, dtype=np.float64)
        if stats is not None:
            self.mean = np.array(stats["mean"], dtype=np.float64)
            self.var = np.array(stats["var"], dtype=np.float64)

    def normalize(self, numbers):
        x = np.asarray(numbers, dtype=np.float32).astype(np.float64)
        z = (x - self.mean) / np.sqrt(self.var + 1e-8)
        return np.clip(z, -10.0, 10.0).astype(np.float32)
