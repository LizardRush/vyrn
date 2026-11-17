"""Tiny RFC 6455 WebSocket helpers (Python standard library only).

Enough of the protocol for the Vyrn Link tools: the HTTP upgrade handshake (server and client), text messages
(fragmented or not), ping/pong and close. Binary messages are ignored. Not a general-purpose library.
"""
import base64
import hashlib
import json
import os
import socket
import struct
import threading
from urllib.parse import urlparse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MESSAGE = 1 << 20  # 1 MiB, the Vyrn Link limit


class WSClosed(Exception):
    """The other side closed the connection."""


class WSError(Exception):
    """The other side broke the WebSocket protocol."""


def accept_key(key: str) -> str:
    """The Sec-WebSocket-Accept value for a Sec-WebSocket-Key."""
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


def _xor_mask(data: bytes, mask: bytes) -> bytes:
    if not data:
        return data
    n = len(data)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "big") ^ int.from_bytes(key, "big")).to_bytes(n, "big")


def _read_http_head(sock, limit=16384):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise WSClosed("connection closed during the handshake")
        data += chunk
        if len(data) > limit:
            raise WSError("handshake too large")
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
    return lines[0], headers, rest


class WebSocket:
    """One WebSocket connection. Sending is thread-safe; receive from one thread only."""

    def __init__(self, sock: socket.socket, is_client: bool, leftover: bytes = b""):
        self.sock = sock
        self.is_client = is_client
        self.closed = False
        self._buf = bytearray(leftover)
        self._send_lock = threading.Lock()

    def peer(self):
        try:
            return self.sock.getpeername()
        except OSError:
            return ("?", 0)

    # ---- sending ------------------------------------------------------------------------------------
    def _send_frame(self, opcode: int, payload: bytes) -> None:
        header = bytearray([0x80 | opcode])
        mask_bit = 0x80 if self.is_client else 0x00
        n = len(payload)
        if n < 126:
            header.append(mask_bit | n)
        elif n < 65536:
            header.append(mask_bit | 126)
            header += struct.pack(">H", n)
        else:
            header.append(mask_bit | 127)
            header += struct.pack(">Q", n)
        if self.is_client:  # clients must mask every frame
            mask = os.urandom(4)
            header += mask
            payload = _xor_mask(payload, mask)
        with self._send_lock:
            self.sock.sendall(bytes(header) + payload)

    def send_text(self, text: str) -> None:
        self._send_frame(0x1, text.encode("utf-8"))

    def send_json(self, obj) -> None:
        self.send_text(json.dumps(obj, separators=(",", ":")))

    # ---- receiving ----------------------------------------------------------------------------------
    def _recv_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                self.closed = True
                raise WSClosed("connection closed")
            self._buf += chunk
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def recv_text(self):
        """Return the next text message, or None when the other side closed cleanly."""
        fragments, kind = [], None
        while True:
            b1, b2 = self._recv_exact(2)
            fin, opcode = b1 & 0x80, b1 & 0x0F
            masked, n = b2 & 0x80, b2 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._recv_exact(8))[0]
            if n > MAX_MESSAGE:
                raise WSError("message larger than 1 MiB")
            mask = self._recv_exact(4) if masked else None
            payload = self._recv_exact(n)
            if mask:
                payload = _xor_mask(payload, mask)
            if opcode == 0x8:  # close: answer and stop
                try:
                    self._send_frame(0x8, payload[:2])
                except OSError:
                    pass
                self.closed = True
                return None
            if opcode == 0x9:  # ping
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode in (0x1, 0x2):
                kind, fragments = opcode, [payload]
            elif opcode == 0x0 and kind is not None:
                fragments.append(payload)
            else:
                raise WSError(f"unexpected frame opcode {opcode}")
            if sum(len(f) for f in fragments) > MAX_MESSAGE:
                raise WSError("message larger than 1 MiB")
            if fin:
                if kind == 0x2:  # binary: not part of Vyrn Link, ignore
                    fragments, kind = [], None
                    continue
                return b"".join(fragments).decode("utf-8")

    def recv_json(self):
        text = self.recv_text()
        return None if text is None else json.loads(text)

    def close(self, code: int = 1000) -> None:
        if not self.closed:
            try:
                self._send_frame(0x8, struct.pack(">H", code))
            except OSError:
                pass
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass


def server_handshake(sock: socket.socket) -> WebSocket:
    """Answer an incoming HTTP upgrade request on an accepted socket."""
    request_line, headers, rest = _read_http_head(sock)
    key = headers.get("sec-websocket-key")
    if not request_line.startswith("GET ") or headers.get("upgrade", "").lower() != "websocket" or not key:
        sock.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        raise WSError("not a WebSocket request")
    sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Accept: {accept_key(key)}\r\n\r\n").encode())
    return WebSocket(sock, is_client=False, leftover=rest)


def connect(url: str, timeout: float = 5.0) -> WebSocket:
    """Open a client connection to ws://host:port/path."""
    u = urlparse(url)
    if u.scheme != "ws":
        raise WSError("only ws:// URLs are supported")
    host, port, path = u.hostname, u.port or 80, (u.path or "/") + (("?" + u.query) if u.query else "")
    sock = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    status, headers, rest = _read_http_head(sock)
    if " 101 " not in status + " " or headers.get("sec-websocket-accept") != accept_key(key):
        sock.close()
        raise WSError(f"server refused the WebSocket upgrade: {status}")
    sock.settimeout(None)
    return WebSocket(sock, is_client=True, leftover=rest)
