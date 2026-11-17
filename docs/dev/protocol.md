# Vyrn app ↔ Python worker protocol (v1)

This is the binding contract between the C++ app (`vyrn_core`, "the chip") and the Python worker
(`python/vyrn/_host.py`, "the microcontroller"). Both sides, and the pure-Python test engine
(`python/tools/engine.py`), implement exactly this. When in doubt, this file wins over the plan.

## 1. Worker process

Launch (the app does this with `posix_spawn`, on Windows `CreateProcessW`; tests and `check.py` through
`python/tools/engine.py`):

```
<python> -u -X utf8 <data>/python/vyrn_worker.py <project_dir>
```

- `<python>` defaults to `/usr/bin/python3` (macOS: `python3` on PATH, Windows: `py -3`/`python.exe`, Python ≥ 3.10,
  docs/dev/platforms.md); `vyrn.json` may set `"python"`.
- **Linux, macOS**: fd 0 = `/dev/null`, fd 1/fd 2 = pipes (user `print()` and tracebacks go to the app console),
  **fd 3 = the worker's end of an `AF_UNIX`/`SOCK_STREAM` socketpair**. No other fds are inherited.
  The worker's first statement is `os.set_inheritable(3, False)` (when `VYRN_FD` is set), so programs the user starts
  (`os.system`, `Popen`) never inherit the socket.
- **Windows** (docs/dev/platforms.md section 1): stdin = `NUL`, stdout/stderr = pipes, `CREATE_NO_WINDOW`, inside a
  job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. No `VYRN_FD`; instead `VYRN_ADDR=127.0.0.1:<port>` (the app's
  listener) and `VYRN_TOKEN=<32 lowercase hex digits>`. The worker opens two TCP connections to it, first
  `<token> main\n` (then the frames of section 2, exactly like fd 3), then `<token> ctl\n` (the control connection:
  the app sends single bytes, `I` = interrupt like SIGINT, `F` = freeze like SIGUSR1; EOF on it = the app died). Both
  use `TCP_NODELAY`. The worker removes `VYRN_TOKEN` from its environment once connected (programs the user starts
  never see it). The app accepts exactly these two connections within the hello timeout (a wrong token or a third
  connection is closed), then closes its listener.
- A worker that can't reach the app (no fd 3, no or bad `VYRN_ADDR`/`VYRN_TOKEN`, connection refused) prints why on
  stderr and exits with status 2.
- Environment set by the app: `VYRN_FD=3` (POSIX) or `VYRN_ADDR` + `VYRN_TOKEN` (Windows), `VYRN_DATA=<data dir>`
  (the folder containing `python/`), `OMP_NUM_THREADS=1`, `OPENBLAS_NUM_THREADS=1`, `PYTHONDONTWRITEBYTECODE=1`,
  plus the user's environment (`WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `DISPLAY` pass through).
- The worker runs in its **own process group** (the app kills the group; Windows: `TerminateJobObject`).
- `sys.path` = `[<data>/python, <project_dir>, ...standard...]`; `sys.dont_write_bytecode = True`
  (no `__pycache__` in the user's project). The project folder goes on `sys.path` only after Vyrn's own modules
  are imported, so a user file such as `glob.py` can't replace them. At `load` the worker changes its working
  directory to the project folder (so `open("data.txt")` finds the project's files).
- The app writes `$XDG_RUNTIME_DIR/vyrn/<pid>.pid` containing `"<pid> <starttime> <pgid>\n"`
  (`starttime` = field 22 of `/proc/<pid>/stat`) right after spawning, and deletes it after `waitpid`.
  `scripts/vyrn-panic` uses these files. A file `$XDG_RUNTIME_DIR/vyrn/panic` appearing means the panic key
  was pressed: when a worker dies, the app checks for it, reports "Panic stop" and deletes it.
  At startup the app deletes stale pid files whose process no longer matches.

Test-only environment overrides (never set by the app in normal runs):

| Variable | Effect |
|---|---|
| `VYRN_KEYS_BACKEND=record:<path>` | instead of the real keyboard (uinput, Quartz, SendInput), append one line per key event to `<path>`: `<monotonic_seconds> down|up <KEY_NAME>` (every system) |
| `VYRN_GRIM=<path>` | program used instead of `grim` (must accept the same `-g "x,y wxh" -t ppm -` arguments and write a PPM to stdout); selects the grim backend on every system, so no test captures the real screen |
| `VYRN_TRANSPORT=tcp` | use Windows' transport (`VYRN_ADDR` + `VYRN_TOKEN`, main + control connection) on Linux/macOS too |

A `focus_helper` path in `load` (section 3.2) selects that helper program (section 6) on every system; the app
sends `null`, which means this system's own focus backend.

## 2. Framing (both directions)

```
offset 0   4 bytes   magic "VYRN" (ASCII)
offset 4   u32 LE    json_len   (1 .. 1_048_576)
offset 8   u32 LE    bin_len    (0 .. 67_108_864)
offset 12  json_len  UTF-8 JSON object
then       bin_len   binary blob
```

- Every JSON object has `"t"` (message type, string) and `"id"` (integer). Requests from the app use
  increasing ids starting at 1; **every reply echoes the request's id**. `hello` uses id 0.
- **Arrays** inside JSON are references into the blob: `{"$a": [offset, dtype, shape]}`
  - `dtype` is `"u8"` or `"f32"` (little-endian float32); `shape` is a list of ints, C order.
  - `offset` is a multiple of 64; writers pad with zero bytes. Byte length = prod(shape) × itemsize.
  - Readers reject offset/length outside the blob.
- JSON text is valid UTF-8. The worker writes a lone surrogate (OS data such as a file name that Python decoded
  with `surrogateescape`) as U+FFFD.
- Violations (bad magic, oversize, bad JSON, bad array ref, unknown `"t"` from the worker) are fatal protocol
  errors. Unknown JSON *keys* are ignored by both sides (forward compatibility).
- A frame the worker can't read gets `error` with `id` 0, `kind` `"protocol"`, `fatal: true`, and the worker
  exits with status 2. An unknown `"t"`, a missing or non-integer `id` (answered with `id` 0), a §3.3/§3.4
  request other than `status` before `load` or for the other mode, a second `load`, or a bad choice number gets
  `kind` `"protocol"`, `fatal: true`. `lock_focus` without `allow_input` gets `kind` `"input"`.
- **One request outstanding per worker.** The worker sends exactly one reply (`ok`, `result`, `spec`, `bye`
  or `error`) per request, in order.

Golden test vector: `tests/golden/frame_result.bin` is the encoding of
`{"t":"result","id":7,"obs":{"$a":[0,"u8",[2,2,3]]},"points":1.5,"game_over":false}`
(JSON serialized compactly with `separators=(",", ":")`, keys in exactly that order) with blob = bytes 0..11.
Both the C++ and Python tests must produce/parse it byte-for-byte.

## 3. Messages

### 3.1 Worker → app, unprompted: `hello`

```json
{"t":"hello","id":0,"proto":1,"vyrn":"0.1.0","python":"3.13.5","pid":1234,
 "backends":{"screen":"grim","keys":"uinput","focus":null},
 "problems":{"keys":"/dev/uinput is not writable: run ./install.sh --keyboard"}}
```

`backends.*` is `null` when unavailable; `problems` explains why in plain English (shown by the Setup dialog).
`proto` other than 1 → the app stops with "library/app version mismatch".

### 3.2 `load` (app → worker) → `spec`

```json
{"t":"load","id":1,"project":"/home/u/Vyrn/catch","check":false,"allow_input":false,
 "seed":12345,"round_limit":1000,"decisions_per_second":10.0,"focus_helper":"/home/u/.local/bin/vyrn-focus"}
```

`allow_input` must be a JSON boolean (missing or `null` means false); anything else is a fatal `protocol` error.
Optional `"screen": false` (sent only by an AI agent's Check that the person hasn't allowed to see the screen,
docs/dev/mcp.md §4): `vyrn.screen()`, `pixel()` and `on_screen()` raise error kind `"input"` ("… can't read the
screen in this run") before anything is captured. Missing, `null` or `true` = the screen may be read; anything
else is a fatal `protocol` error.

The worker:
1. Seeds `random.seed(seed)` and `numpy.random.seed(seed % 2**32)`.
2. Imports the user scripts **as modules** named `world` (if `world.py` exists), `inputs`, `responses`, `points`,
   in that order. Missing `inputs.py` / `responses.py` / `points.py` is a fatal load error. A script another
   script already imported is that same module (it doesn't run twice).
3. Finds the **hooks** (§4.1) and **choices** (§4.2).
4. Creates one blank `world` object (`types.SimpleNamespace()`), shared by all scripts.
5. Mode = `"sim"` if `world.py` exists, else `"realtime"`.
6. Infers the observation: sim → calls `restart(world)` (if defined) then `see(world)`;
   realtime → calls `see()` only (never `restart()`, which may press keys).
7. Replies `spec`:

```json
{"t":"spec","id":1,"mode":"sim",
 "obs":{"kind":"picture","shape":[10,10,3],"names":null},
 "choices":["left","right","stay"], "labels":["left","right","stay"],
 "effects":["call left(world)","call right(world)","do nothing"],
 "uses":{"screen":false,"keys":false,"game":false}, "buttons":[],
 "hooks":{"see":"inputs.py:3","points":"points.py:1","game_over":"points.py:8","restart":"world.py:3","step":"world.py:9"},
 "preview":{"$a":[0,"u8",[10,10,3]]},
 "warnings":[{"script":"responses.py","line":0,"message":"No do-nothing choice: add  wait = None  so the AI can stand still"}]}
```

- `obs.kind` is `"picture"` (shape `[H,W,3]`, u8) or `"numbers"` (shape `[D]`, f32, `names` = list of D strings).
- `labels` = choice names with `_` replaced by spaces (for the UI).
- `effects` = plain-English description per choice: `hold A`, `hold Shift+D`, `tap Space`, `call left(world)`,
  `do nothing`.
- `uses.keys` is true when any choice is a key string, or any script or project module they import calls
  `vyrn.press` (also as `v.press` after `import vyrn as v`, or an imported `press`); `uses.screen` likewise for
  `screen`, `pixel` and `on_screen`. (Informational, read from the source. Real enforcement is runtime gating, §5.)
- `uses.game` is true when a choice is a game button (§4.3) or the scripts use `vyrn.game`, `vyrn.events`,
  `vyrn.connected` or `vyrn.send` (read from the source too): the project talks to a game over Vyrn Link (§3.4).
- `buttons` = the game buttons the choices hold or tap, lowercase, in order of first use (`[]` without game
  choices). The app puts them in the Vyrn Link `welcome`.
- `hooks` maps each found hook to `"file:line"` of its `def` (decorators skipped).
- Any exception or validation failure during load → `error` with `"fatal": true`.

### 3.3 Simulation requests (mode `"sim"`)

| Request | Worker does | Reply |
|---|---|---|
| `{"t":"reset"}` | `round += 1; tick = 0; choice = None`; `restart(world)` if defined; `obs = see(world)` | `result` with `first: true`, `points: null` |
| `{"t":"step","a":i}` | see below | `result` |

`step(a)`:
1. Apply choice `i`'s effect (in a sim only functions and `None` are possible; key strings are rejected at load
   with "simulations can't press keys"). `vyrn.choice = choices[i]`.
2. `step(world)` if defined. `tick += 1`.
3. `obs = see(world)`; `pts = points(world)`; `over = bool(game_over(world))` (false if undefined).
4. `truncated = (not over) and tick >= round_limit`.
5. If `over or truncated`: `final_obs = obs`, then auto-restart: `round += 1; tick = 0; choice = None;
   restart(world)`; `obs = see(world)` → reply `{obs, points: pts, game_over: over, truncated, final_obs, first: true}`.
6. Else reply `{obs, points: pts, game_over: false, truncated: false, final_obs: null, first: false}`.

So the transition for the brain is `(s, a, r=points, s'=final_obs or obs, terminal=game_over)`; when `first` is
true, `obs` is the first observation of the next round.

### 3.4 Real-time requests (mode `"realtime"`)

| Request | Worker does | Reply |
|---|---|---|
| `{"t":"lock_focus"}` | (only with `allow_input`) start the focus helper if needed, wait ≤ 1 s for its state, lock onto the active window; create the virtual keyboard (not in check mode) | `ok {"app_id","title"}`; `error` kind `"focus"` if no window is active, the active window is Vyrn itself (`app_id` `"vyrn"` or `"vyrn.desktop"`; other systems: section 6), or the helper is unavailable; kind `"input"` without `allow_input` |
| `{"t":"restart"}` | `restart()` if defined | `ok {"did":[...]}` |
| `{"t":"check_over"}` | `game_over()` (false if undefined) | `ok {"game_over":bool}` |
| `{"t":"observe","first":bool}` | first: `round += 1; tick = 0; choice = None`, then `obs = see()` only. Not first: `tick += 1; obs = see(); pts = points(); over = game_over()` | `result {obs, points (null if first), game_over (false if first), focused, ...}` |
| `{"t":"act","a":i}` | apply choice `i`'s effect (§4.3); `vyrn.choice = choices[i]` | `ok {"did":[...]}` |
| `{"t":"status"}` | no user code; any mode, also before `load`; doesn't count for the dead-man timer (§5) | `ok {"focused":bool,"active":{"id","app_id","title"} or null,"held":["A"],"frozen":bool}` |

The app owns the tick clock and the round limit in real time. `restart` and `check_over` release held keys before
running user code; `observe` releases them when a round starts (`first: true`) and when it reports
`game_over: true`. When the app holds for a reason the worker can't see (user hold, late tick), it sends `pause`.

**Vyrn Link** (docs/dev/link.md; the game's side is docs/user/vyrn-link.md). For a project whose game is connected
over Vyrn Link, the app adds the game's messages to `observe`, `check_over` and `restart`:

| Key | Meaning in the worker |
|---|---|
| `game` | The game's newest `state` message (an object; `{}` before the first one) → `vyrn.game`. Missing: `vyrn.game` keeps its value |
| `game_events` | The game's `event` messages since the previous request, oldest first → `vyrn.events`. Missing: `vyrn.events` is `[]` |
| `connected` | Whether a game is connected → `vyrn.connected` (only `true` counts). Missing: unchanged |

```json
{"t":"observe","id":11,"first":false,"connected":true,
 "game":{"type":"state","frame":1234,"y":105.0,"dead":false},"game_events":[{"type":"event","name":"death"}]}
```

The replies carry the AI's buttons and the scripts' messages back:

- The `ok` reply to `act` always has `"game_input": {"hold": [...], "tap": [...]}`: the game buttons of the chosen
  effect (§4.3). `hold` is every button that must be down until the next `act` (empty for any other choice), `tap`
  the buttons pressed once. In check mode both are empty and `did` describes them (`"would hold game button jump"`).
  The app sends them to the game as `{"type":"input","frame":<the state's frame>,"hold":[...],"tap":[...]}`.
- Any reply except `error` may have `"send": [{"type":"restart"}, ...]`: the messages `vyrn.send()` queued since the
  previous reply, in order (at most 64 and 512 KiB per reply). Each is an object with a string `type` that is not
  `input`, `stop`, `welcome`, `error`, `hello`, `state` or `event`; numbers are finite. The app forwards them to the
  game unchanged, before that tick's `input`. A request that ends in `error` drops its messages. In check mode
  nothing is queued and `did` describes them (`would send {"type":"restart"}`); during `load` they are skipped with
  a spec warning.

```json
{"t":"ok","id":12,"did":[],"ms":{"respond":0.01},"game_input":{"hold":["jump"],"tap":[]},"send":[{"type":"restart"}]}
```

Old apps ignore `game_input` and `send`, and a worker that gets no Link keys keeps `vyrn.game == {}`,
`vyrn.events == []` and `vyrn.connected == False`.

### 3.5 Both modes

| Request | Worker does | Reply |
|---|---|---|
| `{"t":"pause"}` | release every held key, cancel pending taps | `ok` |
| `{"t":"resume"}` | clear the frozen flag (§5) | `ok` |
| `{"t":"close"}` | release keys, close the virtual keyboard for good, stop the focus helper | `bye`, then exit 0 (§5) |

### 3.6 `result` (worker → app)

```json
{"t":"result","id":9,"obs":{"$a":[0,"u8",[84,84,3]]},"points":1.0,"game_over":false,"truncated":false,
 "first":false,"final_obs":null,"focused":true,
 "show":{"speed":3.5,"label":"jumping","view":{"$a":[64,"u8",[10,10,3]]}},
 "did":[],"warnings":[],"ms":{"see":21.3,"points":0.4,"game_over":0.1,"respond":0.0,"step":0.0}}
```

- `points` is a float or `null` (first tick). `focused` is `null` when no focus lock is active.
- `show` holds only names whose value changed since the previous reply (numbers → float, text → string ≤ 200 chars,
  pictures → u8 `[H,W,3]` array).
- `did` (check mode only): descriptions of effects that would have run, e.g. `"would hold A"`, `"would tap Space"`,
  `"would hold game button jump"`, `'would send {"type":"restart"}'`.
- `warnings`: one-time runtime warnings `{"script","line","message"}` (e.g. points() returned None).
- `ms`: wall time per hook for this request (milliseconds).

### 3.7 `error` (worker → app, instead of the normal reply)

```json
{"t":"error","id":9,"fatal":false,"kind":"exception","script":"points.py","line":7,"col":12,"func":"points",
 "exc_type":"NameError","message":"name 'scor' is not defined","hint":"Is it spelled the same as where you made it?",
 "trace":["points.py:7 in points","inputs.py:3 in see"]}
```

- `kind`: `"syntax"`, `"load"`, `"exception"`, `"bad_return"`, `"timeout"`, `"input"`, `"focus"`, `"protocol"`.
- Location = the **deepest traceback frame inside the project folder**; for `bad_return` it is the hook's `def`
  line; for `syntax` the SyntaxError's `lineno`/`offset`. `script` is relative to the project folder. If no
  frame is inside the project, `script` is `""`.
- `line` is 1-based. `col` counts Unicode characters, 1-based (a tab is one character), like `SyntaxError.offset`
  and QScintilla's line/index (which counts from 0: index = `col` − 1); 0 when unknown. For "unindent does not
  match any outer indentation level", `col` is the line's first non-blank character.
- `fatal: true` for anything during `load`; the app ends the run. Otherwise the app pauses the run (keys are
  already released by the worker before it sends the error) and the user may fix the file and resume, which
  respawns the worker.
- `message` and `hint` are plain English for beginners.

## 4. Script semantics

### 4.1 Hooks

Hook names: `see` (required), `points` (required), `game_over`, `restart`, `step`. A hook is a function defined
in (not imported into) one of the user modules (`fn.__module__` is that module). Defined in more than one script →
fatal load error naming both places. A hook takes **0 or 1** required positional parameters; with 1 it receives
the shared `world` object. Anything else → fatal load error with the `def` line.

Return validation:

| Hook | Accepts | Errors |
|---|---|---|
| `see` | picture or numbers (§4.4) | anything else (`bad_return`) |
| `points` | int, float, bool (→1/0), numpy scalar; `None` → 0 with a one-time warning | NaN/inf, str, list... (`bad_return`) |
| `game_over` | any value; truthiness decides (`None` = not over) | — |
| `restart`, `step` | return value ignored | — |

### 4.2 Choices (responses.py)

- If `responses.py` defines a dict named `RESPONSES`, its keys (strings) in order are the choices and its values
  the effects.
- Otherwise every top-level name in `responses.py`, **in definition order** (module `__dict__` order), is a choice if:
  it is bound by a statement in `responses.py` itself (names that only `from x import *` brings in, and names from
  `from vyrn import`, are not choices), it does not start with `_`, it is not a hook name, and its value is a
  `str`, `None`, or a function whose `__module__ == "responses"`. Modules, classes, imported functions and other
  values (numbers, lists...) are ignored.
- Fewer than 2 choices → fatal load error ("the AI needs at least 2 choices").
- Choice functions take 0 or 1 required parameter (the `world`), like hooks.
- In a sim, a string effect is a fatal load error ("simulations can't press keys; use a function").

### 4.3 Effect strings

```
effect := ["tap "] combo          combo := key ("+" key)*      (case-insensitive, spaces around "+" allowed)
        | ["tap "] "game " buttons                        buttons := button ("+" button)*
```

- **Game button** effect (`"game jump"`, `"game left+jump"`, `"tap game jump"`): buttons of a game connected with
  Vyrn Link (§3.4), held while the AI keeps choosing the choice, or tapped once. A button name is 1-32 letters,
  digits, `_` or `-`, lowercased (`"game Jump"` is `jump`). Nothing is pressed on this computer, so game buttons
  need no `allow_input`, and choosing one releases held keys like any choice that isn't a key. Described as
  `hold game button jump`, `hold game buttons left+jump`, `tap game button jump`. In a simulation a game button is a
  fatal load error ("a simulation has no game to press buttons in"); `vyrn.press("game jump")` is an error too (a
  load error when the text is written in the call).

- **Held** effect (`"a"`, `"shift+d"`, `"a+space"`): the keys are held while the AI keeps choosing it. On each
  `act` the worker computes the new held set and releases `old - new`, presses `new - old` (modifiers
  pressed first, released last). Choosing `None`, a function, or a tap effect makes the held set empty.
- **Tap** effect (`"tap space"`): press now, release 50 ms later on a timer thread (or earlier, before the next key
  action). The reply does not wait for the release.
- Allowed key names (and the evdev code each maps to):
  - letters `a`–`z`, digits `0`–`9`, `f1`–`f12`
  - `space`, `enter`/`return`, `tab`, `esc`/`escape`, `backspace`, `delete`, `insert`, `home`, `end`,
    `pageup`, `pagedown`, `up`, `down`, `left`, `right`
  - `shift` (left shift), `ctrl`/`control` (left ctrl)
  - punctuation: `-` `=` `[` `]` `;` `'` `` ` `` `\` `,` `.` `/` and their names `minus`, `equal`, `bracketleft`,
    `bracketright`, `semicolon`, `apostrophe`/`quote`, `grave`/`backtick`, `backslash`, `comma`, `period`/`dot`,
    `slash`
- **Refused** at load with a plain reason: `alt`, `altgr`, `super`/`win`/`windows`/`meta`/`cmd`, `capslock`,
  `numlock`, `scrolllock`, `print`/`printscreen`/`sysrq`, `power`, `sleep`, `suspend`, `wake`, media/volume keys,
  right-hand modifiers. Unknown names → fatal load error with a did-you-mean (`difflib.get_close_matches`).
- The virtual keyboard device declares **only** the allowed keys.

### 4.4 Observations from `see()`

Pictures (sent as u8 `[H,W,3]`):
- numpy uint8 array `[H,W]` (gray → copied to 3 channels), `[H,W,1]`, `[H,W,3]`, `[H,W,4]` (alpha dropped)
- a `PIL.Image` (converted to RGB), a `vyrn.canvas` (its pixels)
- float/other dtypes → `bad_return` with hint "pictures are whole numbers 0-255: use .astype('uint8')"
- 1 ≤ H, W ≤ 4096; in a simulation also H×W×3 ≤ 32 MiB − 32 bytes, because a round-end result carries `obs` and
  `final_obs` in one frame (else a `bad_return`, fatal at load). A picture whose size changes during a run is
  allowed (one-time warning).

Numbers (sent as f32 `[D]`):
- an int/float/bool, a flat list/tuple of numbers, a 1-D numeric numpy array, or a dict `name → number`
  (`names` = the keys as strings, in order; otherwise `names` = `null`)
- 1 ≤ D ≤ 4096; NaN/inf → `bad_return`; nested lists → `bad_return` ("return a flat list of numbers, or a
  picture as a numpy array")
- The kind, and for numbers D and the names, must not change during a run (`bad_return`).

## 5. Input safety (enforced in the worker)

- `allow_input: false` (the default; always false in sims) → any key action (`act` with a key effect,
  `vyrn.press`) raises error kind `"input"`: "This run isn't allowed to press keys. Use Train in Vyrn: it
  counts down and asks you to click your game first." In real time the `load` and check-mode rules below come
  first; in a sim every key action is an `"input"` error ("Simulations can't press keys"), also then.
- Key actions while `load` runs (top-level script code, the first `see()`) are never sent: they are skipped and
  reported once (per line) as a spec warning.
- `check: true` → key actions are **never executed**; they are described in `did` (`"would hold A"`). In real
  time, choice functions are described too (`"would call jump()"`), never run, and the spec warns about this.
  `vyrn.wait()` does not sleep in check mode. A real-time Check (the app's and `check.py`) runs `restart()` once,
  then 5 `observe` ticks; a `game_over()` that never comes true in real time is a note, not a warning.
- In real time, `load` checks every literal key in `vyrn.press("…")` in the project's scripts without running
  them, so also in functions a Check never runs: an unknown or refused key is a fatal `load` error with a
  did-you-mean.
- `vyrn.press()` returns after its key-up.
- The virtual keyboard (Linux `evdev.UInput`; macOS Quartz events, which need the Accessibility permission; Windows
  `SendInput` with scan codes) is created only after `lock_focus` (or lazily on the first allowed key action; on
  Linux then waiting 200 ms for the compositor). It is never created in a sim, in check mode, while `load`
  runs, or after `close`. After `close` nothing is pressed; the worker exits after `bye` even if threads the
  scripts started still run.
- **Focus guard**: after `lock_focus`, every key-down first checks that the locked window is still active;
  if not, all keys are released and nothing is sent (`focused: false` in results). It fails closed: a stopped
  helper or a closed window counts as not active. The watchdog also releases held keys within about 50 ms once
  the locked window stops being active.
- **Freeze**: `SIGUSR1` (Windows: control byte `F`) → release all keys immediately and refuse key actions (silently
  skip) until `resume`. Handled on the watchdog thread (via `signal.set_wakeup_fd`, or reading the control
  connection), so it works while user code is stuck.
- **Watchdog thread** (always running) notices the app died → release all keys → `os._exit(1)`: Linux polls fd 3 for
  `POLLRDHUP|POLLHUP|POLLERR`; macOS waits with kqueue for `EVFILT_PROC NOTE_EXIT` on the parent pid (the app) and for
  EOF on fd 3; Windows sees EOF on the control connection. **Dead-man timer** (real time, while any key is held): no
  request other than `status` for `max(0.5 s, 3 / decisions_per_second)` → release all keys.
- `SIGINT` (sent by the app on a timeout; Windows: control byte `I`, on which the watchdog calls
  `_thread.interrupt_main()`) → the watchdog releases all keys, and the main thread raises `KeyboardInterrupt` only
  while user code runs (never in the host's own code, so no reply is cut in half) → `error` kind `"timeout"` with
  the stuck line ("points() took too long — is there a loop that never ends?").
  The app SIGKILLs the group (Windows: terminates the job) shortly after either way.
- **Stuck code**: when user code starts while keys are held (real time), a `faulthandler` timer of
  2 × max(1 s, 5 ticks) + 5 s ends the worker if that code never returns. It needs no GIL (one long C call can
  keep the watchdog from running); the kernel then releases the keys. The app escalates an unanswered freeze or
  timeout to SIGKILL.
- `SIGTERM` → on the watchdog thread: release keys, close the keyboard, `os._exit(0)` (user `atexit` handlers
  don't run).
- On every exit path (close, error, exception, signal, `atexit`) held keys are released. On Linux the kernel also
  releases them when the uinput device closes (covers SIGKILL); on macOS and Windows synthetic keys don't come up by
  themselves, so the app sends key-ups for every allowed key after killing a worker that used keys
  (`platform::release_all_keys`, docs/dev/platforms.md section 2).

## 6. Focus helper (`vyrn-focus`, built from `focus/vyrn-focus.c`)

Speaks `zwlr_foreign_toplevel_management_unstable_v1`. Writes one line per event to stdout (UTF-8, `\t`-separated,
tabs/newlines inside titles replaced by spaces) and flushes:

```
ready                                  initial state complete (after the first roundtrips)
active<TAB><id><TAB><app_id><TAB><title>   a window became active, or the active window's title/app_id changed
none                                   no window is active
closed<TAB><id>                        a window closed
error<TAB><message>                    protocol unavailable etc.; then exits with status 2
```

`<id>` is a small integer the helper assigns per toplevel handle, stable for that window's lifetime. The helper
exits when its stdin reaches EOF (the worker keeps a pipe open) or on SIGTERM. The worker locks onto `<id>`, not
the title (titles change).

The helper is Linux-only. On macOS and Windows the worker produces the same lines itself, polling 20 times a second
(and once more right before each key-down): macOS `CGWindowListCopyWindowInfo` (the first on-screen layer-0 window:
`<id>` = its owner's pid, `<app_id>` the owner's bundle identifier or name, `<title>` the owner's name; the lock holds
for that pid + name only; a failing call, no layer-0 window or an unreadable layer/pid = `none`), Windows
`GetForegroundWindow` (`<id>` = the window handle, `<app_id>` the program's file name such as `game.exe`, `<title>`
the window text). The active window is "Vyrn itself" when its
`app_id` is `vyrn`, `vyrn.desktop`, `Vyrn.exe`, a bundle id ending in `.vyrn`, or it belongs to the worker's parent
process (the app).

## 7. Timeouts (enforced by the app)

| Request | Timeout |
|---|---|
| hello (after spawn) | 15 s |
| load, reset, restart, lock_focus | 30 s |
| step, check_over | 5 s |
| observe, act | real time: max(1 s, 5 ticks) |
| status, pause, resume | 2 s |
| close | 2 s, then SIGKILL |

On timeout the app sends `SIGINT` to the worker **pid** (not the group; Windows: `I` on the control connection), waits
for the `timeout` error, and SIGKILLs the **group** (Windows: terminates the job) 2 s later (sim) or 0.5 s later (real
time). EOF or a protocol error → SIGKILL the group
immediately and report "Python stopped" with the last 20 stderr lines.

## 8. Project files

```
<project>/vyrn.json      settings (below)        <project>/pictures/   Snapshot PNGs
<project>/inputs.py  responses.py  points.py  [world.py]   [other .py helpers, e.g. demo_game.py]
<project>/brains/        checkpoints             <project>/runs/       CSV logs
```

`vyrn.json` (all keys optional; unknown keys preserved):

```json
{"name":"Catch","template":"catch-pictures","decisions_per_second":10,"round_seconds":60,"round_ticks":1000,
 "start_program":"","python":"","game_window":"","brain":"","advanced":{"explore_ticks":20000,"frames":4}}
```

`round_ticks` is the sim round limit; real time uses `round_seconds × decisions_per_second`.

**Screen coordinates** (`vyrn.screen()`, `vyrn.pixel()`, `vyrn.on_screen()`, Pick area, and the Snapshot PNG text
`vyrn-area` = `"x,y,w,h"`): the main screen's own pixels on Linux (display scale 100%) and Windows, **points** on
macOS (logical coordinates; one point is 2x2 pixels on a Retina screen). The worker scales every macOS capture to
one pixel per point, so an area of `w x h` is always a `w x h` picture. A Snapshot should be saved with one pixel per
point too; one saved with the screen's pixels (bigger than its `vyrn-area`) is scaled down before comparing.
`"link": {"port": 7457, "lockstep": false, "frames_per_step": 1}` (optional; `true` = these defaults) means the
project's game connects over Vyrn Link (docs/dev/link.md); without it Vyrn never listens.
