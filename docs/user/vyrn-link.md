# Vyrn Link protocol v1 (for game mods)

Vyrn Link lets a game, or a mod inside a game, stream its state to Vyrn and receive the AI's button presses over a
**WebSocket**. Vyrn is the server and the game is the client. The game can run on another computer, for example
Geometry Dash with Geode on your PC talking to Vyrn on a Raspberry Pi.

Nothing in this protocol is specific to Geometry Dash. Any game, mod, Godot project or web page that can open a
WebSocket can use it.

## Connecting

- **URL:** `ws://<vyrn-host>:7457/`. The port is set per project in Vyrn under Settings → Vyrn Link. On the same
  machine use `ws://127.0.0.1:7457/`.
- **Frames:** text only. Each frame is one UTF-8 JSON object of at most 1 MiB. Vyrn ignores fields it doesn't know,
  and you should too.
- **First message** (game → Vyrn):
  ```json
  {"type":"hello","protocol":1,"game":"Geometry Dash","client":"my-mod 0.1","code":"483920"}
  ```
  `code` is the 6-digit pairing code Vyrn shows for the project. It isn't needed from `127.0.0.1`, with one
  exception: a client whose WebSocket handshake has an `Origin` header (every web browser sends one) must send the
  code even from `127.0.0.1`, because any web page you open could otherwise connect to Vyrn.
- **Vyrn answers** with:
  ```json
  {"type":"welcome","protocol":1,"vyrn":"0.1.0","project":"GD bot","buttons":["jump"],
   "lockstep":false,"frames_per_step":1,"tick_hz":30}
  ```
  - `buttons`: the button names the project's choices use.
  - `tick_hz`: AI decisions per second (real-time mode).
  - `lockstep`, `frames_per_step`: see below.
- **Refusal:** if Vyrn refuses, it sends `{"type":"error","code":"bad_code"|"busy"|"protocol"|"bad_message","message":"…"}`
  and closes the connection. Only one game can be connected to a project at a time (`busy`).
- **Reconnect:** if the connection drops, reconnect every couple of seconds. Vyrn pauses the AI until you're back.
  If your game crashed or lost the network without closing the connection, Vyrn may still count the old connection
  as connected. A new `hello` (with the right code, when one is needed) can then take its place: when the old
  connection has sent nothing for about a second, Vyrn pings it and keeps the new `hello` waiting for up to about
  1.5 s. If the old connection doesn't answer, it gets `stop` and is closed, and the new one gets `welcome`. If it
  answers, or sent something during the last second, the new one gets `busy`.

## Game → Vyrn

| Message | When | Meaning |
|---|---|---|
| `{"type":"state","frame":1234, …your fields…}` | Real time: every frame, or at least `tick_hz` times a second. Lockstep: once per step | The newest state becomes `vyrn.game` in the user's Python. Keep fields flat and JSON-friendly: numbers, booleans, short lists of numbers |
| `{"type":"event","name":"death", …}` | Whenever something happens once | Added to `vyrn.events` for the next AI tick |

## Vyrn → game

| Message | Meaning |
|---|---|
| `{"type":"input","frame":1234,"hold":["jump"],"tap":[]}` | `hold`: buttons that must be **down** until the next `input`; every other button is up. `tap`: press and release, down for one frame. `frame` is the frame of the state this decision was based on. Sent once per AI tick in real-time mode, or once per state in lockstep |
| `{"type":"stop"}` | The AI paused or stopped: release all buttons and play normally (leave lockstep waiting) |
| anything else | Sent by the user's scripts with `vyrn.send(...)`. Suggested types: `restart`, `pause`, `resume`, `speed` (`{"type":"speed","value":2.0}`) |

## Lockstep mode (`welcome.lockstep` is true)

The game waits for the AI at every step. Runs are repeatable, there is no network jitter, and the game can run faster
than real time.

1. After `welcome`, send a `state` for the current frame `f`, then pause the game simulation.
2. Wait for the `input` whose `frame` is `f`. Apply `hold`/`tap`, advance `frames_per_step` physics frames, send the
   next `state` (frame `f + frames_per_step`), and pause again.
3. On `stop`, unpause and play normally. Keep sending states as in real-time mode until an `input` arrives again.
4. Never block the game's main thread while waiting. Skip simulation updates instead, so rendering and menus stay
   alive.

## Suggested state for Geometry Dash

This is a good set of fields for a Geometry Dash mod (the link-runner example project reads the same names). If you
want different fields, change both sides together.

```json
{"type":"state","frame":1234,
 "x":512.0,"y":105.0,"speed_y":0.0,"on_ground":1,"mode":0,"percent":12.5,"dead":false,
 "ahead":[90,0,1, 150,30,2, 0,0,0, 0,0,0, 0,0,0]}
```

- `mode`: 0 cube, 1 ship, 2 ball, 3 UFO, 4 wave, 5 robot, 6 spider, 7 swing.
- `ahead`: `dx, dy, kind` for the next 5 hazards in front of the player (zeros when fewer).
  `kind`: 1 spike/hazard, 2 solid block, 3 orb, 4 pad, 5 portal. Positions are relative to the player, in game units.
- Events: `{"type":"event","name":"death"}`, `{"type":"event","name":"complete"}`, `{"type":"event","name":"reset"}`.
- Handle `{"type":"restart"}` by restarting the level from the start.

## Tips for a Geode mod

- Run the WebSocket client on its own thread (for example IXWebSocket added with CPM). Hand received messages to the
  game thread with `geode::queueInMainThread`.
- Apply inputs through the game's own button handling. In GD 2.2 this path goes through
  `GJBaseGameLayer::handleButton(down, button, isPlayer1)`. Check the bindings of your Geode version.
- Restart with `PlayLayer::resetLevel`, or `resetLevelFromStart` in practice mode. Check your bindings here too.
- For lockstep, skip the game's physics update while waiting for Vyrn instead of sleeping.

## Test without Vyrn

`tools/link/link_test_server.py` stands in for Vyrn and needs only Python 3. Start it on the Pi or on your PC:

```bash
python3 tools/link/link_test_server.py --code 483920
```

It prints the addresses to connect to, checks your `hello`, prints what you send (a state summary every second and
every event), and sends `input`s. Type `j` + Enter to toggle holding jump, `t` + Enter to tap, `r` + Enter to send
`restart`, `s` + Enter to send `stop`, `q` + Enter to quit. Options:

- `--lockstep` and `--frames-per-step N` to test lockstep;
- `--policy every:20` to tap jump every 20 decisions with no typing;
- `--tick-hz 30`.

`tools/link/link_fake_game.py` is the other side: a tiny fake "jump over spikes" game that speaks the protocol. Run
it to see a correct client at work:

```bash
python3 tools/link/link_fake_game.py ws://127.0.0.1:7457/ --code 483920
```
