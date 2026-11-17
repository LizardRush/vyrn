"""The three Q-networks of the C++ brain (core/src/brain/nets.cpp) as numpy forward passes.

- MLP (numbers): fc1 d->128, ReLU, fc2 128->128, ReLU, head 128->n.
- Small CNN (pictures up to 32 px): /255, conv1 16@3x3 s1 p1, ReLU, conv2 32@3x3 s2 p1, ReLU, flatten (C, H, W),
  fc1 ->256, ReLU, head 256->n.
- DER CNN (84 x 84): /255, conv1 32@5x5 s5, ReLU, conv2 64@5x5 s5, ReLU, flatten (C, H, W), fc1 ->256, ReLU,
  head 256->n.

Convolutions are cross-correlations done with im2col and one matrix product. Weights keep torch's layout
(conv [out, in, kh, kw], linear [out, in]); the maths runs in float64, after the same float32 /255 as torch.
"""

import numpy as np

MLP, SMALL_CNN, DER = "mlp", "small_cnn", "der"
_SCALE = np.float32(1.0) / np.float32(255.0)      # torch: x.to(kFloat) * (1.0f / 255.0f)


def conv_out(size, kernel, stride, pad):
    span = size + 2 * pad - kernel
    return 0 if span < 0 else span // stride + 1


def net_kind(shape):
    """"mlp", "small_cnn" or "der" for a brain shape (a dict like the manifest's "shape")."""
    if shape["kind"] == "numbers":
        return MLP
    return SMALL_CNN if shape["small_picture"] else DER


def describe(shape):
    """The C++ describe(): "MLP, 3 numbers -> 3 choices", "small CNN, 12x10x10 -> 3 choices"..."""
    n = len(shape["choices"])
    kind = net_kind(shape)
    if kind == MLP:
        return f"MLP, {shape['d']} numbers -> {n} choices"
    name = "small CNN" if kind == SMALL_CNN else "DER CNN"
    return f"{name}, {shape['in_c']}x{shape['in_h']}x{shape['in_w']} -> {n} choices"


def tensor_shapes(shape):
    """Every parameter of the network a brain shape needs: name -> shape (torch's named_parameters())."""
    n = len(shape["choices"])
    kind = net_kind(shape)
    if kind == MLP:
        d = shape["d"]
        return {"fc1.weight": (128, d), "fc1.bias": (128,), "fc2.weight": (128, 128), "fc2.bias": (128,),
                "head.weight": (n, 128), "head.bias": (n,)}
    c, h, w = shape["in_c"], shape["in_h"], shape["in_w"]
    if kind == SMALL_CNN:
        h2, w2 = conv_out(h, 3, 2, 1), conv_out(w, 3, 2, 1)
        return {"conv1.weight": (16, c, 3, 3), "conv1.bias": (16,), "conv2.weight": (32, 16, 3, 3),
                "conv2.bias": (32,), "fc1.weight": (256, 32 * h2 * w2), "fc1.bias": (256,),
                "head.weight": (n, 256), "head.bias": (n,)}
    h2, w2 = conv_out(conv_out(h, 5, 5, 0), 5, 5, 0), conv_out(conv_out(w, 5, 5, 0), 5, 5, 0)
    if h2 < 1 or w2 < 1:
        raise ValueError("The DER network needs pictures of at least 25x25.")
    return {"conv1.weight": (32, c, 5, 5), "conv1.bias": (32,), "conv2.weight": (64, 32, 5, 5), "conv2.bias": (64,),
            "fc1.weight": (256, 64 * h2 * w2), "fc1.bias": (256,), "head.weight": (n, 256), "head.bias": (n,)}


def _conv2d(x, weight, bias, stride, pad):
    """x [B, C, H, W] -> [B, O, H', W'] (cross-correlation, zero padding) with im2col."""
    out_c, in_c, k, _ = weight.shape
    if pad:
        x = np.pad(x, ((0, 0), (0, 0), (pad, pad), (pad, pad)))
    batch, _, height, width = x.shape
    oh, ow = (height - k) // stride + 1, (width - k) // stride + 1
    windows = np.lib.stride_tricks.sliding_window_view(x, (k, k), axis=(2, 3))[:, :, ::stride, ::stride]
    # [B, C, oh, ow, k, k] -> columns [B, C*k*k, oh*ow], rows in weight order (c, kh, kw)
    cols = windows.transpose(0, 1, 4, 5, 2, 3).reshape(batch, in_c * k * k, oh * ow)
    out = np.matmul(weight.reshape(out_c, in_c * k * k), cols) + bias[None, :, None]
    return out.reshape(batch, out_c, oh, ow)


def _relu(x):
    return np.maximum(x, 0.0)


class QNet:
    """forward(x) -> Q-values [B, n]. MLP: x float [B, d] (normalized numbers). Picture nets: x raw 0-255 values
    [B, in_c, in_h, in_w] (or flat [B, in_c*in_h*in_w])."""

    def __init__(self, shape, tensors):
        self.shape = shape
        self.kind = net_kind(shape)
        self.n_actions = len(shape["choices"])
        self.w = {name: np.asarray(value, dtype=np.float64) for name, value in tensors.items()}

    def _linear(self, x, name):
        return x @ self.w[name + ".weight"].T + self.w[name + ".bias"]

    def forward(self, x):
        s = self.shape
        if self.kind == MLP:
            x = np.asarray(x, dtype=np.float32).reshape(-1, s["d"]).astype(np.float64)
            x = _relu(self._linear(x, "fc1"))
            x = _relu(self._linear(x, "fc2"))
            return self._linear(x, "head")
        x = np.asarray(x).reshape(-1, s["in_c"], s["in_h"], s["in_w"])
        x = (x.astype(np.float32) * _SCALE).astype(np.float64)
        if self.kind == SMALL_CNN:
            x = _relu(_conv2d(x, self.w["conv1.weight"], self.w["conv1.bias"], 1, 1))
            x = _relu(_conv2d(x, self.w["conv2.weight"], self.w["conv2.bias"], 2, 1))
        else:
            x = _relu(_conv2d(x, self.w["conv1.weight"], self.w["conv1.bias"], 5, 0))
            x = _relu(_conv2d(x, self.w["conv2.weight"], self.w["conv2.bias"], 5, 0))
        x = _relu(self._linear(x.reshape(x.shape[0], -1), "fc1"))      # flatten(1): (C, H, W) order
        return self._linear(x, "head")
