"""A Vyrn Link v1 server in pure Python, for the standalone runtime (docs/user/vyrn-link.md is the protocol).

Ported from tools/link/link_test_server.py and tools/link/wslite.py, with the rules of the app's C++ server
(core/src/link/link_server.cpp, docs/dev/link.md "Server hardening"):

- RFC 6455 over HTTP/1.1 (a plain HTTP request gets 426 and a one-line explanation); clients must mask their
  frames; text frames of UTF-8 JSON objects with a string "type", at most 1 MiB per message (fragments together);
  ping/pong, and a ping of our own to a game that has been quiet for 5 s; a game silent for 20 s is dropped.
- At most 8 connections waiting for their hello, 10 s each. The first message must be the hello (protocol 1).
- The 6-digit pairing code isn't needed from 127.0.0.1, except from a web page: a handshake with an Origin header
  needs the code even locally. Wrong codes: at most 5 per address and 20 in all per minute.
- One game at a time. A correct hello while another game is connected: when that game has been quiet for 1 s,
  Vyrn pings it and keeps the new hello waiting up to 1.5 s; if the old game doesn't answer, it gets {"type":"stop"}
  and close 1001, and the new one its welcome. Otherwise (it answered, or sent something during the last second)
  the new one gets "busy".
- Every refusal: {"type":"error","code":"bad_code"|"busy"|"protocol"|"bad_message","message":...}, then a close
  frame, then the game's close is awaited (up to 2 s). Types Vyrn doesn't know are ignored.

Threads: one accepts, one per connection reads (and runs that connection's timers); sending is locked per
connection, so the runtime's thread may send at any time.
"""

import base64
import hashlib
import json
import math
import os
import random
import socket
import struct
import threading
import time
from collections import deque

from ._state import VERSION

PROTOCOL = 1
DEFAULT_PORT = 7457
MAX_MESSAGE = 1 << 20
MAX_JSON_DEPTH = 64
CODE_FILE = ".vyrn-link-code"

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_CLOSE_GOING_AWAY, _CLOSE_PROTOCOL, _CLOSE_BAD_DATA, _CLOSE_POLICY, _CLOSE_TOO_BIG = 1001, 1002, 1007, 1008, 1009
_HELP = ("This is Vyrn Link. Games and mods connect here with a WebSocket, for example ws://<this computer>:<port>/ "
         "(docs/user/vyrn-link.md).\n")
_BUSY = "Another game is already connected to this project. Only one game can play at a time."


class LinkError(Exception):
    """The server can't run (for example: the port is in use)."""


class _Refuse(Exception):
    def __init__(self, code, message, close_code):
        super().__init__(message)
        self.code, self.message, self.close_code = code, message, close_code


class _Closed(Exception):
    pass


# -- pairing codes and addresses --------------------------------------------------------------------------

def valid_code(code):
    return isinstance(code, str) and len(code) == 6 and code.isdigit() and code[0] != "0"


def new_code():
    """6 digits, never a leading 0 (a mod's settings field may drop it)."""
    rng = random.SystemRandom()
    return str(rng.randint(1, 9)) + "".join(str(rng.randint(0, 9)) for _ in range(5))


def folder_code(folder):
    """The pairing code kept in <folder>/.vyrn-link-code (made when missing, mode 0600). Returns (code, saved)."""
    path = os.path.join(folder, CODE_FILE)
    try:
        if os.path.isfile(path) and not os.path.islink(path):
            with open(path, encoding="ascii", errors="replace") as file:
                text = file.read(64).strip()
            if valid_code(text):
                return text, True
    except OSError:
        pass
    code = new_code()
    try:
        tmp = path + ".part"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="ascii") as file:
            file.write(code + "\n")
        os.replace(tmp, path)
        return code, True
    except OSError:
        return code, False


def lan_addresses():
    """This computer's IPv4 addresses on its networks, the best guess first ("127.0.0.1" when there are none)."""
    found = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))          # sends nothing: only picks the outgoing interface
            found.append(s.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            if info[4][0] not in found:
                found.append(info[4][0])
    except OSError:
        pass
    found = [a for a in found if not a.startswith("127.")]
    return found or ["127.0.0.1"]


# -- WebSocket (RFC 6455, server side) --------------------------------------------------------------------

def _accept_key(key):
    return base64.b64encode(hashlib.sha1((key + _GUID).encode()).digest()).decode()


def _unmask(data, mask):
    if not data:
        return data
    n = len(data)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "big") ^ int.from_bytes(key, "big")).to_bytes(n, "big")


def _frame(opcode, payload):
    head = bytearray([0x80 | opcode])
    n = len(payload)
    if n < 126:
        head.append(n)
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    return bytes(head) + payload


def _http_response(status, extra=""):
    return (f"HTTP/1.1 {status}\r\n{extra}Content-Type: text/plain; charset=utf-8\r\nContent-Length: "
            f"{len(_HELP.encode())}\r\nConnection: close\r\n\r\n{_HELP}").encode()


def _has_token(value, token):
    return any(part.strip().lower() == token for part in (value or "").split(","))


def _parse_message(text):
    """A game's message as a dict with a string "type", or _Refuse("bad_message")."""
    def no_constant(name):
        raise ValueError(name)

    try:
        value = json.loads(text, parse_constant=no_constant)
    except RecursionError:
        raise _Refuse("bad_message", "A message from the game is nested more than 64 levels deep.",
                      _CLOSE_BAD_DATA) from None
    except ValueError as err:
        if isinstance(err, json.JSONDecodeError):
            raise _Refuse("bad_message", f"A message from the game isn't valid JSON (near byte {err.pos}).",
                          _CLOSE_BAD_DATA) from None
        raise _Refuse("bad_message", "A message from the game has a number too big to use.", _CLOSE_BAD_DATA) from None
    todo = [(value, 1)]
    while todo:
        item, depth = todo.pop()
        if isinstance(item, float) and not math.isfinite(item):
            raise _Refuse("bad_message", "A message from the game has a number too big to use.", _CLOSE_BAD_DATA)
        if isinstance(item, (dict, list)):
            if depth > MAX_JSON_DEPTH:
                raise _Refuse("bad_message", "A message from the game is nested more than 64 levels deep.",
                              _CLOSE_BAD_DATA)
            todo.extend((child, depth + 1) for child in (item.values() if isinstance(item, dict) else item))
    if not isinstance(value, dict):
        raise _Refuse("bad_message", 'Each message must be a JSON object, like {"type":"state","frame":1}.',
                      _CLOSE_BAD_DATA)
    if not isinstance(value.get("type"), str):
        raise _Refuse("bad_message", 'A message from the game has no "type", like {"type":"state", ...}.',
                      _CLOSE_BAD_DATA)
    return value


def _whole(value):
    """A JSON whole number as int (1.0 counts), else None."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and math.isfinite(value) and value == math.floor(value):
        return int(value)
    return None


class GameMessage:
    """A state or event from the game: `value` (the parsed object), `frame` (states), `seq`, `connection`."""

    __slots__ = ("connection", "seq", "frame", "value", "received")

    def __init__(self, connection, seq, frame, value, received):
        self.connection, self.seq, self.frame, self.value, self.received = connection, seq, frame, value, received


class _Conn:
    def __init__(self, server, sock, address):
        self.server = server
        self.sock = sock
        self.address = address
        self.local = address.startswith("127.")
        self.browser = False
        self.buf = bytearray()
        self.send_lock = threading.Lock()
        self.connected_at = time.monotonic()
        self.last_rx = self.connected_at
        self.last_ping = 0.0
        self.active = False
        self.closed = False
        self.number = 0
        self.name = "The game"
        self.closing = False

    # -- sending (any thread) --
    def send(self, opcode, payload):
        with self.send_lock:
            if self.closed:
                return False
            try:
                self.sock.sendall(_frame(opcode, payload))
                return True
            except OSError:
                return False

    def send_json(self, obj):
        return self.send(0x1, json.dumps(obj, separators=(",", ":"), allow_nan=False).encode())

    def close_with(self, code, reason=""):
        """Send a close frame (once); the reader then waits a moment for the game's own close."""
        if not self.closing:
            self.closing = True
            self.send(0x8, struct.pack(">H", code) + reason.encode()[:120])

    def hang_up(self):
        with self.send_lock:
            if self.closed:
                return
            self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass

    # -- receiving (the connection's own thread) --
    def _fill(self, deadline):
        """Read more bytes. False on a timeout slice (the caller runs its timers); _Closed when the peer is gone."""
        try:
            self.sock.settimeout(max(0.01, min(0.25, deadline - time.monotonic())) if deadline else 0.25)
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return False
        except OSError:
            raise _Closed() from None
        if not chunk:
            raise _Closed()
        self.buf += chunk
        self.last_rx = time.monotonic()
        return True

    def take_frame(self):
        """One complete frame from the buffer as (fin, opcode, payload), or None when more bytes are needed."""
        b = self.buf
        if len(b) < 2:
            return None
        if b[0] & 0x70:
            raise _Refuse("protocol", "A frame used reserved bits (Vyrn uses no WebSocket extensions).",
                          _CLOSE_PROTOCOL)
        fin, opcode = bool(b[0] & 0x80), b[0] & 0x0F
        if opcode not in (0x0, 0x1, 0x2, 0x8, 0x9, 0xA):
            raise _Refuse("protocol", f"A frame has the unknown type {opcode}.", _CLOSE_PROTOCOL)
        if not b[1] & 0x80:
            raise _Refuse("protocol", "A frame from the game wasn't masked (RFC 6455 section 5.1: clients mask "
                          "every frame).", _CLOSE_PROTOCOL)
        n, at = b[1] & 0x7F, 2
        if opcode >= 0x8 and (n > 125 or not fin):
            raise _Refuse("protocol", "A control frame is too long or fragmented.", _CLOSE_PROTOCOL)
        if n == 126:
            if len(b) < 4:
                return None
            n, at = struct.unpack(">H", b[2:4])[0], 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, at = struct.unpack(">Q", b[2:10])[0], 10
        if n > MAX_MESSAGE:
            raise _Refuse("protocol", "A message is bigger than 1 MiB.", _CLOSE_TOO_BIG)
        if len(b) < at + 4 + n:
            return None
        mask = bytes(b[at:at + 4])
        payload = _unmask(bytes(b[at + 4:at + 4 + n]), mask)
        del b[:at + 4 + n]
        return fin, opcode, payload


class LinkServer:
    """One project's Vyrn Link server. start(), then the runtime polls the game's state and sends it inputs."""

    def __init__(self, port=DEFAULT_PORT, code=None, *, bind="0.0.0.0", project="", buttons=(), lockstep=False,
                 frames_per_step=1, tick_hz=10.0, **limits):
        self.requested_port = port
        self.code = code or new_code()
        self.bind = bind
        self.welcome = {"project": project, "buttons": list(buttons), "lockstep": bool(lockstep),
                        "frames_per_step": int(frames_per_step), "tick_hz": tick_hz}
        self.hello_timeout = limits.get("hello_timeout", 10.0)
        self.ping_after = limits.get("ping_after", 5.0)
        self.idle_timeout = limits.get("idle_timeout", 20.0)
        self.replace_quiet = limits.get("replace_quiet", 1.0)
        self.replace_probe = limits.get("replace_probe", 1.5)
        self.max_pending = limits.get("max_pending", 8)
        self.max_events = limits.get("max_events", 1000)
        self.bad_codes_per_address = limits.get("bad_codes_per_address", 5)
        self.bad_codes_total = limits.get("bad_codes_total", 20)
        self.bad_code_window = limits.get("bad_code_window", 60.0)
        self.close_linger = limits.get("close_linger", 2.0)
        self._lock = threading.Condition()
        self._listener = None
        self._threads = []
        self._conns = []
        self._active = None           # the _Conn of the connected game
        self._waiting = None          # a _Conn waiting to take the place of a quiet game
        self._connections = 0
        self._seq = 0
        self._latest = None
        self._events = deque()
        self._notices = []
        self._bad_codes = {}
        self._bad_codes_all = deque()
        self._stopping = False
        self.port = 0
        self.stats = {"connections": 0, "states": 0, "events": 0, "inputs": 0, "refused": 0}

    # -- life -------------------------------------------------------------------------------------------
    def start(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if not hasattr(socket, "SO_EXCLUSIVEADDRUSE"):   # Windows only: SO_REUSEADDR there would share the port
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            listener.bind((self.bind, self.requested_port))
            listener.listen(16)
        except OSError as err:
            listener.close()
            raise LinkError(f"Vyrn Link can't listen on port {self.requested_port}: {err.strerror or err}. Is "
                            "another program (or another Vyrn) using it? Change \"link\": {\"port\": ...} in "
                            "vyrn.json.") from None
        listener.settimeout(0.25)
        self._listener = listener
        self.port = listener.getsockname()[1]
        self._spawn(self._accept_loop)

    def stop(self):
        """Tell the game to stop, close every connection (1001) and free the port."""
        with self._lock:
            self._stopping = True
            conns = list(self._conns)
            self._lock.notify_all()
        for conn in conns:
            if conn.active:
                conn.send_json({"type": "stop"})
            conn.close_with(_CLOSE_GOING_AWAY, "Vyrn stopped")
        deadline = time.monotonic() + 1.0
        for thread in list(self._threads):
            thread.join(max(0.0, deadline - time.monotonic()))
        for conn in conns:
            conn.hang_up()
        if self._listener is not None:
            self._listener.close()
            self._listener = None

    def _spawn(self, target, *args):
        thread = threading.Thread(target=target, args=args, name="vyrn-link", daemon=True)
        self._threads.append(thread)
        thread.start()

    def _notice(self, text):
        with self._lock:
            self._notices.append(text)

    def take_notices(self):
        """Console lines since the last call ("Fake Runner (fake 1.0) connected from 127.0.0.1.")."""
        with self._lock:
            out, self._notices = self._notices, []
            return out

    # -- the runtime's side -------------------------------------------------------------------------------
    def set_buttons(self, buttons):
        with self._lock:
            self.welcome["buttons"] = list(buttons)

    def connected(self):
        with self._lock:
            return self._active is not None

    def client(self):
        """{"game", "client", "address", "connection"} of the connected game, or None."""
        with self._lock:
            conn = self._active
            if conn is None:
                return None
            return {"game": conn.game, "client": conn.client, "address": conn.address, "connection": conn.number}

    def latest_state(self):
        with self._lock:
            return self._latest

    def drain_events(self):
        with self._lock:
            out = list(self._events)
            self._events.clear()
            return out

    def wait_state(self, newer_than_seq, timeout):
        """("ok", state) once there is a state newer than newer_than_seq; ("timeout" | "disconnected" |
        "stopped", None) otherwise."""
        deadline = time.monotonic() + timeout
        with self._lock:
            while True:
                if self._stopping:
                    return "stopped", None
                if self._active is None:
                    return "disconnected", None
                if self._latest is not None and self._latest.seq > newer_than_seq:
                    return "ok", self._latest
                left = deadline - time.monotonic()
                if left <= 0:
                    return "timeout", None
                self._lock.wait(left)

    def wait_connected(self, timeout):
        deadline = time.monotonic() + timeout
        with self._lock:
            while self._active is None and not self._stopping:
                left = deadline - time.monotonic()
                if left <= 0:
                    return False
                self._lock.wait(left)
            return self._active is not None

    def _active_conn(self):
        with self._lock:
            return self._active

    def send_input(self, frame, hold, tap, connection=None):
        """{"type":"input","frame":f,"hold":[...],"tap":[...]} to the game (only to `connection` when given)."""
        conn = self._active_conn()
        if conn is None or (connection is not None and conn.number != connection):
            return False
        sent = conn.send_json({"type": "input", "frame": frame, "hold": list(hold), "tap": list(tap)})
        if sent:
            with self._lock:
                self.stats["inputs"] += 1
        return sent

    def send_stop(self):
        conn = self._active_conn()
        return conn is not None and conn.send_json({"type": "stop"})

    def send_custom(self, message):
        """A vyrn.send() message (already checked by the worker's library) to the game."""
        conn = self._active_conn()
        if conn is None or not isinstance(message, dict) or not isinstance(message.get("type"), str):
            return False
        text = '{"type":' + json.dumps(message["type"])
        for key, value in message.items():
            if key != "type":
                text += "," + json.dumps(key) + ":" + json.dumps(value, separators=(",", ":"))
        return conn.send(0x1, (text + "}").encode())

    # -- connections ----------------------------------------------------------------------------------------
    def _accept_loop(self):
        while True:
            with self._lock:
                if self._stopping:
                    return
            try:
                sock, peer = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self._lock:
                pending = sum(1 for c in self._conns if not c.active and not c.closed)
                if pending >= self.max_pending or self._stopping:
                    self.stats["refused"] += 1
                    sock.close()
                    continue
                conn = _Conn(self, sock, peer[0])
                self._conns.append(conn)
            self._spawn(self._serve, conn)

    def _serve(self, conn):
        try:
            if self._handshake(conn):
                self._hello(conn)
                if conn.active:
                    self._play(conn)
        except _Refuse as refusal:
            self._refuse(conn, refusal)
        except _Closed:
            pass
        except Exception as err:           # never let one connection take the server down
            self._notice(f"A connection from {conn.address} failed: {err!r}.")
        finally:
            self._linger(conn)
            conn.hang_up()
            with self._lock:
                if self._active is conn:
                    self._active = None
                    self._notices.append(f"{conn.name} left.")
                if self._waiting is conn:
                    self._waiting = None
                if conn in self._conns:
                    self._conns.remove(conn)
                self._lock.notify_all()

    def _handshake(self, conn):
        deadline = conn.connected_at + self.hello_timeout
        while b"\r\n\r\n" not in conn.buf:
            if len(conn.buf) > 16384:
                conn.sock.sendall(_http_response("431 Request Header Fields Too Large"))
                return False
            if time.monotonic() >= deadline:
                return False                       # no WebSocket yet: just hang up
            conn._fill(deadline)
        head, _, rest = bytes(conn.buf).partition(b"\r\n\r\n")
        conn.buf = bytearray(rest)
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ")
        headers = {}
        for line in lines[1:]:
            name, sep, value = line.partition(":")
            if sep:
                headers[name.strip().lower()] = value.strip()
        problem = None
        if len(parts) != 3 or parts[0] != "GET" or parts[2] != "HTTP/1.1":
            problem, response = "something that isn't a WebSocket request", _http_response("400 Bad Request")
        elif not _has_token(headers.get("upgrade"), "websocket") or not _has_token(headers.get("connection"),
                                                                                    "upgrade"):
            problem = "an HTTP request without a WebSocket upgrade (a web browser?)"
            response = _http_response("426 Upgrade Required", "Upgrade: websocket\r\nConnection: Upgrade\r\n")
        elif headers.get("sec-websocket-version", "").strip() != "13":
            problem = "an unsupported WebSocket version (Vyrn speaks version 13)"
            response = _http_response("426 Upgrade Required", "Sec-WebSocket-Version: 13\r\n")
        else:
            key = headers.get("sec-websocket-key", "").strip()
            try:
                valid = len(base64.b64decode(key, validate=True)) == 16
            except ValueError:
                valid = False
            if not valid:
                problem = "a WebSocket request without a valid Sec-WebSocket-Key"
                response = _http_response("400 Bad Request")
        if problem:
            with self._lock:
                self.stats["refused"] += 1
            self._notice(f"Refused a connection from {conn.address}: {problem}.")
            try:
                conn.sock.sendall(response)
            except OSError:
                pass
            return False
        conn.browser = bool(headers.get("origin"))
        conn.sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Accept: {_accept_key(key)}\r\n\r\n").encode())
        return True

    def _next_text(self, conn, deadline=None, on_idle=None):
        """The next text message (str); None when `deadline` passed. Answers pings, skips pongs and binary."""
        parts, kind = [], None
        while True:
            frame = conn.take_frame()
            if frame is None:
                if deadline is not None and time.monotonic() >= deadline:
                    return None
                if not conn._fill(deadline) and on_idle is not None:
                    on_idle()
                continue
            fin, opcode, payload = frame
            if opcode == 0x8:
                conn.close_with(1000)
                raise _Closed()
            if opcode == 0x9:
                conn.send(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode in (0x1, 0x2):
                if kind is not None:
                    raise _Refuse("protocol", "A new message started before the last one ended.", _CLOSE_PROTOCOL)
                kind, parts = opcode, [payload]
            elif kind is None:
                raise _Refuse("protocol", "A continuation frame came without a message.", _CLOSE_PROTOCOL)
            else:
                parts.append(payload)
            if sum(len(p) for p in parts) > MAX_MESSAGE:
                raise _Refuse("protocol", "A message is bigger than 1 MiB.", _CLOSE_TOO_BIG)
            if not fin:
                continue
            data, was = b"".join(parts), kind
            parts, kind = [], None
            if was == 0x2:
                continue                           # binary: not part of Vyrn Link
            try:
                return data.decode("utf-8")
            except UnicodeDecodeError:
                raise _Refuse("bad_message", "A message from the game isn't valid UTF-8 text.",
                              _CLOSE_BAD_DATA) from None

    def _hello(self, conn):
        text = self._next_text(conn, conn.connected_at + self.hello_timeout)
        if text is None:
            raise _Refuse("protocol", 'No hello: a game must send {"type":"hello",...} first, right after connecting.',
                          _CLOSE_POLICY)
        hello = _parse_message(text)
        if hello.get("type") != "hello":
            raise _Refuse("protocol", 'The first message must be the hello: {"type":"hello","protocol":1,...}.',
                          _CLOSE_PROTOCOL)
        if "protocol" in hello and _whole(hello["protocol"]) != PROTOCOL:
            shown = json.dumps(hello["protocol"])[:40]
            raise _Refuse("protocol", f"This Vyrn speaks Vyrn Link protocol 1, and the game asked for protocol "
                          f"{shown}.", _CLOSE_PROTOCOL)
        if not conn.local or conn.browser:    # another computer, or a web page (any page can reach 127.0.0.1)
            now = time.monotonic()
            if self._too_many_bad_codes(conn.address, now):
                raise _Refuse("bad_code", "Too many wrong pairing codes. Wait a minute, then try again.",
                              _CLOSE_POLICY)
            if not self._code_matches(hello):
                with self._lock:
                    self._bad_codes.setdefault(conn.address, deque()).append(now)
                    self._bad_codes_all.append(now)
                raise _Refuse("bad_code", "Wrong pairing code: use the 6-digit code Vyrn shows for this project.",
                              _CLOSE_POLICY)
        self._take_place(conn)
        self._welcome(conn, hello)

    def _code_matches(self, hello):
        given = hello.get("code")
        if isinstance(given, str):
            given = given.replace(" ", "").replace("-", "")     # "483 920" as typed into a mod's settings
        elif isinstance(given, int) and not isinstance(given, bool) and given >= 0:
            given = str(given).rjust(len(self.code), "0")
        else:
            return False
        return given == self.code

    def _too_many_bad_codes(self, address, now):
        since = now - self.bad_code_window
        with self._lock:
            while self._bad_codes_all and self._bad_codes_all[0] < since:
                self._bad_codes_all.popleft()
            for key in list(self._bad_codes):
                times = self._bad_codes[key]
                while times and times[0] < since:
                    times.popleft()
                if not times:
                    del self._bad_codes[key]
            mine = len(self._bad_codes.get(address, ()))
            return mine >= max(1, self.bad_codes_per_address) or len(self._bad_codes_all) >= max(1, self.bad_codes_total)

    def _take_place(self, conn):
        """Return when `conn` may become the connected game; _Refuse("busy") when it may not."""
        with self._lock:
            old = self._active
            if old is None:
                return
            now = time.monotonic()
            if self._waiting is not None or now - old.last_rx < self.replace_quiet:
                raise _Refuse("busy", _BUSY, _CLOSE_POLICY)
            self._waiting = conn
            probe_at, replace_at = now, now + max(0.05, self.replace_probe)
        old.send(0x9, b"vyrn")
        old.last_ping = probe_at
        try:
            while True:
                with self._lock:
                    if self._active is not old:
                        return                     # the quiet game left by itself
                    if old.last_rx > probe_at:
                        raise _Refuse("busy", _BUSY, _CLOSE_POLICY)
                    if time.monotonic() >= replace_at:
                        self._active = None
                        self._notices.append(f"{old.name} stopped answering, so the game connecting from "
                                             f"{conn.address} takes its place.")
                        break
                    self._lock.wait(0.02)
        finally:
            with self._lock:
                if self._waiting is conn:
                    self._waiting = None
        old.send_json({"type": "stop"})
        old.close_with(_CLOSE_GOING_AWAY, "replaced by a new connection")
        old.replaced = True

    def _welcome(self, conn, hello):
        with self._lock:
            welcome = {"type": "welcome", "protocol": PROTOCOL, "vyrn": VERSION, **self.welcome}
            self._connections += 1
            conn.number = self._connections
            conn.game = str(hello.get("game") or "")
            conn.client = str(hello.get("client") or "")
            conn.name = _shown(conn.game) if conn.game else "The game"
            conn.send_json(welcome)        # before any input the runtime sends to this game
            conn.active = True
            self._active = conn
            self._latest = None
            self._events.clear()
            self.stats["connections"] += 1
            client = f" ({_shown(conn.client)})" if conn.client else ""
            self._notices.append(f"{conn.name}{client} connected from {conn.address}.")
            self._lock.notify_all()

    def _play(self, conn):
        def timers():
            now = time.monotonic()
            if getattr(conn, "replaced", False):
                raise _Closed()
            if now - conn.last_rx >= self.idle_timeout:
                self._notice(f"{conn.name} stopped answering, so Vyrn closed the connection.")
                conn.close_with(_CLOSE_GOING_AWAY, "no answer")
                raise _Closed()
            if now - conn.last_rx >= self.ping_after and now - conn.last_ping >= self.ping_after:
                conn.send(0x9, b"vyrn")
                conn.last_ping = now

        while True:
            with self._lock:
                if self._stopping or self._active is not conn:
                    return
            text = self._next_text(conn, time.monotonic() + 0.25, timers)
            if text is None:
                timers()
                continue
            message = _parse_message(text)
            kind = message["type"]
            if kind == "state":
                frame = _whole(message.get("frame"))
                if frame is None:
                    raise _Refuse("bad_message", 'A "state" needs "frame": the game\'s frame number, a whole number.',
                                  _CLOSE_BAD_DATA)
                with self._lock:
                    if self._active is not conn:
                        return
                    self._seq += 1
                    self._latest = GameMessage(conn.number, self._seq, frame, message, time.monotonic())
                    self.stats["states"] += 1
                    self._lock.notify_all()
            elif kind == "event":
                with self._lock:
                    if self._active is not conn:
                        return
                    self._seq += 1
                    self._events.append(GameMessage(conn.number, self._seq, None, message, time.monotonic()))
                    self.stats["events"] += 1
                    while len(self._events) > max(1, self.max_events):
                        self._events.popleft()
                    self._lock.notify_all()
            # anything else (a second hello, types from a newer protocol) is ignored

    def _refuse(self, conn, refusal):
        conn.send_json({"type": "error", "code": refusal.code, "message": refusal.message})
        conn.close_with(refusal.close_code, refusal.code)
        with self._lock:
            if conn.active:
                self._notices.append(f"Closed the connection to {conn.name}: {refusal.message}")
            else:
                self.stats["refused"] += 1
                self._notices.append(f"Refused a game from {conn.address}: {refusal.message}")

    def _linger(self, conn):
        """After our close frame: wait (up to close_linger) for the game's own close before hanging up."""
        if not conn.closing or conn.closed:
            return
        deadline = time.monotonic() + self.close_linger
        try:
            while time.monotonic() < deadline:
                frame = conn.take_frame()
                if frame is None:
                    conn._fill(deadline)
                elif frame[1] == 0x8:
                    return
        except (_Closed, _Refuse):
            return


def _shown(text):
    """Text from the game for a console line: printable characters only, at most 60."""
    return "".join(ch for ch in str(text) if ch.isprintable())[:60] or "?"
