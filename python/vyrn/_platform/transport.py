"""The worker's connection to the app, and how the watchdog learns what the app wants or that it is gone
(docs/dev/platforms.md section 1, protocol.md sections 1 and 5).

A Link carries the frames on `.sock`. `Link.watch()` makes what the watchdog thread waits on; its wait(timeout)
returns a list of events:

    "gone"        the app died: release the keys and exit (reported once)
    "freeze"      SIGUSR1, or control byte F: release the keys and skip key actions until "resume"
    "release"     SIGINT arrived (the kernel interrupts the user's code by itself): release the keys
    "interrupt"   control byte I: release the keys and interrupt the main thread (_thread.interrupt_main), so stuck
                  user code raises KeyboardInterrupt at its line, exactly like SIGINT would
    "terminate"   SIGTERM: release the keys and exit

Which link and which watch a system uses is decided in vyrn._platform; nothing here looks at sys.platform, so the
tests can run every backend on Linux.
"""

import _thread
import os
import re
import select
import signal
import socket
import time

__all__ = ["ConnectError", "Link", "connect_fd", "connect_tcp", "PollWatch", "KqueueWatch", "ControlWatch"]

CONNECT_TIMEOUT = 10.0          # seconds to reach the app's listener (it accepts within its hello timeout)
_TOKEN = re.compile(r"[\x21-\x7e]{1,128}\Z")   # what may go in the "<token> main" line (the app makes 32 hex digits)
_TRY_IT = "To try a project yourself, run:  python3 python/tools/check.py <project folder>"


class ConnectError(Exception):
    """The worker can't reach the app; the message says why (vyrn_worker.py prints it and exits with status 2)."""


class Link:
    """The worker's connection to the app: `sock` carries the frames (protocol.md section 2)."""

    def __init__(self, kind, sock, make_watch, ctl=None):
        self.kind = kind            # "fd" or "tcp"
        self.sock = sock
        self.ctl = ctl              # the control connection (tcp only)
        self._make_watch = make_watch

    def watch(self):
        """A new watch for the watchdog thread (call install() on it from the main thread first)."""
        return self._make_watch()


def connect_fd(environ, watch_class):
    """POSIX: the app's end of a socketpair is fd 3 ($VYRN_FD). Programs the user's scripts start never inherit it."""
    try:
        fd = int(environ.get("VYRN_FD", "3"))
        os.set_inheritable(fd, False)
        sock = socket.socket(fileno=fd)
    except (OSError, ValueError):
        raise ConnectError("vyrn_worker.py is started by the Vyrn app (it needs the app's socket on fd 3).\n"
                           + _TRY_IT) from None
    parent = os.getppid()           # the app; KqueueWatch watches it exit
    return Link("fd", sock, lambda: watch_class(sock, parent))


def connect_tcp(environ, timeout=CONNECT_TIMEOUT):
    """Windows: open the main and the control connection to $VYRN_ADDR, each starting with "<token> <role>\\n".

    The token is then removed from `environ`, so programs the user's scripts start never see it.
    """
    addr = environ.get("VYRN_ADDR", "").strip()
    token = environ.get("VYRN_TOKEN", "").strip()
    if not addr or not token:
        raise ConnectError("vyrn_worker.py is started by the Vyrn app (it needs VYRN_ADDR and VYRN_TOKEN to "
                           "connect to it).\n" + _TRY_IT)
    host, sep, port = addr.rpartition(":")
    host = host.strip("[]")
    if not sep or not host or not port.isdigit() or not 0 < int(port) < 65536 or not _TOKEN.match(token):
        raise ConnectError(f"vyrn_worker.py can't use VYRN_ADDR={addr!r} and VYRN_TOKEN: they aren't what the Vyrn "
                           "app sets.\n" + _TRY_IT)
    socks = []
    try:
        for role in ("main", "ctl"):
            sock = socket.create_connection((host, int(port)), timeout=timeout)
            socks.append(sock)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.sendall(f"{token} {role}\n".encode("ascii"))
            sock.settimeout(None)
    except OSError as err:
        for sock in socks:
            sock.close()
        raise ConnectError(f"vyrn_worker.py couldn't connect to the Vyrn app at {addr}: "
                           f"{getattr(err, 'strerror', None) or err}") from None
    environ.pop("VYRN_TOKEN", None)
    main, ctl = socks
    return Link("tcp", main, lambda: ControlWatch(ctl), ctl=ctl)


def _ignore(signum, frame):
    """A Python-level handler that does nothing: the watchdog reacts through the wakeup fd."""


class _PosixSignals:
    """SIGUSR1 (freeze), SIGTERM and SIGINT reach the watchdog through signal.set_wakeup_fd, so they work even while
    the user's code is stuck. Only the main thread may install() them."""

    _wake_r = -1

    def install(self, on_sigint):
        wake_r, wake_w = os.pipe()               # non-inheritable (PEP 446)
        os.set_blocking(wake_r, False)
        os.set_blocking(wake_w, False)
        signal.set_wakeup_fd(wake_w, warn_on_full_buffer=False)
        signal.signal(signal.SIGUSR1, _ignore)
        signal.signal(signal.SIGTERM, _ignore)
        signal.signal(signal.SIGINT, on_sigint)
        self._wake_r = wake_r

    def _signal_events(self):
        try:
            data = os.read(self._wake_r, 256)
        except (BlockingIOError, InterruptedError):
            return []
        events = []
        for signum in data:
            if signum == signal.SIGTERM:
                events.append("terminate")
            elif signum == signal.SIGUSR1:
                events.append("freeze")
            elif signum == signal.SIGINT:
                events.append("release")
        return events

    def interrupt_main(self):
        """POSIX: SIGINT itself interrupts the main thread; nothing more to do."""


class PollWatch(_PosixSignals):
    """Linux: the app is gone when its socket reports POLLRDHUP, POLLHUP or POLLERR."""

    def __init__(self, sock, parent_pid=None):
        self._sock_fd = sock.fileno()
        self._poller = None

    def wait(self, timeout):
        if self._poller is None:
            self._poller = select.poll()
            self._poller.register(self._sock_fd,
                                  getattr(select, "POLLRDHUP", 0) | select.POLLHUP | select.POLLERR)
            if self._wake_r >= 0:
                self._poller.register(self._wake_r, select.POLLIN)
        events = []
        for fd, flags in self._poller.poll(max(0, round(timeout * 1000))):
            if fd == self._wake_r:
                events += self._signal_events()
            elif flags & select.POLLNVAL:
                self._poller.unregister(fd)
            else:
                self._poller.unregister(fd)      # report it once
                events.append("gone")
        return events


class KqueueWatch(_PosixSignals):
    """macOS: the app is gone when it exits (EVFILT_PROC NOTE_EXIT on the parent pid) or its socket reaches EOF.

    The socket's read filter is edge-triggered (EV_CLEAR): a request arriving wakes the watchdog once, and only EOF
    counts. `kq` is select's kqueue module part, replaceable in tests.
    """

    def __init__(self, sock, parent_pid, kq=select):
        self._sock_fd = sock.fileno()
        self._parent = parent_pid
        self._kq_api = kq
        self._kq = None
        self._pending = []

    def _setup(self):
        api = self._kq_api
        self._kq = api.kqueue()
        changes = [api.kevent(self._sock_fd, api.KQ_FILTER_READ, api.KQ_EV_ADD | api.KQ_EV_CLEAR)]
        if self._wake_r >= 0:
            changes.append(api.kevent(self._wake_r, api.KQ_FILTER_READ, api.KQ_EV_ADD))
        self._kq.control(changes, 0)
        if self._parent is None or self._parent <= 1 or os.getppid() != self._parent:
            self._pending.append("gone")         # re-parented: the app is already gone
            return
        try:
            self._kq.control([api.kevent(self._parent, api.KQ_FILTER_PROC, api.KQ_EV_ADD | api.KQ_EV_ONESHOT,
                                         api.KQ_NOTE_EXIT)], 0)
        except OSError:                          # ESRCH: it exited before we could watch it
            self._pending.append("gone")

    def wait(self, timeout):
        if self._kq is None:
            self._setup()
        events, self._pending = self._pending, []
        if events:
            return events
        api = self._kq_api
        for kev in self._kq.control(None, 8, max(0.0, timeout)):
            if kev.filter == api.KQ_FILTER_PROC:
                events.append("gone")
            elif kev.ident == self._wake_r:
                events += self._signal_events()
            elif kev.ident == self._sock_fd and kev.flags & api.KQ_EV_EOF:
                try:
                    self._kq.control([api.kevent(self._sock_fd, api.KQ_FILTER_READ, api.KQ_EV_DELETE)], 0)
                except OSError:
                    pass
                events.append("gone")
        return events


class ControlWatch:
    """Windows: the app's control connection. Byte I = interrupt, F = freeze, EOF (or a broken connection) = gone.

    Signals: only SIGINT gets a handler, so _thread.interrupt_main() runs the host's guarded SIGINT handler (it raises
    KeyboardInterrupt only while user code runs, never in the middle of a reply).
    """

    def __init__(self, ctl):
        self._ctl = ctl
        self._open = True

    def install(self, on_sigint):
        signal.signal(signal.SIGINT, on_sigint)

    def wait(self, timeout):
        if not self._open:
            time.sleep(max(0.0, timeout))
            return []
        try:
            readable, _, _ = select.select([self._ctl], [], [], max(0.0, timeout))
        except (OSError, ValueError):
            readable = [self._ctl]
        if not readable:
            return []
        try:
            data = self._ctl.recv(64)
        except (BlockingIOError, InterruptedError, socket.timeout):
            return []
        except (OSError, ValueError):
            data = b""
        if not data:
            self._open = False
            return ["gone"]
        events = []
        for byte in data:
            if byte == 0x49:                     # "I"
                events.append("interrupt")
            elif byte == 0x46:                   # "F"
                events.append("freeze")
        return events

    def interrupt_main(self):
        """What SIGINT does on POSIX: the main thread's SIGINT handler runs at its next bytecode."""
        _thread.interrupt_main()
