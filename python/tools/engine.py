"""A pure-Python stand-in for the Vyrn app's side of the worker protocol (docs/dev/protocol.md).

Used by the tests and by check.py. It starts vyrn_worker.py the way the app does on this system
(docs/dev/platforms.md section 1) and mirrors the app's timeout handling:

- transport "fd" (Linux, macOS): posix_spawn with the socket on fd 3, its own session and process group;
  a timeout sends SIGINT to the worker pid, then SIGKILL to its group.
- transport "tcp" (Windows; the tests also select it on Linux): a listener on 127.0.0.1 and a random token in
  VYRN_ADDR/VYRN_TOKEN; the worker connects "main" (the frames) and "ctl" (control bytes: "I" interrupts, "F"
  freezes, closing it means the app died). A timeout sends "I", then kills the worker.

stdout/stderr are drained by reader threads in both cases.

    with Worker("templates/catch-numbers") as w:
        spec = w.load(seed=1)
        result = w.request("reset")
        result = w.request("step", a=0)
"""

import codecs
import hmac
import math
import os
import secrets
import select
import signal
import socket
import subprocess
import sys
import threading
import time

try:
    import fcntl                # POSIX only: moving the child's fds above 9 (transport "fd")
except ImportError:
    fcntl = None

PYTHON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.dirname(PYTHON_DIR)          # the folder that contains python/
WORKER = os.path.join(PYTHON_DIR, "vyrn_worker.py")

if PYTHON_DIR not in sys.path:
    sys.path.insert(0, PYTHON_DIR)

from vyrn import _platform, _proto  # noqa: E402  (needs PYTHON_DIR on sys.path)

REPLY_TYPES = frozenset({"spec", "result", "ok", "bye", "error"})
OUTPUT_LIMIT = 32 << 20          # keep at most this many characters of each output stream


class WorkerError(Exception):
    """The worker answered with an "error" reply. The error dict is .error."""

    def __init__(self, error):
        self.error = error
        super().__init__(format_error(error))


class WorkerDied(Exception):
    """The worker stopped, broke the protocol, or had to be killed. .stderr has its last lines."""

    def __init__(self, message, stderr=""):
        self.stderr = stderr
        text = message
        if stderr.strip():
            text += "\nLast lines Python printed:\n" + stderr.rstrip()
        super().__init__(text)


def format_error(error):
    """One line for an error reply: "points.py:7: name 'scor' is not defined"."""
    script, line = error.get("script") or "", error.get("line") or 0
    where = f"{script}:{line}" if script and line else (script or "Vyrn")
    return f"{where}: {error.get('message', '')}"


class _Output:
    """Thread-safe text collector for one output stream."""

    def __init__(self):
        self._chunks = []
        self._size = 0
        self._dropped = 0
        self._lock = threading.Lock()

    def add(self, text):
        with self._lock:
            if self._size + len(text) > OUTPUT_LIMIT:
                self._dropped += len(text)
                return
            self._chunks.append(text)
            self._size += len(text)

    def text(self):
        with self._lock:
            text = "".join(self._chunks)
            dropped = self._dropped
        if dropped:
            text += f"\n[{dropped} more characters were dropped]\n"
        return text


class Worker:
    """One worker process for a project, driven like the app drives it."""

    def __init__(self, project_dir, python=sys.executable, env=None, *, hello_timeout=15.0,
                 interrupt_grace=2.0, on_output=None, transport=None):
        """Start the worker and read its hello.

        env: extra environment variables (a value of None removes the variable).
        interrupt_grace: after a timeout's interrupt, how long to wait for the "timeout" error.
        on_output(stream, text): called from a reader thread for user output ("stdout"/"stderr").
        transport: "fd" or "tcp" (default: this system's, vyrn._platform.TRANSPORT). "tcp" on a POSIX system also
        sets VYRN_TRANSPORT=tcp, so the worker runs the Windows transport there (tests).
        """
        self.project_dir = os.path.abspath(project_dir)
        self.transport = transport or _platform.TRANSPORT
        if self.transport not in ("fd", "tcp"):
            raise ValueError(f"unknown transport {self.transport!r}")
        self.ctl = None                   # the control connection (transport "tcp")
        self._proc = None                 # the subprocess.Popen (where there is no posix_spawn)
        self.interrupt_grace = interrupt_grace
        self.on_output = on_output
        self.returncode = None
        self.interrupted = False          # a request timed out and the worker got SIGINT
        self.hello = None
        self._next_id = 0
        self._streams = {"stdout": _Output(), "stderr": _Output()}

        environment = dict(os.environ)
        environment.update({"VYRN_FD": "3", "VYRN_DATA": DATA_DIR, "OMP_NUM_THREADS": "1",
                            "OPENBLAS_NUM_THREADS": "1", "PYTHONDONTWRITEBYTECODE": "1"})
        for key, value in (env or {}).items():
            if value is None:
                environment.pop(key, None)
            else:
                environment[key] = str(value)

        argv = [python, "-u", "-X", "utf8", WORKER, self.project_dir]
        if self.transport == "fd":
            parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                out_r, err_r = self._spawn(python, argv, environment, child.fileno())
            except BaseException:
                parent.close()
                raise
            finally:
                child.close()
            self.sock = parent
            self._start_readers(out_r, err_r)
        else:
            self._start_tcp(python, argv, environment, hello_timeout)

        try:
            hello = self._read(hello_timeout, expect_id=0, interrupt=False, types=("hello",))
        except BaseException:
            self.kill()
            raise
        if hello.get("proto") != 1:
            self.kill()
            raise WorkerDied(f"library/app version mismatch (worker speaks protocol {hello.get('proto')!r})")
        self.hello = hello

    def _spawn(self, python, argv, environment, sock_fd=None):
        """Start the worker with stdin = /dev/null, stdout/stderr = new pipes and (sock_fd given) that socket on fd 3,
        in its own session. Returns the read ends of the two pipes."""
        if sock_fd is not None and not hasattr(os, "posix_spawn"):
            raise OSError("the worker can only get a socket on fd 3 on Linux and macOS (use transport 'tcp')")
        out_r, out_w = os.pipe()
        err_r, err_w = os.pipe()
        if not hasattr(os, "posix_spawn"):                 # Windows: no fd 3, a console-less child
            try:
                flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
                self._proc = subprocess.Popen(argv, executable=python, env=environment, stdin=subprocess.DEVNULL,
                                              stdout=out_w, stderr=err_w, creationflags=flags)
            except BaseException:
                for fd in (out_r, err_r):
                    os.close(fd)
                raise
            finally:
                for fd in (out_w, err_w):
                    os.close(fd)
            self.pid = self._proc.pid
            return out_r, err_r
        high = []
        try:
            # Move the child's ends above fd 9, so setting up fds 0-3 can't clobber one of them.
            ends = ([sock_fd] if sock_fd is not None else []) + [out_w, err_w]
            high = [fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 10) for fd in ends]
            actions = [(os.POSIX_SPAWN_DUP2, high[0], 3)] if sock_fd is not None else []
            actions += [(os.POSIX_SPAWN_DUP2, high[-2], 1),
                        (os.POSIX_SPAWN_DUP2, high[-1], 2),
                        (os.POSIX_SPAWN_OPEN, 0, os.devnull, os.O_RDONLY, 0)]
            if hasattr(os, "POSIX_SPAWN_CLOSEFROM"):
                actions.append((os.POSIX_SPAWN_CLOSEFROM, 4 if sock_fd is not None else 3))
            spawn = os.posix_spawn if os.sep in python else os.posix_spawnp
            options = dict(file_actions=actions, setsigmask=(),
                           setsigdef=(signal.SIGINT, signal.SIGTERM, signal.SIGUSR1, signal.SIGHUP))
            try:
                self.pid = spawn(python, argv, environment, setsid=True, **options)
            except NotImplementedError:          # no POSIX_SPAWN_SETSID (older macOS): its own process group will do
                self.pid = spawn(python, argv, environment, setpgroup=0, **options)
        except BaseException:
            for fd in (out_r, err_r):
                os.close(fd)
            raise
        finally:
            for fd in high + [out_w, err_w]:
                os.close(fd)
        return out_r, err_r

    def _start_readers(self, out_r, err_r):
        self._readers = [threading.Thread(target=self._drain, args=(fd, name), daemon=True,
                                          name=f"vyrn-engine-{name}")
                         for fd, name in ((out_r, "stdout"), (err_r, "stderr"))]
        for reader in self._readers:
            reader.start()

    def _start_tcp(self, python, argv, environment, timeout):
        """Like the Windows app: listen on 127.0.0.1, start the worker with VYRN_ADDR + VYRN_TOKEN, accept exactly
        its "main" and "ctl" connections (anything else is closed), then close the listener."""
        token = secrets.token_hex(16)
        listener = socket.create_server(("127.0.0.1", 0))
        try:
            environment.pop("VYRN_FD", None)
            environment.update({"VYRN_ADDR": f"127.0.0.1:{listener.getsockname()[1]}", "VYRN_TOKEN": token})
            if _platform.TRANSPORT != "tcp":
                environment["VYRN_TRANSPORT"] = "tcp"
            self._start_readers(*self._spawn(python, argv, environment))
            try:
                self.sock, self.ctl = self._accept(listener, token, time.monotonic() + timeout)
            except BaseException:
                self.kill()
                raise
        finally:
            listener.close()

    def _accept(self, listener, token, deadline):
        conns = {}
        listener.settimeout(0.05)
        try:
            while len(conns) < 2:
                if time.monotonic() >= deadline:
                    raise WorkerDied("Python didn't connect to Vyrn in time", self.stderr_tail())
                if self.wait(0) is not None:
                    for reader in self._readers:
                        reader.join(timeout=1.0)       # its last words
                    raise WorkerDied(f"Python stopped (exit status {self.returncode}) before it connected",
                                     self.stderr_tail())
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    continue
                role = self._hello_line(conn, token, deadline)
                if role in ("main", "ctl") and role not in conns:
                    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    conns[role] = conn
                else:
                    conn.close()               # wrong token, a repeated role, or garbage
        except BaseException:
            for conn in conns.values():
                conn.close()
            raise
        return conns["main"], conns["ctl"]

    @staticmethod
    def _hello_line(conn, token, deadline):
        """The role ("main"/"ctl") from a connection's first line "<token> <role>\n", or None. Reads byte by byte,
        so nothing after the line (the first frame) is consumed."""
        conn.settimeout(max(0.01, deadline - time.monotonic()))
        line = b""
        try:
            while not line.endswith(b"\n") and len(line) <= 200:
                byte = conn.recv(1)
                if not byte:
                    return None
                line += byte
        except OSError:
            return None
        finally:
            if conn.fileno() >= 0:
                conn.settimeout(None)
        parts = line.decode("ascii", "replace").split()
        if len(parts) != 2 or not hmac.compare_digest(parts[0].encode(), token.encode()):
            return None
        return parts[1]

    # -- requests ------------------------------------------------------------------------------

    def request(self, t, timeout=30.0, **fields):
        """Send one request and return its reply (arrays as numpy). Raises WorkerError on an error reply.

        Like the app: when no reply comes within timeout, the worker gets SIGINT and has
        interrupt_grace seconds to send its "timeout" error; otherwise its group is killed (WorkerDied).
        """
        self._next_id += 1
        msg = {"t": t, "id": self._next_id}
        msg.update(fields)
        self._send(msg)
        reply = self._read(timeout, expect_id=self._next_id, interrupt=True)
        if reply["t"] == "error":
            raise WorkerError(reply)
        return reply

    def load(self, **fields):
        """Send "load" with the app's defaults (overridable) and return the spec."""
        msg = {"project": self.project_dir, "check": False, "allow_input": False, "seed": 0,
               "round_limit": 1000, "decisions_per_second": 10.0, "focus_helper": None}
        msg.update(fields)
        return self.request("load", **msg)

    def reset(self, timeout=30.0):
        return self.request("reset", timeout=timeout)

    def step(self, a, timeout=5.0):
        return self.request("step", timeout=timeout, a=a)

    def observe(self, first=False, timeout=5.0):
        return self.request("observe", timeout=timeout, first=first)

    def act(self, a, timeout=5.0):
        return self.request("act", timeout=timeout, a=a)

    # -- process control ---------------------------------------------------------------------------

    def signal(self, signum):
        """Send a signal to the worker process (not its group). POSIX only."""
        try:
            os.kill(self.pid, signum)
        except ProcessLookupError:
            pass

    def control(self, byte):
        """Send one control byte on the "ctl" connection (transport "tcp")."""
        try:
            self.ctl.sendall(byte)
        except (OSError, AttributeError):
            pass

    def interrupt(self):
        """Like the app on a timeout: SIGINT to the worker (fd), or "I" on the control connection (tcp)."""
        if self.ctl is not None:
            self.control(b"I")
        else:
            self.signal(signal.SIGINT)

    def freeze(self):
        """Like the app's freeze: SIGUSR1 (fd), or "F" on the control connection (tcp)."""
        if self.ctl is not None:
            self.control(b"F")
        else:
            self.signal(signal.SIGUSR1)

    def wait(self, timeout=None):
        """Wait for the worker to exit; returns its exit code (negative = killed by a signal) or None."""
        if self._proc is not None:                       # started with subprocess (Windows)
            if self.returncode is None:
                try:
                    self.returncode = self._proc.wait(timeout)
                except subprocess.TimeoutExpired:
                    return None
            return self.returncode
        end = None if timeout is None else time.monotonic() + timeout
        while self.returncode is None:
            try:
                pid, status = os.waitpid(self.pid, 0 if end is None else os.WNOHANG)
            except ChildProcessError:
                self.returncode = -1
                break
            if pid:
                self.returncode = os.waitstatus_to_exitcode(status)
                break
            if time.monotonic() >= end:
                return None
            time.sleep(0.01)
        return self.returncode

    def kill(self):
        """SIGKILL the worker's whole process group (Windows: terminate the worker) and reap it."""
        if self.returncode is None:
            try:
                if self._proc is not None:
                    self._proc.kill()
                else:
                    os.killpg(self.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError, OSError):
                pass
            self.wait()
        self._finish()

    def close(self, timeout=2.0):
        """Send "close", wait for "bye" and the exit; kill the group if that takes too long."""
        if self.returncode is None:
            try:
                self._next_id += 1
                self._send({"t": "close", "id": self._next_id})
                self._read(timeout, expect_id=self._next_id, interrupt=False, types=("bye", "error"))
            except (WorkerDied, OSError):
                pass
            if self.wait(timeout) is None:
                self.kill()
        self._finish()
        return self.returncode

    @property
    def alive(self):
        return self.returncode is None and self.wait(0) is None

    def stdout_text(self):
        return self._streams["stdout"].text()

    def stderr_text(self):
        return self._streams["stderr"].text()

    def stderr_tail(self, lines=20):
        return "\n".join(self.stderr_text().splitlines()[-lines:])

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()

    # -- internals ---------------------------------------------------------------------------------

    def _drain(self, fd, name):
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        sink = self._streams[name]
        try:
            while True:
                data = os.read(fd, 65536)
                if not data:
                    break
                text = decoder.decode(data)
                if text:
                    sink.add(text)
                    if self.on_output is not None:
                        self.on_output(name, text)
        except OSError:
            pass
        finally:
            os.close(fd)

    def _finish(self):
        for sock in (getattr(self, "sock", None), self.ctl):
            try:
                sock.close()
            except (OSError, AttributeError):
                pass
        for reader in getattr(self, "_readers", ()):
            reader.join(timeout=1.0)

    def _died(self, message):
        self.kill()
        raise WorkerDied(message, self.stderr_tail())

    def _send(self, msg):
        try:
            self.sock.sendall(_proto.encode(msg))
        except OSError as err:
            self._died(f"Python stopped (couldn't send {msg['t']!r}: {err})")

    def _wait_readable(self, timeout):
        poller = None
        if hasattr(select, "poll"):                      # no fd-number limit; Windows has select() only
            poller = select.poll()
            poller.register(self.sock, select.POLLIN | select.POLLHUP | select.POLLERR)
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return False
            if poller is not None:
                if poller.poll(max(1, math.ceil(left * 1000))):
                    return True
            elif select.select([self.sock], [], [self.sock], left)[0]:
                return True

    def _read(self, timeout, expect_id, interrupt, types=REPLY_TYPES):
        if not self._wait_readable(timeout):
            if not interrupt:
                self._died(f"Python didn't answer within {timeout} s")
            # Like the app: interrupt the worker (SIGINT, or "I"); it reports the stuck line as a "timeout" error.
            self.interrupted = True
            self.interrupt()
            if not self._wait_readable(self.interrupt_grace):
                self._died(f"Python didn't answer within {timeout} s, even after an interrupt")
        self.sock.settimeout(10.0)          # the rest of a started frame comes right away
        try:
            msg = _proto.recv(self.sock)
        except (_proto.ProtocolError, OSError) as err:
            self._died(f"Python sent a broken message: {err}")
        finally:
            if self.sock.fileno() >= 0:
                self.sock.settimeout(None)
        if msg is None:
            self._died("Python stopped")
        if msg.get("t") not in types or msg.get("id") != expect_id:
            self._died(f"unexpected reply {msg.get('t')!r} with id {msg.get('id')!r} (wanted id {expect_id})")
        return msg
