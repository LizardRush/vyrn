"""Frame codec: protocol.md section 2 and python-internals.md section 4 (vyrn._proto)."""

import json
import socket
import struct
import threading

import numpy as np
import pytest

from testkit import GOLDEN_FRAME
from vyrn import _proto

GOLDEN_JSON = b'{"t":"result","id":7,"obs":{"$a":[0,"u8",[2,2,3]]},"points":1.5,"game_over":false}'


def frame(json_bytes, blob=b"", magic=b"VYRN", json_len=None, bin_len=None):
    """Build a frame by hand, optionally lying about the lengths."""
    json_len = len(json_bytes) if json_len is None else json_len
    bin_len = len(blob) if bin_len is None else bin_len
    return magic + struct.pack("<II", json_len, bin_len) + json_bytes + blob


def golden_by_hand():
    body = json.dumps({"t": "result", "id": 7, "obs": {"$a": [0, "u8", [2, 2, 3]]}, "points": 1.5,
                       "game_over": False}, separators=(",", ":")).encode()
    assert body == GOLDEN_JSON
    return frame(body, bytes(range(12)))


def split(data):
    """(json_bytes, blob) of an encoded frame, checking the header on the way."""
    assert data[:4] == b"VYRN"
    json_len, bin_len = struct.unpack_from("<II", data, 4)
    assert len(data) == 12 + json_len + bin_len
    return data[12:12 + json_len], data[12 + json_len:]


def pair():
    a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    a.settimeout(5)
    b.settimeout(5)
    return a, b


def recv_bytes(data, close=True, chunk=None):
    """Feed `data` into a socket (optionally in small chunks) and return what _proto.recv makes of it."""
    a, b = pair()
    try:
        def feed():
            if chunk:
                for i in range(0, len(data), chunk):
                    a.sendall(data[i:i + chunk])
            else:
                a.sendall(data)
            if close:
                a.shutdown(socket.SHUT_WR)

        writer = threading.Thread(target=feed, daemon=True)
        writer.start()
        try:
            return _proto.recv(b)
        finally:
            writer.join(5)
    finally:
        a.close()
        b.close()


# ---- the golden vector ------------------------------------------------------------------------------------------

def test_constants():
    assert _proto.MAGIC == b"VYRN"
    assert _proto.MAX_JSON == 1_048_576
    assert _proto.MAX_BIN == 67_108_864


def test_golden_file_matches_hand_built_frame():
    assert GOLDEN_FRAME.read_bytes() == golden_by_hand()


def test_encode_produces_golden_frame():
    obs = np.arange(12, dtype=np.uint8).reshape(2, 2, 3)
    encoded = _proto.encode({"t": "result", "id": 7, "obs": obs, "points": 1.5, "game_over": False})
    assert encoded == golden_by_hand()


def test_decode_golden_frame():
    json_bytes, blob = split(GOLDEN_FRAME.read_bytes())
    msg = _proto.decode(json_bytes, blob)
    assert list(msg) == ["t", "id", "obs", "points", "game_over"]
    assert msg["t"] == "result" and msg["id"] == 7 and msg["points"] == 1.5 and msg["game_over"] is False
    assert isinstance(msg["obs"], np.ndarray)
    assert msg["obs"].dtype == np.uint8 and msg["obs"].shape == (2, 2, 3)
    assert msg["obs"].tobytes() == bytes(range(12))


@pytest.mark.parametrize("chunk", [None, 1, 7], ids=["whole", "byte-by-byte", "chunks-of-7"])
def test_recv_golden_frame(chunk):
    msg = recv_bytes(GOLDEN_FRAME.read_bytes(), chunk=chunk)
    assert msg["id"] == 7 and msg["obs"].tobytes() == bytes(range(12))


# ---- encoding rules -----------------------------------------------------------------------------------------------

def test_arrays_are_64_byte_aligned_and_zero_padded():
    arrays = [np.arange(5, dtype=np.uint8), np.array([1.5, -2.0, 3.25], dtype=np.float32),
              np.full((2, 2, 3), 7, dtype=np.uint8)]
    json_bytes, blob = split(_proto.encode({"t": "x", "id": 1, "a": arrays[0], "b": arrays[1], "c": arrays[2]}))
    refs = json.loads(json_bytes)
    placed = sorted(((refs[key]["$a"], array, dtype) for key, array, dtype in zip("abc", arrays, ["u8", "f32", "u8"])),
                    key=lambda item: item[0][0])  # in blob order
    end = 0
    for (offset, kind, shape), array, dtype in placed:
        assert offset % 64 == 0 and offset >= end, refs
        assert kind == dtype and shape == list(array.shape)
        assert blob[end:offset] == bytes(offset - end), "padding must be zero bytes"
        assert blob[offset:offset + array.nbytes] == array.astype(array.dtype.newbyteorder("<")).tobytes()
        end = offset + array.nbytes
    assert len(blob) == end, "no bytes after the last array (like the golden frame)"


def test_float_arrays_travel_as_little_endian_f32():
    big_endian = np.array([1.0, -0.5], dtype=">f4")
    json_bytes, blob = split(_proto.encode({"t": "x", "id": 1, "v": big_endian, "w": np.array([0.25, 8.0])}))
    refs = json.loads(json_bytes)
    assert refs["v"]["$a"][1] == "f32" and refs["w"]["$a"][1] == "f32"
    offset = refs["v"]["$a"][0]
    assert blob[offset:offset + 8] == struct.pack("<ff", 1.0, -0.5)
    msg = _proto.decode(json_bytes, blob)
    assert msg["w"].dtype == np.float32 and msg["w"].tolist() == [0.25, 8.0]


def test_non_contiguous_and_fortran_arrays_are_sent_in_c_order():
    strided = np.arange(48, dtype=np.uint8).reshape(4, 4, 3)[:, ::2]
    fortran = np.asfortranarray(np.arange(6, dtype=np.uint8).reshape(2, 3))
    msg = _proto.decode(*split(_proto.encode({"t": "x", "id": 1, "s": strided, "f": fortran})))
    np.testing.assert_array_equal(msg["s"], strided)
    np.testing.assert_array_equal(msg["f"], fortran)


def test_arrays_nested_in_dicts_and_lists_round_trip():
    view = np.arange(12, dtype=np.uint8).reshape(2, 2, 3)
    numbers = np.array([1, 2, 3], dtype=np.float32)
    original = {"t": "result", "id": 9, "show": {"view": view, "speed": 3.5},
                "items": [numbers, "text", None, True], "final_obs": None}
    msg = _proto.decode(*split(_proto.encode(original)))
    np.testing.assert_array_equal(msg["show"]["view"], view)
    np.testing.assert_array_equal(msg["items"][0], numbers)
    assert msg["show"]["speed"] == 3.5 and msg["items"][1:] == ["text", None, True] and msg["final_obs"] is None


def test_json_is_compact_utf8():
    json_bytes, blob = split(_proto.encode({"t": "log", "id": 3, "text": "héllo ✓", "list": [1, 2]}))
    assert blob == b""
    assert json_bytes == '{"t":"log","id":3,"text":"héllo ✓","list":[1,2]}'.encode("utf-8")


def test_encoder_refuses_nan():
    with pytest.raises((ValueError, _proto.ProtocolError)):
        _proto.encode({"t": "result", "id": 1, "points": float("nan")})


def test_non_u8_integer_and_bool_arrays_travel_as_f32():
    # python-internals.md section 4: bool/int arrays are u8 only if they already are u8; the rest become f32.
    json_bytes, blob = split(_proto.encode({"t": "x", "id": 1, "b": np.array([True, False]),
                                            "i": np.array([3, -2], dtype=np.int64)}))
    refs = json.loads(json_bytes)
    assert refs["b"]["$a"][1] == "f32" and refs["i"]["$a"][1] == "f32"
    msg = _proto.decode(json_bytes, blob)
    assert msg["b"].tolist() == [1.0, 0.0] and msg["i"].tolist() == [3.0, -2.0]


@pytest.mark.parametrize("message, part", [
    pytest.param({"t": "x", "id": 1, "pad": "a" * _proto.MAX_JSON}, "json", id="json"),
    pytest.param({"t": "x", "id": 1, "a": np.zeros(_proto.MAX_BIN + 1, dtype=np.uint8)}, "data", id="data"),
])
def test_encoder_refuses_frames_that_are_too_big(message, part):
    with pytest.raises(_proto.FrameTooBig) as info:
        _proto.encode(message)
    assert isinstance(info.value, _proto.ProtocolError) and info.value.part == part


def test_decoded_arrays_are_writable_copies():
    json_bytes, blob = split(GOLDEN_FRAME.read_bytes())
    blob = bytearray(blob)
    msg = _proto.decode(json_bytes, blob)
    blob[0] = 99
    assert msg["obs"].flat[0] == 0, "decode must copy out of the blob"
    msg["obs"][0, 0, 0] = 5  # writable


def test_send_and_recv_a_large_frame():
    picture = np.random.default_rng(1).integers(0, 256, size=(720, 1280, 3), dtype=np.uint8)
    numbers = np.linspace(0, 1, 4096, dtype=np.float32)
    a, b = pair()
    try:
        writer = threading.Thread(target=_proto.send, args=(a, {"t": "result", "id": 2, "obs": picture,
                                                                "numbers": numbers}), daemon=True)
        writer.start()
        msg = _proto.recv(b)
        writer.join(5)
    finally:
        a.close()
        b.close()
    np.testing.assert_array_equal(msg["obs"], picture)
    np.testing.assert_array_equal(msg["numbers"], numbers)


def test_json_of_exactly_max_size_is_accepted():
    head = b'{"t":"x","id":1,"pad":"'
    tail = b'"}'
    body = head + b"a" * (_proto.MAX_JSON - len(head) - len(tail)) + tail
    assert len(body) == _proto.MAX_JSON
    assert recv_bytes(frame(body))["id"] == 1


# ---- malformed frames ---------------------------------------------------------------------------------------------

def test_clean_eof_before_a_header_is_none():
    assert recv_bytes(b"") is None


@pytest.mark.parametrize("data", [
    pytest.param(b"VYR", id="eof-in-magic"),
    pytest.param(b"VYRN\x10\x00\x00", id="eof-in-header"),
    pytest.param(frame(b'{"t":"x","id":1}')[:-3], id="eof-in-json"),
    pytest.param(frame(b'{"t":"x","id":1}', bytes(10))[:-1], id="eof-in-blob"),
])
def test_truncated_frames_are_protocol_errors(data):
    with pytest.raises(_proto.ProtocolError):
        recv_bytes(data)


@pytest.mark.parametrize("data", [
    pytest.param(frame(b'{"t":"x","id":1}', magic=b"VYRX"), id="bad-magic"),
    pytest.param(frame(b'{"t":"x","id":1}', magic=b"vyrn"), id="lowercase-magic"),
    pytest.param(frame(b"", json_len=0), id="empty-json"),
    pytest.param(b"VYRN" + struct.pack("<II", _proto.MAX_JSON + 1, 0), id="json-too-big"),
    pytest.param(b"VYRN" + struct.pack("<II", 2, _proto.MAX_BIN + 1), id="blob-too-big"),
    pytest.param(b"VYRN" + struct.pack("<II", 0xFFFFFFFF, 0xFFFFFFFF), id="huge-lengths"),
])
def test_bad_headers_are_rejected_without_waiting_for_a_body(data):
    # The socket stays open: a reader that trusted the lengths would block here (5 s timeout -> failure).
    with pytest.raises(_proto.ProtocolError):
        recv_bytes(data, close=False)


@pytest.mark.parametrize("json_bytes", [
    pytest.param(b"{not json", id="invalid-json"),
    pytest.param(b'{"t":"x","id":1', id="unterminated"),
    pytest.param(b'{"t":"\xff\xfe","id":1}', id="invalid-utf8"),
    pytest.param(b"[1,2,3]", id="not-an-object"),
    pytest.param(b'"text"', id="a-string"),
])
def test_bad_json_is_a_protocol_error(json_bytes):
    with pytest.raises(_proto.ProtocolError):
        _proto.decode(json_bytes, b"")
    with pytest.raises(_proto.ProtocolError):
        recv_bytes(frame(json_bytes))


@pytest.mark.parametrize("ref", [
    pytest.param([0, "u8", [13]], id="longer-than-blob"),
    pytest.param([64, "u8", [1]], id="offset-past-blob"),
    pytest.param([-64, "u8", [1]], id="negative-offset"),
    pytest.param([0, "f32", [4]], id="f32-longer-than-blob"),
    pytest.param([0, "f64", [1]], id="unknown-dtype"),
    pytest.param([0, "u8", [-1]], id="negative-dimension"),
    pytest.param([0, "u8"], id="missing-shape"),
    pytest.param("0,u8,[12]", id="not-a-list"),
])
def test_bad_array_references_are_protocol_errors(ref):
    json_bytes = json.dumps({"t": "result", "id": 1, "obs": {"$a": ref}}).encode()
    with pytest.raises(_proto.ProtocolError):
        _proto.decode(json_bytes, bytes(range(12)))
    with pytest.raises(_proto.ProtocolError):
        recv_bytes(frame(json_bytes, bytes(range(12))))


def test_unknown_keys_are_not_an_error():
    msg = recv_bytes(frame(b'{"t":"result","id":4,"future_field":{"x":[1,2]}}'))
    assert msg["t"] == "result" and msg["id"] == 4


def test_lone_surrogates_become_replacement_characters():
    # Text from file names decoded with "surrogateescape" can't be UTF-8 as it is; the frame must still be valid.
    frame = _proto.encode({"t": "result", "id": 1, "show": {"name\udce9": "caf\udce9 😀"}})
    json_len = struct.unpack_from("<I", frame, 4)[0]
    text = frame[12:12 + json_len].decode("utf-8")      # strict: raises on anything that isn't UTF-8
    assert json.loads(text)["show"] == {"name�": "caf� \U0001F600"}
