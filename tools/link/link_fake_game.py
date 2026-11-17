#!/usr/bin/env python3
"""A tiny fake game that speaks Vyrn Link (a runner jumping over spikes).

Use it to try Vyrn Link without a real game, to test a Link server, or as a reference client when writing a mod.
It sends the same state fields the Geometry Dash template reads (docs/user/vyrn-link.md):
x, y, speed_y, on_ground, mode, percent, dead, ahead=[dx, dy, kind] * 5.

    python3 tools/link/link_fake_game.py ws://127.0.0.1:7457/ --code 483920
    python3 tools/link/link_fake_game.py ws://192.168.1.20:7457/ --code 483920 --reconnect   # like a mod

It prints one JSON line at the end: {"frames", "deaths", "completes", "attempts", "inputs", "connections"}.
"""
import argparse
import json
import random
import sys
import threading
import time

from wslite import WSClosed, WSError, connect

FPS = 60
SPEED = 6.0            # units per frame
GRAVITY = -1.1         # units per frame²
JUMP = 13.0            # initial jump speed
LEVEL_LENGTH = 6000.0
SPIKE_W = 30.0


class Runner:
    """The game: the player runs right; spikes must be jumped over; reaching the end completes the level."""

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.attempts = 0
        self.restart()

    def restart(self):
        self.attempts += 1
        self.x, self.y, self.vy = 0.0, 0.0, 0.0
        self.dead = False
        self.complete = False
        self.spikes, pos = [], 400.0
        while pos < LEVEL_LENGTH - 200:
            pos += self.rng.uniform(220, 520)
            self.spikes.append(pos)

    def step(self, holding_jump: bool, tapped: bool):
        """Advance one frame. Returns an event name or None."""
        if self.dead or self.complete:
            return None
        on_ground = self.y <= 0.0
        if on_ground and (holding_jump or tapped):
            self.vy = JUMP
        self.vy += GRAVITY
        self.y = max(0.0, self.y + self.vy)
        if self.y == 0.0:
            self.vy = 0.0
        self.x += SPEED
        for s in self.spikes:
            if s - SPIKE_W / 2 < self.x < s + SPIKE_W / 2 and self.y < 25.0:
                self.dead = True
                return "death"
        if self.x >= LEVEL_LENGTH:
            self.complete = True
            return "complete"
        return None

    def state(self, frame):
        ahead = []
        for s in self.spikes:
            if s + SPIKE_W / 2 > self.x and len(ahead) < 15:
                ahead += [round(s - self.x, 1), 0.0, 1]
        ahead += [0, 0, 0] * ((15 - len(ahead)) // 3)
        return {"type": "state", "frame": frame, "x": round(self.x, 1), "y": round(self.y, 1),
                "speed_y": round(self.vy, 2), "on_ground": int(self.y <= 0.0), "mode": 0,
                "percent": round(min(100.0, 100.0 * self.x / LEVEL_LENGTH), 2), "dead": self.dead,
                "ahead": ahead}


def play(ws, game, welcome, args, log, totals):
    """Plays one connection until it closes, the time is up or Ctrl+C. Returns False when the run should end."""
    lockstep = bool(welcome.get("lockstep"))
    fps_step = max(1, int(welcome.get("frames_per_step", 1)))
    log(f"connected to {welcome.get('vyrn')} ({welcome.get('project')}); "
        f"{'lockstep' if lockstep else 'real time'}, buttons {welcome.get('buttons')}")

    lock = threading.Condition()
    inputs = {"hold": set(), "taps": 0, "last_frame": None, "restart": False, "stopped": False,
              "closed": False, "count": 0}

    def reader():
        try:
            while True:
                msg = ws.recv_json()
                if msg is None:
                    break
                with lock:
                    kind = msg.get("type")
                    if kind == "input":
                        inputs["hold"] = set(msg.get("hold", []))
                        inputs["taps"] += len(msg.get("tap", []))
                        inputs["last_frame"] = msg.get("frame")
                        inputs["stopped"] = False
                        inputs["count"] += 1
                    elif kind == "stop":
                        inputs["hold"], inputs["stopped"] = set(), True
                    elif kind == "restart":
                        inputs["restart"] = True
                    elif kind == "error":
                        log(f"server error: {msg.get('message')}")
                    lock.notify_all()
        except (WSClosed, WSError, OSError, ValueError):
            pass
        finally:
            with lock:
                inputs["closed"] = True
                lock.notify_all()

    threading.Thread(target=reader, daemon=True).start()
    frame = totals["frames"]
    respawn_at = None
    next_frame = time.monotonic()
    last_log = time.monotonic()
    counted = 0
    hold = False
    keep_going = True
    try:
        while True:
            if args.seconds and time.monotonic() - totals["started"] > args.seconds:
                keep_going = False
                break
            with lock:
                if inputs["closed"]:
                    break
                if inputs["restart"]:
                    inputs["restart"] = False
                    game.restart()
                    respawn_at = None
                    ws.send_json({"type": "event", "name": "reset"})
                waiting = lockstep and not inputs["stopped"]
            if waiting:
                # Lockstep: send the state for this frame, then wait for the input that answers it.
                ws.send_json(game.state(frame))
                with lock:
                    ok = lock.wait_for(lambda: inputs["closed"] or inputs["stopped"] or inputs["last_frame"] == frame,
                                       timeout=10)
                    if inputs["closed"]:
                        break
                    if inputs["stopped"]:
                        continue          # Vyrn paused or the round ended: play on by itself (below)
                    if not ok:
                        log("no input for 10 s: is the AI paused?")
                        continue
                    hold, taps = "jump" in inputs["hold"], inputs["taps"] > 0
                    inputs["taps"] = 0
                steps = fps_step
            else:
                # Real time, or lockstep after a stop: play at the game's own speed until an input comes.
                now = time.monotonic()
                if now < next_frame:
                    time.sleep(next_frame - now)
                    next_frame += 1.0 / FPS
                else:
                    next_frame = now + 1.0 / FPS
                with lock:
                    hold, taps = "jump" in inputs["hold"], inputs["taps"] > 0
                    inputs["taps"] = 0
                steps = 1
            for _ in range(steps):
                event = game.step(hold, taps)
                taps = False
                frame += 1
                if event:
                    if event == "death":
                        totals["deaths"] += 1
                    else:
                        totals["completes"] += 1
                    ws.send_json({"type": "event", "name": event, "percent": game.state(frame)["percent"]})
                    if not lockstep:
                        respawn_at = time.monotonic() + 1.0  # like a game: restart by itself after a second
                    break
            if not waiting:
                if respawn_at and time.monotonic() >= respawn_at:
                    game.restart()
                    respawn_at = None
                ws.send_json(game.state(frame))
            totals["frames"] = frame
            with lock:
                totals["inputs"] += inputs["count"] - counted
                counted = inputs["count"]
            if time.monotonic() - last_log >= 1.0:
                last_log = time.monotonic()
                log(f"frame {frame}  attempt {game.attempts}  {game.state(frame)['percent']:5.1f}%  deaths "
                    f"{totals['deaths']}  inputs {totals['inputs']}  {'HOLD' if hold else 'up'}")
    except (WSClosed, WSError, OSError) as e:
        log(f"connection ended: {e}")
    finally:
        ws.close()
    return keep_going


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url", nargs="?", default="ws://127.0.0.1:7457/")
    ap.add_argument("--code", default="")
    ap.add_argument("--seconds", type=float, default=0, help="quit after this many seconds (0 = run until stopped)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--reconnect", action="store_true",
                    help="connect again every 2 seconds when Vyrn isn't there or the connection ends, like a mod")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    game = Runner(args.seed)
    log = (lambda *a: None) if args.quiet else (lambda *a: print(*a, flush=True))
    totals = {"frames": 0, "deaths": 0, "completes": 0, "attempts": 0, "inputs": 0, "connections": 0,
              "started": time.monotonic()}
    status = 0

    def time_left():
        return not args.seconds or time.monotonic() - totals["started"] < args.seconds

    try:
        while time_left():
            try:
                ws = connect(args.url)
                ws.send_json({"type": "hello", "protocol": 1, "game": "Fake Runner", "client": "link_fake_game 1.0",
                              "code": args.code})
                welcome = ws.recv_json()
            except (WSClosed, WSError, OSError, ValueError) as e:
                if not args.reconnect:
                    print(f"can't connect to {args.url}: {e}", file=sys.stderr)
                    status = 1
                    break
                log(f"can't connect to {args.url} ({e}); trying again in 2 s")
                time.sleep(2)
                continue
            if not isinstance(welcome, dict) or welcome.get("type") != "welcome":
                ws.close()
                code = welcome.get("code") if isinstance(welcome, dict) else None
                if args.reconnect and code == "busy":
                    log("another game is connected; trying again in 2 s")
                    time.sleep(2)
                    continue
                print(f"refused: {welcome}", file=sys.stderr)
                status = 2
                break
            totals["connections"] += 1
            if not play(ws, game, welcome, args, log, totals) or not args.reconnect:
                break
            log("disconnected; connecting again in 2 s")
            time.sleep(2)
    except KeyboardInterrupt:
        pass
    totals["attempts"] = game.attempts
    del totals["started"]
    print(json.dumps(totals))
    return status


if __name__ == "__main__":
    sys.exit(main())
