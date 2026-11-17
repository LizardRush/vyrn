# Vyrn Link: games talk to Vyrn over WebSocket (Geometry Dash via Geode, Godot, browser games, …)

Status: built. Part 1: the server (`core/src/link/`, library `vyrn_link`), the worker side (protocol.md §3.2, §3.4,
§4.3) and the test project `link-runner` (tests/fixtures; not a preset). Part 2: the real-time runner, the pairing code, the app
and vyrn-cli ("Real-time runner and app" below). The Geode mod itself is the user's (docs/user/vyrn-link.md).

## Why

For games that can be modded, a mod can *send* the game state and *receive* the AI's choices. That beats reading
pixels off the screen and pressing desktop keys:

- **Exact numbers:** the player's position and speed and the obstacles ahead, instead of an 84×84 screenshot.
- **Speed:** no screen capture and no keyboard injection, so no focus guard or countdown is needed; nothing on
  the desktop is touched.
- **Remote play:** the game can run on another computer. Geometry Dash does not run on the Pi, so a PC with Geode
  talks to Vyrn on the Pi over the LAN.

"The app handles the networking": the **app** (C++ core) hosts the server and keeps the connection open across
worker restarts. The Python scripts only see `vyrn.game` and use `"game …"` effects.

## User-facing Python (three files stay tiny)

```python
# inputs.py: what the AI sees
import vyrn

def see():
    g = vyrn.game                         # newest state from the game (a dict; {} until it sends one)
    return [g.get("y", 0), g.get("speed_y", 0), g.get("on_ground", 0)]
```
```python
# responses.py: game buttons are held while the AI keeps choosing them
jump = "game jump"
wait = None
```
```python
# points.py
import vyrn

def points():
    if game_over():
        return -10
    return 1

def game_over():
    return vyrn.game.get("dead", False)

def restart():
    vyrn.send("restart")
```

| Name | Meaning |
|---|---|
| `vyrn.game` | The latest `state` message from the game (a dict, `{}` before the first one). A missing key raises a `KeyError` that explains itself; `.get()` works as usual |
| `vyrn.events` | The game's `event` messages since the last tick (list of dicts) |
| `vyrn.connected` | Whether a game is connected |
| `vyrn.send(type, **fields)` | Send `{"type": type, **fields}` to the game with the AI's next decision (`vyrn.send({"type": …})` works too). Not `input`/`stop` (Vyrn's own) |

Effects (protocol.md §4.3): `"game <button>"` holds the button while the AI keeps choosing it, `"tap game <button>"`
presses it once, `"game left+jump"` holds both. Each tick the AI's buttons go to the game as one
`{"type":"input","frame":f,"hold":[…],"tap":[…]}`: `hold` is the full set of buttons down, so switching choices
releases the others. Check describes them (`would hold game button jump`, `would send {"type":"restart"}`).

A project uses the link when `vyrn.json` has `"link": {"port": 7457}` (plus `"lockstep"` and `"frames_per_step"`;
`"link": true` means the defaults). The templates add it, and Settings shows it.

## Wire protocol for game clients

Frozen as **Vyrn Link protocol v1** in `docs/user/vyrn-link.md`, the document for mod authors. In short:
- **Handshake:** `hello` (with the pairing code) → `welcome` (buttons, `lockstep`, `frames_per_step`, `tick_hz`).
- **Game → Vyrn:** `state` (often; the newest becomes `vyrn.game`) and `event` (one-shot, into `vyrn.events`).
- **Vyrn → game:** `input` (`hold`: the full set of buttons down until the next input; `tap`: one-frame presses;
  `frame`: the state it answers), `stop`, and anything from `vyrn.send()`.
- **Lockstep:** the game sends one `state`, waits for the matching `input`, then advances `frames_per_step` frames.

Reference implementations, Python standard library only:
- `tools/link/link_test_server.py`: a stand-in for Vyrn, for developing a mod before the app supports Link.
- `tools/link/link_fake_game.py`: a fake runner game client (the `link-runner` template plays it; the GTests use it).
- `tools/link/wslite.py`: shared WebSocket code.

## The server (`core/src/link/link_server.h`)

`vyrn_link` is a static library (C++17, standard library, POSIX sockets, nlohmann_json; no torch, no Qt), linked
into `vyrn_core`. `LinkServer` listens on `0.0.0.0:<port>` and runs one IO thread with `poll()`:

- RFC 6455: HTTP upgrade (a browser gets a 426 with a one-line explanation), masked client frames, fragmentation,
  ping/pong (and pings of its own to a quiet game: 5 s; a game silent for 20 s is dropped), the closing handshake.
- Limits: 1 MiB per message (fragments together); text frames with valid UTF-8 JSON objects only; 8 connections
  waiting for their hello (10 s each); one game at a time, others get `busy` (unless the connected game stopped
  answering: see "Server hardening" below). The pairing code (6 digits, never a leading 0) isn't needed from
  127.0.0.1, except from a web page (an `Origin` header); wrong codes are limited to 5 per address and 20 in all per
  minute.
- Every refusal sends `{"type":"error","code":"bad_code"|"busy"|"protocol"|"bad_message","message":…}`, then a
  close frame, then waits for the game's close before hanging up. Types Vyrn doesn't know are ignored.
- Thread-safe API for the runner: `connected()`, `client()`, `latest_state()` (text and parsed JSON, `frame`,
  `seq`), `drain_events()`, `send_input(frame, hold, tap)`, `send_stop()`, `send_custom(msg)`,
  `wait_state(newer_than_seq, timeout)`, `step(answered_state, hold, tap, timeout)` (lockstep), `wait_connected()`,
  `interrupt_waits()`, `set_welcome()`, `stats()`, `take_notices()` (console lines like "Geometry Dash (my-mod
  0.1) connected from 192.168.1.20"). `link_settings(vyrn_json)` reads the project's `"link"` (nullopt = no Link).
- `stop()` sends `{"type":"stop"}`, closes with 1001 and frees the port.

## Real-time runner and app (part 2, built)

Link projects are real-time projects (no `world.py`) whose effects are game buttons, not keys. They need **no**
`allow_input`, countdown or focus lock when `spec.uses.keys` is false, because nothing is pressed on this desktop
(projects that also press keys keep the whole safety flow). Agents can't train them (MCP trains simulations only),
but they can Check them without the person's screen permission when the scripts don't read the screen (the worker
refuses the screen in such a Check: mcp.md §4, protocol.md §3.2). `vyrn_settings` accepts `link` (mcp.md §3).

**`GameLink`** (`core/src/run/game_link.{h,cpp}`) is the run's side of the server; `RealtimeRunner`
(`rt_runner.cpp`) owns one for Check, Train and Watch:

- **Start/stop:** when `link_settings(settings.raw)` is set, the server starts *before* Python loads, with
  `welcome.project` = the project's name, `tick_hz` = decisions per second, `lockstep`/`frames_per_step` from the
  settings and the project's pairing code; after the spec (and after every respawn), `set_welcome()` gets
  `spec["buttons"]`. It stays up across worker respawns and stops (`{"type":"stop"}` + close 1001) when the run
  ends, before the brain is saved. A `LinkError` (port in use, …) is a fatal run error, reason `link` (vyrn-cli exit
  status 2); Check only warns and goes on without the game. Tests listen on 127.0.0.1
  (`test_hooks::link_bind_address`); a run listens on every network (`0.0.0.0`).
- **Pairing code:** `<project>/.vyrn-link-code` (6 digits, mode 0600; `vyrn/link.h`: `link_code()` makes it when it
  is missing, `new_link_code()` replaces it). Hidden files are never put in `.vrlp` files, and `vyrn.json` never
  holds the code, so it stays on this computer. `lan_addresses()` ranks this computer's IPv4 addresses (Wi-Fi and
  Ethernet home networks first, VPNs and containers last); `link_url(port)` = `ws://<best>:<port>/`.
- **Status:** a helper thread forwards `take_notices()` to the console (`Vyrn Link: Fake Runner (…) connected from
  …`) and emits a **`link` event** at the start and whenever a game connects or leaves: `url`, `address`, `port`,
  `code`, `lockstep`, `frames_per_step`, `connected`, and with a game `game`, `client`, `from`. The code is only in
  that event and in the "Waiting for the game" state, never in a console line (MCP's Check returns console lines
  to agents). vyrn-cli prints the event once ("Vyrn Link: connect your game to ws://… with the pairing code …").
- **Waiting for the game** (`wait_for_game()`): before restart(), and whenever the game isn't ready (not connected;
  in real time also no state for over max(1 s, 3 ticks): paused, in a menu), the AI holds like a hold: `cut()`,
  `hold` event with reason `game`, state `holding` "Waiting for the game: connect it to ws://… with the pairing code
  … (on this computer: ws://127.0.0.1:…/)", `pause` to the worker, `stop` to a connected game, clock paused. A
  pause or a hold by the app takes over; Stop, pause, hold, resume and apply call `interrupt_waits()` through
  `Controls::set_waker()`, so every wait ends at once. A new connection also cuts the open segment.
- **Real-time tick:** `observe` carries `connected`, `game` (the newest state's value) and `game_events` (drained);
  every reply's `send` is forwarded with `send_custom()` (restart, check_over, observe and act replies) before the
  tick's `input`, which answers that state's frame (`send_input()`; nothing without a state, nothing to a newer
  connection). A request stays within the worker's 1 MiB JSON limit: the oldest events go first, then the state's
  biggest fields (never `type`/`frame`), each with one plain warning per run.
- **Lockstep tick:** `next_state()` = `wait_state(answered_seq, 0.25 s)`, then observe → act → `send_input()` for
  that state; every state is answered once. On timeout it keeps waiting and after 2 s says "Lockstep: waiting for
  the game's next state…"; Disconnected goes to waiting for the game. Train lets the learner keep up
  (`wait_for_learning()`: at most 4 updates behind `updates_per_tick` per tick, at most 1 s per tick), so lockstep
  learns per tick like a simulation instead of outrunning the network; the pace comes from the game and the
  learner, not the clock.
- **Rounds:** a game over sends `stop` in lockstep (the game plays on by itself and can restart) or an empty input in
  real time (buttons up). In lockstep `stop` also goes out before every restart(). restart()'s `send` (the template
  sends `restart`) reaches the game; `check_over` is polled with fresh states only: ticks without a new state don't
  count towards `restart_stuck` (a game in a menu is waited for). The first input of the round puts a lockstep game
  back into lockstep. The round limit counts ticks as usual.
- **Pause/stop:** holds, errors and worker restarts (`close_worker()`, `respawn()`) send `stop`.
- **Check:** listens too; waits up to 3 s after listening for a game to connect (a mod retries every couple of
  seconds) and send its first state, then attaches its state to restart and the 5 observes. Nothing is ever sent to the game. Without a game
  (or in lockstep, where the state can't change) it doesn't warn about inputs that never change; without a game it
  adds a note. It warns when `spec.uses.game` is true but `vyrn.json` has no `"link"` (Train and Watch too).

**Server hardening** (`link_server.cpp`): a handshake with an `Origin` header (a web page: any page can open
`ws://127.0.0.1`) needs the pairing code even from 127.0.0.1. A correct-code `hello` while another game is
connected: if that game has been quiet for `replace_quiet_s` (1 s), Vyrn pings it; when it doesn't answer within
`replace_probe_s` (1.5 s), the new game takes its place (the old one gets `stop` and close 1001), so a game that
crashed or lost the network reconnects at once. A game that is sending or answers the ping keeps its place: `busy`.

**App:** the project window shows a **Vyrn Link bar** above the editor for Link projects: the address and the code
while idle, "waiting for your game" while a run listens, and the connected game (name, client, address, lockstep).
**Copy address** copies the URL. The status bar says "Waiting for the game" during a `game` hold. **Settings → Vyrn
Link** (real games): on/off (`"link": false` turns it off), port (1024-65535), the address, the pairing code and
**New code**, lockstep and frames per step; other keys inside `"link"` are kept.

**Projects:** `link-runner` is a test-only project in `tests/fixtures/` (no longer a preset; there is no geometry-dash template). `tools/link/` is
installed to `share/vyrn/tools/link/`, so the link-runner fixture works with an installed Vyrn;
`link_fake_game.py --reconnect` keeps reconnecting like a mod.

Tests: `tests/cpp/link/test_link_server.cpp` (Origin, replacing a silent game), `tests/cpp/link/test_pairing.cpp`
(the code file, addresses), `tests/cpp/run/test_rt_link.cpp` (the runner's Link path with a fake worker and a
WebSocket game: real time, lockstep, waiting, reconnecting, a port in use, Check, big states),
`tests/e2e/test_link_e2e.py` (ctest `e2e_link`: vyrn-cli trains link-runner with the fake game in real time and
lockstep, the pairing code, Watch) and `tests/gui/test_link_gui.py` (ctest `gui_link`: the app's Link bar during
Check and Train). The user guide is `docs/user/07-geometry-dash.md`.

## Geometry Dash mod

**The user writes the Geode mod** against `docs/user/vyrn-link.md`, testing it with `tools/link/link_test_server.py`
(or straight against a project that trains). Vyrn used to ship a geometry-dash template (removed; see docs/user/07-geometry-dash.md) (the three files + `vyrn.json` with
`"link"`), which reads the suggested state fields from that document.
