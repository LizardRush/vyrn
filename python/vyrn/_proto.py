"""Frame codec shared by the worker and the pure-Python engine (protocol.md section 2).

    offset 0   4 bytes   magic "VYRN"
    offset 4   u32 LE    json_len   (1 .. 1 MiB)
    offset 8   u32 LE    bin_len    (0 .. 64 MiB)
    offset 12  json_len  UTF-8 JSON object
    then       bin_len   binary blob

numpy arrays inside a message travel in the blob and are referenced from the JSON as
{"$a": [offset, dtype, shape]} with dtype "u8" or "f32" (little-endian), C order, 64-byte aligned.
"""

import json
import math
import struct

import numpy as np

MAGIC = b"VYRN"
MAX_JSON = 1 << 20
MAX_BIN = 64 << 20
ALIGN = 64

_HEADER = struct.Struct("<4sII")
_DTYPES = {"u8": np.dtype(np.uint8), "f32": np.dtype("<f4")}


class ProtocolError(Exception):
    """A frame that breaks the protocol (bad magic, oversize, bad JSON, bad array reference...)."""


class FrameTooBig(ProtocolError):
    """A message too big for one frame. .part says which part: "json" or "data" (the blob)."""

    def __init__(self, message, part):
        super().__init__(message)
        self.part = part


# -- encoding -------------------------------------------------------------------------------------

def encode(msg):
    """Encode one message dict into a complete frame (bytes).

    numpy arrays anywhere in ``msg`` go into the blob: uint8 arrays as "u8", every other numeric
    (or bool) array as "f32". numpy scalars become plain JSON numbers. Raises FrameTooBig (a
    ProtocolError) when the frame would be too big, ValueError for values JSON can't hold (NaN,
    infinity, objects).
    """
    parts = []
    size = 0

    def put(array):
        nonlocal size
        if array.dtype == np.uint8:
            data, dtype = array, "u8"
        elif array.dtype.kind in "biuf":
            data, dtype = array.astype("<f4"), "f32"
        else:
            raise ValueError(f"can't send an array of type {array.dtype}")
        data = np.ascontiguousarray(data)
        pad = -size % ALIGN
        if pad:
            parts.append(bytes(pad))
            size += pad
        offset = size
        raw = data.tobytes()
        parts.append(raw)
        size += len(raw)
        return {"$a": [offset, dtype, [int(n) for n in data.shape]]}

    def convert(value):
        if isinstance(value, dict):
            return {key if isinstance(key, str) else str(key): convert(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [convert(item) for item in value]
        if isinstance(value, np.ndarray):
            return put(value)
        if isinstance(value, np.generic):
            return value.item()
        return value

    body = _utf8(json.dumps(convert(msg), separators=(",", ":"), ensure_ascii=False, allow_nan=False))
    if not 1 <= len(body) <= MAX_JSON:
        raise FrameTooBig(f"message JSON is {len(body)} bytes (the limit is {MAX_JSON})", "json")
    if size > MAX_BIN:
        raise FrameTooBig(f"message data is {size} bytes (the limit is {MAX_BIN})", "data")
    return b"".join([_HEADER.pack(MAGIC, len(body), size), body, *parts])


def _utf8(text):
    """text as UTF-8 bytes. A lone surrogate (from a file name or other OS data that Python decoded with
    "surrogateescape") can't be UTF-8: it becomes U+FFFD, so the text still reaches the app."""
    try:
        return text.encode("utf-8")
    except UnicodeEncodeError:
        return text.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "replace").encode("utf-8")


# -- decoding -------------------------------------------------------------------------------------

def _reject_constant(name):
    raise ProtocolError(f"JSON contains {name}, which the protocol does not allow")


def decode(json_bytes, blob):
    """Decode a frame's JSON and blob into a dict, with array references replaced by numpy arrays.

    The arrays are copies (they don't keep the receive buffer alive). Raises ProtocolError.
    """
    try:
        msg = json.loads(bytes(json_bytes).decode("utf-8"), parse_constant=_reject_constant)
    except ProtocolError:
        raise
    except (UnicodeDecodeError, ValueError, RecursionError) as err:
        raise ProtocolError(f"bad JSON in frame: {err}") from None
    if not isinstance(msg, dict):
        raise ProtocolError("a frame's JSON must be an object")
    blob = memoryview(blob).cast("B")
    return _resolve(msg, blob)


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _resolve(value, blob):
    if isinstance(value, dict):
        if "$a" in value:
            return _array(value, blob)
        return {key: _resolve(item, blob) for key, item in value.items()}
    if isinstance(value, list):
        return [_resolve(item, blob) for item in value]
    return value


def _array(ref, blob):
    spec = ref["$a"]
    if len(ref) != 1 or not isinstance(spec, list) or len(spec) != 3:
        raise ProtocolError(f"bad array reference {ref!r}")
    offset, dtype, shape = spec
    if not _is_int(offset) or offset < 0 or offset % ALIGN:
        raise ProtocolError(f"bad array offset {offset!r}")
    if dtype not in _DTYPES:
        raise ProtocolError(f"bad array dtype {dtype!r}")
    if not isinstance(shape, list) or not all(_is_int(n) and n >= 0 for n in shape):
        raise ProtocolError(f"bad array shape {shape!r}")
    count = math.prod(shape)
    nbytes = count * _DTYPES[dtype].itemsize
    if offset + nbytes > len(blob):
        raise ProtocolError(f"array [{offset}, {nbytes} bytes] is outside the {len(blob)}-byte blob")
    array = np.frombuffer(blob, dtype=_DTYPES[dtype], count=count, offset=offset)
    return array.reshape(shape).copy()


# -- sockets --------------------------------------------------------------------------------------

def send(sock, msg):
    """Encode and send one message."""
    sock.sendall(encode(msg))


def _recv_exact(sock, count, eof_ok=False):
    buf = bytearray(count)
    view = memoryview(buf)
    got = 0
    while got < count:
        n = sock.recv_into(view[got:])
        if n == 0:
            if got == 0 and eof_ok:
                return None
            raise ProtocolError("the connection closed in the middle of a message")
        got += n
    return buf


def recv(sock):
    """Receive one message. Returns None on a clean EOF before a header; ProtocolError otherwise."""
    header = _recv_exact(sock, _HEADER.size, eof_ok=True)
    if header is None:
        return None
    magic, json_len, bin_len = _HEADER.unpack(header)
    if magic != MAGIC:
        raise ProtocolError(f"bad magic {bytes(magic)!r}")
    if not 1 <= json_len <= MAX_JSON:
        raise ProtocolError(f"bad JSON length {json_len}")
    if bin_len > MAX_BIN:
        raise ProtocolError(f"bad data length {bin_len}")
    body = memoryview(_recv_exact(sock, json_len + bin_len))
    return decode(body[:json_len], body[json_len:])
