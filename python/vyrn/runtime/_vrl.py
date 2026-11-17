"""Reading a .vrl brain file without torch and without pickle (docs/dev/package.md sections 2-4).

The checks follow the C++ reader (core/src/package/brain_package.cpp, safetensors.cpp, weights.cpp): the ZIP's
first entry is the brain mimetype, only the four known entries, bounded sizes; a strict manifest (no duplicate
keys, no NaN, at most 32 levels); every field the runtime uses checked against its bounds; the weights' SHA-256;
then the safetensors header and every tensor's name and shape against the network built from the manifest's
shape, every value finite. Anything wrong is a BrainFileError with the C++ reader's plain-English message.
"""

import hashlib
import json
import math
import os
import re
import unicodedata
import zipfile

import numpy as np

from . import _nets

BRAIN_MIME = "application/x-vyrn-brain"
PROJECT_MIME = "application/x-vyrn-project"
MANIFEST = "vyrn-brain.json"
WEIGHTS = "model.safetensors"
PREVIEW = "preview.png"

MAX_FILE = 1 << 30
MAX_ENTRIES = 16
MAX_ENTRY = 64 << 20
MAX_TOTAL = 512 << 20
MAX_MANIFEST = 4 << 20
MAX_HEADER = 1 << 20
MAX_TENSORS = 64
MAX_DIM = 16_777_216
MAX_COUNTER = 10 ** 15
MAX_UPDATES = 10 ** 12
WEIGHTS_MESSAGE = "the brain's weights are damaged or don't match its description."

_TENSOR_NAME = re.compile(r"[A-Za-z0-9_.]{1,128}")
_BIDI = set("‪‫‬‭‮⁦⁧⁨⁩‎‏؜﻿")


class BrainFileError(Exception):
    """A .vrl the runtime refuses. str() is the plain-English message; .detail says exactly what was wrong."""

    def __init__(self, message, detail=""):
        super().__init__(message)
        self.message = message
        self.detail = detail or message


class _Weights(Exception):
    pass


class _Damaged(Exception):
    pass


def clean_text(value, limit=200):
    """Manifest text for display: control and text-direction characters removed, cut to `limit` characters."""
    if not isinstance(value, str):
        return ""
    kept = [ch for ch in value if ch not in _BIDI and unicodedata.category(ch) != "Cc"]
    return "".join(kept)[:limit]


def _strict_json(data, max_depth):
    def pairs(items):
        out = {}
        for key, value in items:
            if key in out:
                shown = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in key)[:40]
                raise _Damaged(f'the key "{shown}" appears twice')
            out[key] = value
        return out

    def no_constant(name):
        raise _Damaged(f"{name} isn't a number JSON allows")

    try:
        text = data.decode("utf-8")
        value = json.loads(text, object_pairs_hook=pairs, parse_constant=no_constant)
    except UnicodeDecodeError:
        raise _Damaged("it isn't UTF-8 text") from None
    except RecursionError:
        raise _Damaged("it is nested too deeply") from None
    except ValueError as err:
        if isinstance(err, json.JSONDecodeError):
            raise _Damaged(f"it isn't valid JSON (near character {err.pos})") from None
        raise
    if _depth(value) > max_depth:
        raise _Damaged(f"it is nested more than {max_depth} levels deep")
    return value


def _depth(value):
    deepest, todo = 0, [(value, 1)]
    while todo:
        item, level = todo.pop()
        if isinstance(item, (dict, list)):
            deepest = max(deepest, level)
            todo.extend((child, level + 1) for child in (item.values() if isinstance(item, dict) else item))
    return deepest


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _int_in(value, lo, hi):
    return _is_int(value) and lo <= value <= hi


def _number_in(value, lo, hi):
    return (isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)
            and lo <= value <= hi)


def _bad(label, detail):
    raise BrainFileError(f"Vyrn can't open {label}: its description is damaged ({detail.rstrip('.')}).", detail)


# -- the manifest's fields --------------------------------------------------------------------------------

def _shape(m, label):
    raw = m.get("shape")
    if not isinstance(raw, dict):
        _bad(label, '"shape" is missing')
    for key in ("in_c", "in_h", "in_w", "d", "frames"):
        if key in raw and not _int_in(raw[key], 0, 1_000_000):
            _bad(label, f"shape.{key} is wrong")
    if "small_picture" in raw and not isinstance(raw["small_picture"], bool):
        _bad(label, "shape.small_picture is wrong")
    for key in ("choices", "inputs"):
        if key in raw and (not isinstance(raw[key], list) or len(raw[key]) > 4096
                           or not all(isinstance(s, str) for s in raw[key])):
            _bad(label, f"shape.{key} is wrong")
    kind = raw.get("kind")
    if kind not in ("picture", "numbers") or not isinstance(raw.get("choices"), list):
        _bad(label, "its shape can't be read")
    shape = {"kind": kind, "in_c": raw.get("in_c", 0), "in_h": raw.get("in_h", 0), "in_w": raw.get("in_w", 0),
             "d": raw.get("d", 0), "small_picture": raw.get("small_picture", False), "frames": raw.get("frames", 1),
             "choices": list(raw["choices"]), "inputs": list(raw.get("inputs", []))}
    picture = kind == "picture"
    if picture and not (shape["in_c"] >= 1 and shape["in_h"] >= 1 and shape["in_w"] >= 1 and shape["frames"] >= 1):
        _bad(label, "its shape can't be read")
    if not picture and shape["d"] < 1:
        _bad(label, "its shape can't be read")
    # check_shape_bounds (core/src/package/common.cpp)
    choices = shape["choices"]
    if not 2 <= len(choices) <= 256:
        _bad(label, "a brain needs 2 to 256 choices.")
    for name in choices:
        if not name or len(name) > 200 or any(unicodedata.category(ch) == "Cc" for ch in name):
            _bad(label, "a choice name is empty, too long or has odd characters.")
    if len(set(choices)) != len(choices):
        _bad(label, "two choices have the same name.")
    if picture:
        frames = shape["frames"]
        if not 1 <= frames <= 8:
            _bad(label, f"it stacks {frames} frames (1 to 8 are allowed).")
        if shape["in_c"] != 3 * frames:
            _bad(label, "its picture channels don't match its frames.")
        h, w = shape["in_h"], shape["in_w"]
        if (not (1 <= h <= 32 and 1 <= w <= 32)) if shape["small_picture"] else (h != 84 or w != 84):
            _bad(label, "its picture size doesn't fit its network.")
        if shape["d"] != 0 or shape["inputs"]:
            _bad(label, "a picture brain can't also have numbers.")
    else:
        if not 1 <= shape["d"] <= 4096:
            _bad(label, f"it sees {shape['d']} numbers (1 to 4096 are allowed).")
        if shape["inputs"] and len(shape["inputs"]) != shape["d"]:
            _bad(label, "its numbers' names don't match.")
        if any(len(n) > 200 or any(unicodedata.category(ch) == "Cc" for ch in n) for n in shape["inputs"]):
            _bad(label, "a number's name is too long or odd.")
        if shape["frames"] != 1 or shape["in_c"] or shape["in_h"] or shape["in_w"] or shape["small_picture"]:
            _bad(label, "a number brain can't also have a picture size.")
    return shape


def _normalizer(value, shape, label):
    """null, or {"count", "mean": [d], "var": [d]} with finite values (var >= 0)."""
    if value is None:
        return None
    ok = (shape["kind"] == "numbers" and isinstance(value, dict) and set(value) == {"count", "mean", "var"}
          and _number_in(value["count"], 0, 1e300) and isinstance(value["mean"], list)
          and isinstance(value["var"], list) and len(value["mean"]) == shape["d"] and len(value["var"]) == shape["d"]
          and all(_number_in(x, -1e300, 1e300) for x in value["mean"])
          and all(_number_in(x, 0, 1e300) for x in value["var"]))
    if not ok:
        _bad(label, '"normalizer" is wrong')
    return {"count": float(value["count"]), "mean": [float(x) for x in value["mean"]],
            "var": [float(x) for x in value["var"]]}


def _texts(value, n):
    if isinstance(value, list) and len(value) == n and all(isinstance(s, str) for s in value):
        return [clean_text(s) for s in value]
    return None


# -- model.safetensors ------------------------------------------------------------------------------------

def decode_safetensors(data, expected_net):
    """name -> float32 array for every tensor of a vyrn-qnet safetensors file. Raises _Weights(detail)."""
    def bad(detail):
        raise _Weights(detail)

    if len(data) < 8:
        bad("the file is shorter than 8 bytes")
    header_len = int.from_bytes(data[:8], "little")
    if header_len > MAX_HEADER:
        bad("the header is longer than 1 MiB")
    if header_len > len(data) - 8:
        bad("the header is longer than the file")
    try:
        header = _strict_json(bytes(data[8:8 + header_len]), 8)
    except _Damaged as err:
        bad(f"the header: {err}")
    if not isinstance(header, dict):
        bad("the header is not a JSON object")
    meta = header.get("__metadata__")
    if not isinstance(meta, dict):
        bad("the header has no __metadata__")
    if not all(isinstance(v, str) for v in meta.values()):
        bad("__metadata__ holds something that is not text")
    if meta.get("format") != "vyrn-qnet":
        bad("__metadata__.format is not vyrn-qnet")
    if meta.get("net") != expected_net:
        bad(f"__metadata__.net is not {expected_net}")
    if len(header) - 1 > MAX_TENSORS:
        bad("more than 64 tensors")
    start = 8 + header_len
    size = len(data) - start
    tensors, expected_begin = {}, 0
    for name in sorted(key for key in header if key != "__metadata__"):
        entry = header[name]
        if not _TENSOR_NAME.fullmatch(name):
            bad("bad tensor name")
        if not isinstance(entry, dict) or set(entry) != {"dtype", "shape", "data_offsets"}:
            bad(f"tensor {name} needs exactly dtype, shape and data_offsets")
        if entry["dtype"] != "F32":
            bad(f"tensor {name} is not F32")
        dims = entry["shape"]
        if not isinstance(dims, list):
            bad(f"tensor {name} has no shape list")
        if not all(_is_int(d) for d in dims):
            bad(f"tensor {name} has a dimension that is not a whole number")
        if not 1 <= len(dims) <= 4:
            bad(f"tensor {name} has rank {len(dims)}")
        nbytes = 4
        for dim in dims:
            if not 1 <= dim <= MAX_DIM:
                bad(f"tensor {name} has a dimension of {dim}")
            nbytes *= dim
            if nbytes > size:
                bad(f"tensor {name} is bigger than the data")
        offsets = entry["data_offsets"]
        if not (isinstance(offsets, list) and len(offsets) == 2 and all(_is_int(o) and o >= 0 for o in offsets)):
            bad(f"tensor {name} has bad data_offsets")
        begin, end = offsets
        if begin != expected_begin:
            bad(f"tensor {name} doesn't start where the previous one ends")
        if end < begin or end - begin != nbytes:
            bad(f"tensor {name} has the wrong byte size")
        if end > size:
            bad(f"tensor {name} ends after the data")
        expected_begin = end
        values = np.frombuffer(data, dtype="<f4", count=nbytes // 4, offset=start + begin).reshape(dims)
        if not np.isfinite(values).all():
            bad(f"tensor {name} holds a value that is not a finite number")
        tensors[name] = values.astype(np.float32)
    if expected_begin != size:
        bad("the data has bytes no tensor uses")
    return tensors


def check_weights_fit(shape, tensors):
    """The number of parameters, after checking that the tensors are exactly the network's. Raises _Weights."""
    wanted = _nets.tensor_shapes(shape)
    if len(wanted) != len(tensors):
        raise _Weights(f"the network has {len(wanted)} tensors, the file {len(tensors)}")
    count = 0
    for name, dims in wanted.items():
        if name not in tensors:
            raise _Weights(f"tensor {name} is missing")
        if tuple(tensors[name].shape) != tuple(dims):
            raise _Weights(f"tensor {name} has the wrong shape")
        count += int(np.prod(dims))
    return count


# -- the file -----------------------------------------------------------------------------------------------

class BrainFile:
    """A checked .vrl: shape, normalizer, choices, labels, effects, some facts to show, and the weights."""

    def __init__(self, **fields):
        self.__dict__.update(fields)


def read_vrl(path, label=None):
    """Read and check a .vrl completely. Returns a BrainFile; raises BrainFileError."""
    label = label or os.path.basename(str(path))
    try:
        size = os.path.getsize(path)
    except OSError as err:
        raise BrainFileError(f"Vyrn can't open {label}: {err.strerror or err}.") from None
    if size > MAX_FILE:
        raise BrainFileError(f"Vyrn can't open {label}: it is bigger than 1 GB.")
    try:
        with zipfile.ZipFile(path) as zf:
            return _read(zf, label)
    except BrainFileError:
        raise
    except _Weights as err:
        raise BrainFileError(f"Vyrn can't open {label}: {WEIGHTS_MESSAGE}", str(err)) from None
    except (zipfile.BadZipFile, zipfile.LargeZipFile, EOFError, OSError, NotImplementedError, ValueError) as err:
        raise BrainFileError(f"Vyrn can't open {label}: it is damaged or isn't a Vyrn file.", str(err)) from None


def _read(zf, label):
    infos = zf.infolist()
    if not infos or infos[0].filename != "mimetype" or infos[0].compress_type != zipfile.ZIP_STORED \
            or infos[0].file_size > 100:
        raise BrainFileError(f"Vyrn can't open {label}: it isn't a Vyrn file.")
    mime = zf.read(infos[0]).decode("utf-8", "replace")
    if mime == PROJECT_MIME:
        raise BrainFileError(f"{label} is a whole Vyrn project (.vrlp), not a single brain (.vrl). "
                             "Open it as a project instead.")
    if mime != BRAIN_MIME:
        raise BrainFileError(f"Vyrn can't open {label}: it isn't a Vyrn brain file.")
    if len(infos) > MAX_ENTRIES:
        raise BrainFileError(f"Vyrn can't open {label}: it holds more than {MAX_ENTRIES} files.")
    seen, total = set(), 0
    for info in infos:
        if info.filename not in ("mimetype", MANIFEST, WEIGHTS, PREVIEW):
            shown = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in info.filename)[:40]
            raise BrainFileError(f'Vyrn can\'t open {label}: it holds a file a brain file can\'t have ("{shown}").')
        if info.filename.lower() in seen:
            raise BrainFileError(f"Vyrn can't open {label}: it holds the same file twice.")
        seen.add(info.filename.lower())
        if info.flag_bits & 0x61 or info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise BrainFileError(f"Vyrn can't open {label}: it is encrypted or packed in a way Vyrn can't read.")
        if info.file_size > MAX_ENTRY:
            raise BrainFileError(f"Vyrn can't open {label}: one of its files is bigger than 64 MB.")
        total += info.file_size
    if total > MAX_TOTAL:
        raise BrainFileError(f"Vyrn can't open {label}: its files are bigger than 512 MB together.")
    if MANIFEST not in seen:
        raise BrainFileError(f"Vyrn can't open {label}: its description (vyrn-brain.json) is missing.")
    if zf.getinfo(MANIFEST).file_size > MAX_MANIFEST:
        raise BrainFileError(f"Vyrn can't open {label}: its description is too big.")
    try:
        m = _strict_json(zf.read(MANIFEST), 32)
    except _Damaged as err:
        _bad(label, str(err))
    if not isinstance(m, dict):
        _bad(label, "it isn't a JSON object")
    if m.get("format") != "vyrn-brain":
        _bad(label, "it isn't a brain description")
    version = m.get("format_version")
    if not _number_in(version, -1e300, 1e300):
        _bad(label, '"format_version" is missing')
    if version >= 2:
        raise BrainFileError(f"{label} was made by a newer version of Vyrn. Please update Vyrn to open it.")
    if version < 1:
        _bad(label, '"format_version" is wrong')

    shape = _shape(m, label)
    net = _nets.net_kind(shape)
    if m.get("net") != net:
        _bad(label, "\"net\" doesn't match the brain's shape")
    if m.get("choices") != shape["choices"]:
        _bad(label, "\"choices\" doesn't match the shape")
    normalizer = _normalizer(m.get("normalizer"), shape, label)
    state = m.get("model_state")
    if not isinstance(state, dict):
        _bad(label, '"model_state" is missing')
    if not (_int_in(state.get("updates"), 0, MAX_UPDATES) and _number_in(state.get("max_abs_reward"), 0, 1e300)
            and isinstance(state.get("reward_frozen"), bool)):
        _bad(label, '"model_state" is wrong')
    counters = m.get("counters")
    if counters is not None and not isinstance(counters, dict):
        _bad(label, '"counters" is wrong')
    counters = counters or {}
    for key in ("ticks", "rounds", "updates"):
        if key in counters and not _int_in(counters[key], 0, MAX_COUNTER):
            _bad(label, f"counters.{key}")
    if "seconds" in counters and not _number_in(counters["seconds"], 0, 1e9):
        _bad(label, "counters.seconds")
    best = m.get("best_mean20")
    if best is not None and not _number_in(best, -1e300, 1e300):
        _bad(label, '"best_mean20" is wrong')

    weights = m.get("weights")
    if not (isinstance(weights, dict) and weights.get("file") == WEIGHTS and isinstance(weights.get("sha256"), str)
            and _int_in(weights.get("tensors"), 0, MAX_TENSORS) and _int_in(weights.get("parameters"), 0, MAX_COUNTER)):
        _bad(label, '"weights" is wrong')
    if WEIGHTS not in seen:
        raise BrainFileError(f"Vyrn can't open {label}: its weights (model.safetensors) are missing.")
    data = zf.read(WEIGHTS)
    if weights["sha256"].lower() != hashlib.sha256(data).hexdigest():
        raise _Weights("the sha256 doesn't match model.safetensors")
    tensors = decode_safetensors(data, net)
    parameters = check_weights_fit(shape, tensors)
    if len(tensors) != weights["tensors"] or parameters != weights["parameters"]:
        raise _Weights("the tensor or parameter count differs from the description")

    n = len(shape["choices"])
    brain = m.get("brain") if isinstance(m.get("brain"), dict) else {}
    return BrainFile(
        label=label, shape=shape, net=net, normalizer=normalizer, tensors=tensors, parameters=parameters,
        choices=list(shape["choices"]), labels=_texts(m.get("labels"), n) or list(shape["choices"]),
        effects=_texts(m.get("effects"), n) or [""] * n,
        name=clean_text(m.get("name")), project=clean_text(m.get("project")), vyrn=clean_text(m.get("vyrn"), 40),
        which="latest" if brain.get("which") == "latest" else "best",
        ticks=counters.get("ticks"), rounds=counters.get("rounds"), best_mean20=best,
        mode=m.get("mode") if m.get("mode") in ("sim", "realtime") else "")
