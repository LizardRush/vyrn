"""Write (or check) tests/golden/frame_result.bin, the protocol's golden frame (protocol.md section 2).

The frame encodes
    {"t":"result","id":7,"obs":{"$a":[0,"u8",[2,2,3]]},"points":1.5,"game_over":false}
with blob = bytes 0..11. It is produced by vyrn._proto.encode and checked byte for byte against a
version built by hand with struct.pack, then decoded back. The C++ tests read the same file.

    python3 tests/golden/make_golden.py           write the file (after checking the encoder)
    python3 tests/golden/make_golden.py --check   only check that the file on disk is right
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True       # no __pycache__ in python/vyrn
sys.path.insert(0, os.path.join(REPO, "python"))

import numpy as np  # noqa: E402

from vyrn import _proto  # noqa: E402

GOLDEN = os.path.join(HERE, "frame_result.bin")


def by_hand():
    """The golden frame built without the encoder: magic, two little-endian u32 lengths, JSON, blob."""
    body = b'{"t":"result","id":7,"obs":{"$a":[0,"u8",[2,2,3]]},"points":1.5,"game_over":false}'
    blob = bytes(range(12))
    return b"VYRN" + struct.pack("<II", len(body), len(blob)) + body + blob


def by_encoder():
    obs = np.arange(12, dtype=np.uint8).reshape(2, 2, 3)
    return _proto.encode({"t": "result", "id": 7, "obs": obs, "points": 1.5, "game_over": False})


def check_frame(frame):
    """Raise AssertionError unless frame is exactly the golden frame and decodes back correctly."""
    expected = by_hand()
    assert frame == expected, f"frame differs from the hand-built one:\n{frame!r}\n{expected!r}"
    json_len, bin_len = struct.unpack_from("<II", frame, 4)
    msg = _proto.decode(frame[12:12 + json_len], frame[12 + json_len:12 + json_len + bin_len])
    assert list(msg) == ["t", "id", "obs", "points", "game_over"], list(msg)
    assert msg["t"] == "result" and msg["id"] == 7 and msg["points"] == 1.5 and msg["game_over"] is False
    assert msg["obs"].dtype == np.uint8 and msg["obs"].shape == (2, 2, 3)
    assert msg["obs"].tobytes() == bytes(range(12))


def main(argv):
    if argv[1:] == ["--check"]:
        with open(GOLDEN, "rb") as file:
            check_frame(file.read())
        print(f"ok: {GOLDEN} matches the protocol")
        return 0
    if argv[1:]:
        print(__doc__)
        return 2
    frame = by_encoder()
    check_frame(frame)
    with open(GOLDEN, "wb") as file:
        file.write(frame)
    print(f"wrote {GOLDEN} ({len(frame)} bytes), identical to the hand-built frame")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
