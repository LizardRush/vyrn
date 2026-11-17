# Python side: module layout and internal interfaces

Read `docs/dev/protocol.md` first (the wire contract) and `docs/dev/PLAN.md` §1 (what users write).
Python 3.10 or newer (3.13 on the Pi), stdlib + numpy + Pillow only (`evdev` is optional and imported lazily). No
other dependencies. Linux, macOS and Windows: everything that differs lives in `vyrn/_platform/` (§13).

```
python/vyrn_worker.py        entry point (§1)
python/vyrn/__init__.py      PUBLIC API: the only thing users import (§2)
python/vyrn/_state.py        process-wide runtime state + error classes (§3)
python/vyrn/_proto.py        frame codec (§4)
python/vyrn/_host.py         request loop, load/discovery, validation, errors, watchdog, signals (§5)
python/vyrn/_canvas.py       Canvas (§6)
python/vyrn/_effects.py      effect-string grammar, key table, did-you-mean (§7)
python/vyrn/_keys.py         virtual keyboard: held-set diffing, taps, policy gate (§8)
python/vyrn/_screen.py       per-tick union capture cache, pixel, on_screen (§9)
python/vyrn/_focus.py        FocusWatcher: the focus guard's state, fed by a focus source (§10)
python/vyrn/_platform/       the backends of each system, chosen in its __init__.py (§13)
python/vyrn/_link.py         Vyrn Link: vyrn.game/events/connected, vyrn.send() queue, game buttons (protocol.md §3.4)
python/tools/engine.py       pure-Python "app side" used by tests and check.py (§11)
python/tools/check.py        `python3 python/tools/check.py <project>`: a Check without the C++ app (§12)
python/tools/gen_api.py      docstrings → app/resources/vyrn.api + app/resources/docs/api.html
```

## 1. `vyrn_worker.py`

```python
import os, sys
if os.environ.get("VYRN_FD"):
    os.set_inheritable(int(os.environ["VYRN_FD"]), False)        # first statement after imports of os/sys
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
project = os.path.abspath(sys.argv[1])
sys.path.insert(0, here)
from vyrn import _platform
link = _platform.connect()              # fd 3, or Windows' two TCP connections; ConnectError -> stderr, exit 2
from vyrn._host import exit_worker, main, preload
preload()                               # before the project is on sys.path
sys.path.insert(1, project)             # sys.path = [library, project, ...standard...]
exit_worker(main(project, link))
```

- `preload()` imports what Vyrn would otherwise import later (this system's backends via `_platform.preload()`:
  `evdev`, `ctypes`, `PIL.ImageGrab`; `numpy.random`; and the standard modules they use) while the project folder is
  not on `sys.path` yet, so a user's helper called `glob.py`, `tempfile.py` or `secrets.py` can't replace them inside
  Vyrn.
- `exit_worker(code)` instead of `sys.exit`: when the user's scripts started threads that never end, it runs the
  `atexit` handlers, flushes open files and calls `os._exit(code)`, so those threads can't keep the worker alive.
  Without such threads it is a normal `sys.exit`.

## 2. Public API (`vyrn/__init__.py`)

Exactly these names are public (`__all__`), each with a beginner-friendly docstring whose **first line** is the
one-line summary used by autocompletion:

```python
screen(x=None, y=None, width=None, height=None) -> numpy.ndarray   # uint8 [h, w, 3]; no args = whole screen
pixel(x, y) -> tuple[int, int, int]
on_screen(name, x=None, y=None, match=0.9) -> bool
press(key) -> None          # tap a key/combo now, e.g. press("space"), press("ctrl+c")
wait(seconds) -> None
canvas(width, height, color="black") -> Canvas
show(name, value) -> None
choice: str | None           # last choice the AI made this round (None at round start)
tick: int                    # ticks since this round started (0 on the first see())
round: int                   # round number, starting at 1
game: dict                   # the Link game's newest state ({} until it sends one; a missing key explains itself)
events: list[dict]           # the Link game's events since the last tick
connected: bool              # a Link game is connected
send(type, **fields) -> None # queue {"type": type, ...} for the game; goes out with the next reply ("send")
```

`choice`, `tick`, `round` are plain module attributes the host assigns (`vyrn.tick = 5`); so are `game`, `events`
and `connected`, from the `game`, `game_events` and `connected` keys of real-time requests (protocol.md §3.4).
`screen`/`pixel`/`on_screen` delegate to `_state.S.screen`; `press` to `_state.S.keyboard.press_now`;
`show` stores into `_state.S.show_changed`; `wait` sleeps in ≤ 50 ms slices and returns early if
`_state.S.stopping` is set (no sleeping at all in check mode).

## 3. `_state.py`

```python
class VyrnUserError(Exception):
    """Raised by library functions for mistakes in the user's scripts. The host reports it as an error
    whose location is the user's calling line (deepest frame inside the project)."""
    def __init__(self, kind, message, hint=""): ...

class SimOnlyError(VyrnUserError): ...          # kind "input" — screen/keys used in a simulation
class InputNotAllowedError(VyrnUserError): ...  # kind "input"
class FocusError(VyrnUserError): ...            # kind "focus"

def safe_repr(value) -> str    # repr() for messages; never fails (huge ints, a user __repr__ that raises)

class State:
    mode = "realtime"          # "sim" | "realtime"
    check = False
    allow_input = False
    screen_allowed = True      # load "screen": false → every screen function raises an "input" error
    loading = False            # the scripts are loading: key actions are skipped (and warned about)
    frozen = False             # set by SIGUSR1, cleared by "resume"
    stopping = False
    focus_locked = False       # a lock_focus succeeded: from now on the focus guard fails closed
    project_dir = ""
    decisions_per_second = 10.0
    generation = 0             # +1 by the host at the start of EVERY request (screen cache key)
    did = []                   # check-mode descriptions for the current request
    warnings = []              # pending one-time warnings for the next reply
    warned = set()             # keys of warnings already sent (one-time)
    show_changed = {}          # name -> value since the last reply
    outbox = []                # vyrn.send() messages for the next reply (Vyrn Link)
    last_request = 0.0         # time.monotonic() of the last request other than "status" (dead-man timer)
    screen = None              # _screen.Screen
    keyboard = None            # _keys.Controller
    focus = None               # _focus.FocusWatcher | None

    def warn_once(self, key, script, line, message): ...
    def user_place(self) -> (str, int, str)   # (script, line, function) of the innermost frame inside the project

S = State()
```

## 4. `_proto.py`

```python
MAGIC = b"VYRN"; MAX_JSON = 1 << 20; MAX_BIN = 64 << 20
class ProtocolError(Exception): ...
def encode(msg: dict) -> bytes
    # msg values may contain numpy arrays (u8 or f32; others converted: bool/ints → u8 only if already u8,
    # floats → f32). Each array is replaced by {"$a":[offset,dtype,shape]}, appended to the blob at a
    # 64-byte aligned offset. JSON: json.dumps(..., separators=(",", ":"), ensure_ascii=False, allow_nan=False).
def decode(json_bytes, blob) -> dict      # replaces {"$a":...} with numpy arrays (copies), validates bounds
def send(sock, msg) -> None               # sock.sendall(encode(msg))
def recv(sock) -> dict | None             # None on clean EOF before a header; ProtocolError on anything bad
```

## 5. `_host.py`

`main(project, link=None) -> int` (`link` from `_platform.connect()`; made here when None, status 2 if impossible):
1. `sock = link.sock`; `watch = link.watch()`; `watch.install(on_sigint)` (§13.1). POSIX: `signal.set_wakeup_fd(w)` on
   a non-blocking pipe; `SIGUSR1` and `SIGTERM` → no-op Python handlers, and the watchdog thread reacts via the
   pipe (`SIGUSR1`: freeze and release keys; `SIGTERM`: release keys, close the keyboard, `os._exit(0)`). Windows:
   the watchdog reads the control connection instead (`F` freeze, `I` interrupt, EOF = the app died).
   `SIGINT` → a handler that raises `KeyboardInterrupt` only while user code runs (never inside the host's own
   code, so no reply is cut in half); the watchdog also releases the keys. On Windows the watchdog turns `I` into
   `_thread.interrupt_main()`, which runs that same handler.
2. Build `S.screen = _screen.Screen()`; send `hello` (probe backends without side effects:
   `_screen.probe()` / `_keys.probe()` / `_focus.probe(None)` return `(name_or_None, problem_or_None)`).
3. Start the watchdog thread (§ protocol 5): it loops on `watch.wait(0.05 or 0.25 s)` and handles the events
   `gone`, `freeze`, `release`, `interrupt`, `terminate` (§13.1), then the focus guard and the dead-man timer.
4. Loop: `msg = recv(sock)`; `S.generation += 1`; `S.did = []`; `S.last_request = monotonic()` (not for
   `status`, which doesn't count for the dead-man timer); dispatch on `msg["t"]`; reply. On EOF → release keys,
   return 0.
5. Every user-code call goes through one wrapper that times it, converts exceptions into `error` replies
   (location = deepest project frame via `traceback.extract_tb`, using `FrameSummary.lineno/colno`),
   converts `KeyboardInterrupt` into kind `"timeout"`, and converts `VyrnUserError` into its kind.
   Before sending any `error` the host calls `S.keyboard.release_all()` (if a keyboard exists).
6. Plain-English hints for common exceptions: NameError, AttributeError, TypeError (wrong number of
   arguments → "with a world.py your functions can take one input: world"), ZeroDivisionError, IndexError,
   KeyError, ImportError/ModuleNotFoundError, IndentationError/SyntaxError.

Loading (protocol §3.2 + §4): `os.chdir(project)` first (so `open("data.txt")` finds the project's own files),
then import scripts with `importlib.util.spec_from_file_location(name, path)` +
`module_from_spec` + `sys.modules[name] = module` + `exec_module`, in the order world, inputs, responses, points.
Hook/choice discovery exactly per protocol §4.1–4.2. Call helpers pass `world` iff the function has exactly one
required positional parameter (`inspect.signature`).

Conversion/validation of `see()` → protocol §4.4; of `points()` → §4.1.

## 6. `_canvas.py`

```python
class Canvas:
    def __init__(self, width, height, color="black")
    width, height                      # ints
    def dot(self, x, y, color="white")
    def box(self, x, y, width, height, color="white")
    def __array__(self, dtype=None, copy=None)   # uint8 [height, width, 3]
    pixels  -> numpy.ndarray (the live array)
```
Colors: names / `"#rrggbb"` via `PIL.ImageColor.getrgb`, or `(r, g, b)` tuples. Coordinates are ints; drawing
outside the canvas is clipped silently; x is the column, y the row (y grows downward).

## 7. `_effects.py`

```python
KEYS: dict[str, str]            # friendly lowercase name -> evdev code name, e.g. "a" -> "KEY_A", "shift" -> "KEY_LEFTSHIFT"
MODIFIERS = {"KEY_LEFTSHIFT", "KEY_LEFTCTRL"}
BLOCKED: dict[str, str]         # friendly name -> reason ("could change system settings", ...)
class EffectError(ValueError):  # .message, .hint
def normalize_key(name: str) -> str          # -> evdev code name; EffectError with did-you-mean / blocked reason
def label(code: str) -> str                  # "KEY_A" -> "A", "KEY_LEFTSHIFT" -> "Shift", "KEY_SPACE" -> "Space"
class Effect:
    kind: str                   # "none" | "hold" | "tap" | "call"
    keys: tuple[str, ...]       # evdev code names, modifiers first
    func: callable | None
    def describe(self) -> str   # "do nothing", "hold A", "hold Shift+D", "tap Space", "call left(world)"
def parse_effect(value, name="") -> Effect   # value: str | None | function
```

## 8. `_keys.py`

```python
def probe() -> (str | None, str | None)      # ("uinput"/"quartz"/"win32"/"record", None) or (None, "plain-English problem")
class Backend:                                # mechanism only (vyrn/_platform/keys_base.py)
    def down(self, code: str) -> None
    def up(self, code: str) -> None
    def close(self) -> None
    hint: str                                 # plain-English fix when this keyboard stops working
class UInputBackend(Backend)    # Linux: evdev.UInput(events={EV_KEY: [allowed codes only]}, name="Vyrn AI keyboard"); syn after each event
class QuartzBackend(Backend)    # macOS: CGEventCreateKeyboardEvent(kVK code) + CGEventSetFlags(held modifiers) + CGEventPost(kCGHIDEventTap)
class SendInputBackend(Backend) # Windows: SendInput(KEYEVENTF_SCANCODE [| KEYEVENTF_EXTENDEDKEY] [| KEYEVENTF_KEYUP])
class RecordingBackend(Backend) # VYRN_KEYS_BACKEND=record:<path>: appends "<monotonic> down|up <CODE>\n" (every system)
def make_backend() -> Backend   # record:<path>, else _platform.make_keyboard()
def keyboard_hint() -> str      # this system's fix for a keyboard that doesn't work

class Controller:                             # policy + state; thread-safe (one RLock)
    def __init__(self, state)                 # backend created lazily via ensure_device()
    def ensure_device(self) -> None           # creates the backend (sleeps 0.2 s after creating a real uinput device)
    def apply(self, effect) -> None           # used by "act": held-set diff (protocol §4.3) or tap
    def press_now(self, combo: str) -> None   # vyrn.press(): parse with _effects, tap, return after the key-up
    def release_all(self) -> None             # also cancels pending tap releases; safe to call from any thread
    def keys_down(self) -> bool               # any key down (held or a pending tap)? (dead-man timer, focus guard)
    def held(self) -> list[str]               # labels of held keys
    def close(self) -> None
```
Policy, checked in this order on every key action: sim → `SimOnlyError`; closing (`state.stopping` or the
keyboard closed) → `release_all()` and skip; `state.loading` → skip and warn once (per line); check mode →
append `"would hold A"` / `"would tap Space"` to `state.did`, send nothing; not `allow_input` →
`InputNotAllowedError`; `state.frozen` → `release_all()` and skip; not focused → `release_all()` and skip. The
focus check fails closed once a lock was taken (`state.focus_locked`): a stopped helper or a closed window
counts as not focused.

## 9. `_screen.py`

```python
def probe() -> (str | None, str | None)       # ("grim"/"imagegrab", None) or (None, problem): _platform.screen_probe()
def parse_ppm(data: bytes) -> numpy.ndarray   # P6, maxval 255 -> uint8 [h, w, 3] (from _platform/screen_grim.py)
class Screen:
    def __init__(self, state=None, backend=None)  # backend: a capture backend (§13.4), made on the first capture
    def grab(self, x=None, y=None, width=None, height=None) -> numpy.ndarray
    def pixel(self, x, y) -> tuple[int, int, int]
    def on_screen(self, name, x=None, y=None, match=0.9) -> bool
    def size(self) -> tuple[int, int]         # (width, height) of the whole screen
```
- Sim mode → `SimOnlyError`. `state.screen_allowed` false (load `"screen": false`) → `VyrnUserError` kind
  `"input"` ("… can't read the screen in this run", `NO_SCREEN`), checked first by every public function and again
  before grim runs.
- **One capture per request generation**: the cache holds the union rectangle of every region requested so far
  (persisting across ticks). When a request in the current generation falls inside the cached capture it is
  cropped from it; otherwise the union grows and a new capture is taken (only that tick pays twice).
- Whole-screen size comes from the first full capture (`grim -t ppm -`, or `ImageGrab.grab()`).
- **Coordinates**: the main screen's own pixels on Linux and Windows; **points** on macOS (one point = 2x2 pixels on a
  Retina screen). The ImageGrab backend scales every macOS capture down to one pixel per point (exact 2x2 averages),
  so `vyrn.screen(x, y, w, h)` is always a `h x w` picture, in the same coordinates the app's Pick area writes. A
  Snapshot whose saved area (`vyrn-area` = `x,y,w,h`) is smaller than its picture (saved with a Retina screen's
  pixels) is scaled the same way before `on_screen` compares it.
- `on_screen`: loads `<project>/pictures/<name>` with Pillow (cached by mtime). Position: `x, y` arguments, else the
  PNG text chunk `vyrn-area` = `"x,y,w,h"` (written by the app's Snapshot), else `VyrnUserError`.
  Similarity = `1 - mean(|a - b|) / 255` over RGB; `True` when ≥ `match`. A missing file → `False` plus a one-time
  warning "Take a Snapshot named game_over.png (toolbar → Snapshot)".

## 10. `_focus.py`

```python
def probe(helper_path) -> (str | None, str | None)   # _platform.focus_probe(): "vyrn-focus", "cgwindowlist" or "win32"
class FocusWatcher:
    def __init__(self, helper_path=None, source=None)  # source: _platform.focus_source(helper_path) (§13.5), started here
    def wait_ready(self, timeout=1.0) -> bool
    def alive(self) -> bool                   # the helper runs and hasn't reported an error
    def active(self) -> dict | None           # {"id": int, "app_id": str, "title": str}
    def lock(self) -> dict                    # lock onto the active window; FocusError if none / it is Vyrn ("vyrn")
    def is_focused(self) -> bool              # True when not locked; else active id == locked id (polls first: source.refresh())
    def stop(self) -> None
```

The watcher only understands the helper's lines (protocol.md section 6); every source produces them. `lock()` asks
the source whether the active window is Vyrn itself (`source.is_vyrn(window)`).

## 11. `tools/engine.py` (test + check helper; mirrors what the C++ app does)

```python
DATA_DIR = <repo root>      # folder that contains python/
class Worker:
    def __init__(self, project_dir, python=sys.executable, env=None, transport=None)
        # transport "fd" (POSIX default): socketpair → child fd 3, own session/process group;
        # "tcp" (Windows default; tests select it anywhere): 127.0.0.1 listener + token, accepts "main" + "ctl"
    hello: dict
    def request(self, t, timeout=30.0, **fields) -> dict  # returns the reply; arrays as numpy; raises WorkerError on "error" replies (the error dict is .error)
    def load(self, **fields) -> dict                      # defaults: check=False, allow_input=False, seed=0, round_limit=1000, decisions_per_second=10.0, focus_helper=None
    def signal(self, signum) -> None                      # POSIX
    def interrupt(self) -> None                           # what the app does on a timeout: SIGINT, or "I" on ctl
    def freeze(self) -> None                              # SIGUSR1, or "F" on ctl
    def control(self, byte) -> None                       # one byte on the control connection (tcp)
    ctl: socket | None                                    # the control connection (tcp)
    def close(self, timeout=2.0) -> int                   # sends "close", waits; kills the group on timeout
    def kill(self) -> None
    stdout_text() / stderr_text() -> str                  # collected by reader threads
    pid: int
```

## 12. `tools/check.py`

`python3 python/tools/check.py <project> [--steps N]`: spawns a worker with `check=True`, prints the spec
(mode, choices with effects, what the AI sees), then runs `reset` + N random `step`s (sim) or `restart` once
(if defined) and N `observe` ticks with `act` (real time, nothing pressed), prints an ASCII preview of the last
picture (or the numbers), the average points, per-hook timings, warnings, notes (a real-time `game_over()` that
stayed False is a note, not a warning), and any error as `file:line: message`. For a real-time project it lists
what this system offers (`backend_line`): "Screen capture: grim", or the problem, e.g. on macOS
"Screen capture: needs the Screen Recording permission (System Settings → Privacy & Security → ...)" plus a note
that check.py's permissions belong to the terminal app. It runs on every system (engine picks the transport). Exit 0 = ok, 2 = usage problem,
3 = script error, 4 = Python stopped.

## 13. `vyrn/_platform/` (docs/dev/platforms.md)

`_platform/__init__.py` is the only Python file that looks at `sys.platform` (`SYSTEM` = `"linux"`, `"macos"` or
`"windows"`; a test enforces it). It picks one backend per concern; every backend module imports on every system
(native libraries are loaded when a backend is made or probed), so the tests run them all on Linux with fakes.

| Concern | Linux | macOS | Windows | Chooser |
|---|---|---|---|---|
| transport | `fd` (fd 3) | `fd` | `tcp` | `TRANSPORT`, `transport_kind()` (`VYRN_TRANSPORT=tcp` forces tcp), `connect()` |
| app watch | `PollWatch` | `KqueueWatch` | `ControlWatch` | `connect()` (follows the transport) |
| keys | `keys_uinput` | `keys_quartz` | `keys_win32` | `keys_module()`, `keys_probe()`, `make_keyboard()` |
| screen | `screen_grim` (Wayland), `screen_imagegrab` (X11) | `screen_imagegrab` | `screen_imagegrab` | `screen_module()` (`VYRN_GRIM` → grim anywhere) |
| focus | `focus_helper` | `focus_mac` | `focus_win32` | `focus_module()` (a `focus_helper` path → the helper anywhere) |

`preload()` imports the chosen backends (and `transport`) before the project folder is on `sys.path`.

### 13.1 `transport.py`

```python
class ConnectError(Exception)                # plain message; vyrn_worker.py prints it and exits 2
class Link:  kind ("fd"|"tcp"), sock (frames), ctl (tcp), watch() -> a new watch
def connect_fd(environ, watch_class) -> Link  # os.set_inheritable(fd, False); socket.socket(fileno=fd); remembers getppid()
def connect_tcp(environ) -> Link              # "<token> main\n", then "<token> ctl\n"; TCP_NODELAY; pops VYRN_TOKEN
# a watch: install(on_sigint) on the main thread, then wait(timeout_s) -> [events] on the watchdog thread,
# interrupt_main() for "interrupt". Events: "gone" (once), "freeze", "release" (SIGINT), "interrupt" (ctl I), "terminate".
class PollWatch     # Linux: poll fd 3 for POLLRDHUP|POLLHUP|POLLERR + the signal wakeup pipe
class KqueueWatch   # macOS: EVFILT_PROC NOTE_EXIT (oneshot) on the parent pid; EVFILT_READ|EV_CLEAR on fd 3, EV_EOF = gone;
                    # already re-parented or ESRCH → gone at once
class ControlWatch  # Windows: select() + recv() on ctl: I → interrupt, F → freeze, EOF/error → gone; SIGINT handler only
```

### 13.2 Keys

Every real backend has a code table for every key of `_effects.ALLOWED_CODES` (evdev names); `test_platform.py`
checks that each table covers exactly the allow-list, has no duplicates and reaches no blocked key.

- `keys_uinput`: evdev's own codes (the allow-list is evdev names). `probe()` checks evdev and `/dev/uinput`.
- `keys_quartz.KVK`: `kVK_*` codes (ANSI layout; Insert = `kVK_Help`). Modifiers set `kCGEventFlagMaskShift` /
  `kCGEventFlagMaskControl` on every event while held. Needs Accessibility: `probe()` and the backend's constructor
  call `AXIsProcessTrusted()` (ApplicationServices) and say "needs the Accessibility permission (System Settings →
  Privacy & Security → Accessibility ...)". Events are `CFRelease`d after posting.
- `keys_win32.SCAN` + `EXTENDED`: set-1 scan codes; the arrows, Insert, Delete, Home, End, Page Up/Down get
  `KEYEVENTF_EXTENDEDKEY`. `INPUT` is the full union (40 bytes on x64), so `SendInput`'s `cbSize` is right.

### 13.3 What only the app can do

Synthetic keys (macOS, Windows) don't come up when the worker is killed: the app calls `platform::release_all_keys()`.
The app also owns the job object (Windows), the panic hotkey and the permission prompts (SetupDialog).

### 13.4 Screen

A capture backend has `capture(rect | None) -> uint8 [h, w, 3]` (rect = `(x0, y0, x1, y1)` in vyrn.screen()
coordinates, end-exclusive) and a `hint`.

- `screen_grim.GrimCapture`: `grim [-g "x,y wxh"] -t ppm -`; a picture of the wrong size → "set the display scale to
  100%".
- `screen_imagegrab.ImageGrabCapture(system, grab=None, points=None)`: `ImageGrab.grab()` / `grab(bbox=rect)`.
  macOS: the whole screen's size in points comes from `CGDisplayBounds(CGMainDisplayID())`; `scale` = capture width
  / points width; `scale_to()` averages each 2x2 block (`Image.reduce`) or box-filters other ratios. `probe()` on
  macOS calls `CGPreflightScreenCaptureAccess()` (asks nothing) → "needs the Screen Recording permission
  (System Settings → Privacy & Security → Screen Recording ...)"; on Linux it needs `DISPLAY` and Pillow with XCB.

### 13.5 Focus

A focus source has `start(on_line, on_end)`, `refresh()`, `is_vyrn(window)`, `stop()`, and the texts the watcher
uses for its errors (`error_text`, `error_hint`, `ended_text`, `install_hint`, `late_text`, `late_hint`).

- `focus_helper.HelperSource`: runs `vyrn-focus` and passes its lines on (Linux; or any system when load names a
  helper, as the tests do).
- `focus_base.PollSource(query, ...)`: a thread polls `query.active() -> Window(id, app_id, title, pid, vyrn) | None`
  every 50 ms (20 Hz) and writes `active`/`none`/`closed` (ids it reported that `query.exists(id)` no longer knows) and
  `ready` after the first answer; any exception → `error` and the source stops (the guard fails closed). `refresh()`
  polls on the calling thread (lock order: the source's lock, then the watcher's).
- `focus_win32.Win32Query`: `GetForegroundWindow`, `GetWindowTextW`, `GetWindowThreadProcessId`,
  `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW` (cached per window); `IsWindow`.
- `focus_mac.MacQuery(server, objc, alive)`: the macOS guard must never fail open, so the WindowServer decides
  (`focus_mac.WindowServer`, CoreGraphics + CoreFoundation via ctypes): `CGWindowListCopyWindowInfo(
  kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)` lists the windows front to
  back, always fresh (no run loop, unlike `NSWorkspace.frontmostApplication`); the first one with `kCGWindowLayer` 0
  is the front window, read with `CFArrayGetCount`/`CFArrayGetValueAtIndex`/`CFDictionaryGetValue` (keys made once
  with `CFStringCreateWithCString`), `CFNumberGetValue` (`kCGWindowOwnerPID`) and `CFStringGetCString`
  (`kCGWindowOwnerName`); neither needs Screen Recording. The copied list is `CFRelease`d every poll, the keys on
  `close()`. Window = (id = pid, app_id = bundle id or owner name, title = owner name, key = owner name): the lock is
  on pid + name, since `PollSource` reports an id whose (pid, key) changed as closed. Fails closed: a NULL list, an
  exception, no layer-0 window, or a front window whose layer or pid can't be read → no active window (`none`, logged
  once on stderr); never a window further back. The bundle id (display only) comes from `NSRunningApplication
  runningApplicationWithProcessIdentifier:` through the objc runtime (`ObjC`: one `objc_msgSend` prototype per
  signature, an autorelease pool per call), cached per pid + name; without AppKit the owner name is used. A pid that
  no longer exists is reported closed.

### 13.6 Tests

`tests/python/test_platform.py`: the chooser, imports with Linux-only calls removed (pretending to be Windows and
macOS in a subprocess), the key tables, `SendInput`/Quartz/ImageGrab/user32/objc/kqueue fakes, Windows' transport over
socket pairs, and real workers on `transport="tcp"`: `I` interrupts stuck code, `F` freezes, EOF on ctl ends the
worker (also the app-death test in `test_safety.py`), a worker that can't connect exits with status 2.
