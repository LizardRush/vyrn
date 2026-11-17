"""The play loops of the standalone runtime: simulations, real time and Vyrn Link (docs/dev/standalone.md section 2).

The user's scripts run in-process through vyrn._host.Host, driven with the very requests the Vyrn app sends the
worker (load, reset/step, restart/check_over/observe/act), so discovery, conversion and error messages are the same
code as in the app. Nothing is learned here: the brain plays greedily (with a little exploring).
"""

import collections
import json
import os
import random
import sys
import threading
import time

from . import _brain, _vrl

COUNTDOWN_S = 3.0          # real time with keys: "click your game window", like the app
RESTART_EVERY_S = 3.0      # restart() again while game_over() stays True...
RESTART_GIVE_UP_S = 30.0   # ...and give up after this long
HOLD_POLL_S = 0.2


class PlayError(Exception):
    """Something to tell the person in plain words; the run ends."""

    def __init__(self, message, hint=""):
        super().__init__(message)
        self.message, self.hint = message, hint


class Stop(Exception):
    """--rounds reached."""


class LocalWatch:
    """What the host's watchdog thread waits on in-process: there is no app to lose, Ctrl+C is Python's own."""

    def wait(self, timeout):
        time.sleep(timeout)
        return []

    def interrupt_main(self):
        import _thread
        _thread.interrupt_main()


# -- project settings (vyrn.json) -----------------------------------------------------------------------------------

def _number(value, low, high, default, whole=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    if not (low <= value <= high) or (whole and value != int(value)):
        return default
    return int(value) if whole else float(value)


def read_settings(folder):
    """The settings the player needs from vyrn.json, with the app's defaults for anything missing or odd."""
    raw = {}
    try:
        with open(os.path.join(folder, "vyrn.json"), encoding="utf-8") as file:
            loaded = json.load(file)
        if isinstance(loaded, dict):
            raw = loaded
    except (OSError, ValueError):
        pass
    advanced = raw.get("advanced") if isinstance(raw.get("advanced"), dict) else {}
    link = raw.get("link")
    if link is True:
        link = {}
    elif not isinstance(link, dict):
        link = None
    if link is not None:
        link = {"port": _number(link.get("port"), 1024, 65535, 7457, True),
                "lockstep": link.get("lockstep") is True,
                "frames_per_step": _number(link.get("frames_per_step"), 1, 1000, 1, True)}
    name = raw.get("name") if isinstance(raw.get("name"), str) else ""
    return {"name": name.strip()[:100],
            "dps": _number(raw.get("decisions_per_second"), 1e-9, 1000, 10.0),
            "round_seconds": _number(raw.get("round_seconds"), 1e-9, 86400, 60.0),
            "round_ticks": _number(raw.get("round_ticks"), 1, 10**12, 1000, True),
            "eps_watch": _number(advanced.get("eps_watch"), 0.0, 1.0, 0.01),
            "link": link}


# -- what the person sees -------------------------------------------------------------------------------------------

class Reporter:
    def __init__(self, quiet, as_json):
        self.quiet, self.as_json = quiet, as_json
        self.recent = collections.deque(maxlen=20)
        self.rounds = 0
        self.total = 0.0
        self._said = set()

    def say(self, text):
        if not self.quiet:
            print(text, file=sys.stderr if self.as_json else sys.stdout, flush=True)

    def warn(self, text):
        if text not in self._said:
            self._said.add(text)
            print(text, file=sys.stderr, flush=True)

    def warnings(self, reply):
        for item in reply.get("warnings") or []:
            where = f"{item.get('script')}:{item.get('line')}: " if item.get("script") else ""
            self.warn(f"Note: {where}{item.get('message')}")

    def round(self, points, ticks):
        self.rounds += 1
        self.total += points
        self.recent.append(points)
        mean = sum(self.recent) / len(self.recent)
        if self.as_json:
            print(json.dumps({"round": self.rounds, "points": points, "ticks": ticks, "mean20": mean}), flush=True)
        elif not self.quiet:
            print(f"Round {self.rounds}: {points:g} points (average of the last {len(self.recent)}: {mean:.2f})",
                  flush=True)

    def summary(self):
        if self.rounds:
            self.say(f"Played {self.rounds} round{'s' if self.rounds != 1 else ''}; "
                     f"average {self.total / self.rounds:.2f} points.")


def describe_error(reply):
    where = ""
    if reply.get("script"):
        where = f"{reply['script']}" + (f":{reply['line']}" if reply.get("line") else "") + ": "
    return PlayError(where + str(reply.get("message") or "Something went wrong."), str(reply.get("hint") or ""))


class View:
    """--view: a small Tk window with what the AI sees (the newest picture, or its numbers)."""

    def __init__(self):
        import tkinter
        self.tk = tkinter
        self.root = tkinter.Tk()
        self.root.title("What the AI sees")
        self.label = tkinter.Label(self.root, bd=0, justify="left", font=("TkFixedFont", 11))
        self.label.pack()
        self.image = None
        self.alive = True
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def close(self):
        self.alive = False
        try:
            self.root.destroy()
        except Exception:
            pass

    def show(self, seen):
        if not self.alive:
            return
        try:
            import numpy as np
            seen = np.asarray(seen)
            if seen.ndim == 3:
                zoom = max(1, 256 // max(seen.shape[0], seen.shape[1]))
                big = np.repeat(np.repeat(seen, zoom, axis=0), zoom, axis=1).astype("uint8")
                data = b"P6 %d %d 255\n" % (big.shape[1], big.shape[0]) + big.tobytes()
                self.image = self.tk.PhotoImage(data=data, format="PPM")
                self.label.configure(image=self.image, text="")
            else:
                self.label.configure(image="", text="\n".join(f"{v:8.3f}" for v in seen.tolist()))
            self.root.update()
        except self.tk.TclError:
            self.alive = False


# -- the player -----------------------------------------------------------------------------------------------------

class Player:
    def __init__(self, folder, brain_path, opts):
        self.folder = os.path.abspath(folder)
        self.brain_path = brain_path
        self.opts = opts
        self.out = Reporter(opts.quiet, opts.json)
        self.settings = read_settings(self.folder)
        self.rng = random.Random(opts.seed)
        self.epsilon = opts.explore if opts.explore is not None else self.settings["eps_watch"]
        self.host = None
        self.brain = None
        self.spec = None
        self.view = None
        self.server = None
        self.ids = 0
        self.sim = os.path.isfile(os.path.join(self.folder, "world.py"))
        self.round_ticks = 0

    # -- start and stop --

    def setup(self):
        from .. import _host                     # numpy and Pillow only; sets nothing up yet
        from .._state import S
        file = _vrl.read_vrl(self.brain_path, os.path.basename(self.brain_path))
        self.brain = _brain.Brain(file)
        for name in ("inputs", "responses", "points"):
            if not os.path.isfile(os.path.join(self.folder, name + ".py")):
                raise PlayError(f"This folder has no {name}.py, so there is nothing for the AI to play.")
        sys.dont_write_bytecode = True
        try:
            from .. import _platform
            _platform.preload()
            import numpy.random  # noqa: F401
        except Exception:
            pass
        sys.path.insert(1, self.folder)
        self.S = S
        S.describe_keys = not self.opts.allow_keys
        self.host = _host.Host(self.folder, None)
        self.host.watch = LocalWatch()
        self.host.start_services()
        dps = self.settings["dps"]
        limit = self.settings["round_ticks"] if self.sim else max(1, round(self.settings["round_seconds"] * dps))
        self.round_ticks = limit
        spec = self.call("load", project=self.folder, allow_input=bool(self.opts.allow_keys), check=False,
                         decisions_per_second=dps, round_limit=limit, seed=self.opts.seed)
        self.spec = spec
        self.out.warnings(spec)
        verdict, why = _brain.compare(self.brain.shape, spec)
        if verdict == _brain.INCOMPATIBLE:
            raise PlayError("This brain doesn't fit these scripts: " + why,
                            "Use the brain.vrl that came with these scripts, or put back the scripts it was "
                            "trained with.")
        if why:
            self.out.say(why)
        if verdict == _brain.APPENDED:
            self.out.say("The AI will only use the choices it learned; the new ones are never picked.")
        title = file.name or self.settings["name"] or os.path.basename(self.folder)
        self.out.say(f"{title}: {self.brain.describe()}. Press Ctrl+C to stop.")
        if self.opts.view:
            try:
                self.view = View()
            except Exception as err:
                self.out.warn(f"--view needs a display and Tk ({err}), so it is off.")

    def close(self):
        if self.host is not None:
            try:
                self.host.release_keys()
                self.host.shutdown()
            except Exception:
                pass
        if self.server is not None:
            try:
                self.server.stop()
            except Exception:
                pass
        if self.view is not None:
            self.view.close()

    # -- talking to the scripts --

    def call(self, kind, **fields):
        self.ids += 1
        msg = {"t": kind, "id": self.ids}
        msg.update({k: v for k, v in fields.items() if v is not None})
        reply = self.host.handle(msg)
        if reply.get("t") == "error":
            raise describe_error(reply)
        self.out.warnings(reply)
        if self.server is not None:
            for message in reply.get("send") or []:
                self.server.send_custom(message)
        return reply

    def see_obs(self, reply, start):
        (self.brain.start if start else self.brain.push)(reply["obs"])
        if self.view is not None:
            self.view.show(self.brain.seen())

    def count_round(self, points, ticks):
        self.out.round(points, ticks)
        if self.opts.rounds is not None and self.out.rounds >= self.opts.rounds:
            raise Stop()

    # -- pacing --

    def pacer(self, period):
        """sleep() until the next tick on an absolute clock; a run that fell behind doesn't try to catch up."""
        state = {"due": time.perf_counter()}

        def wait():
            if period <= 0:
                return
            now = time.perf_counter()
            if state["due"] > now:
                time.sleep(state["due"] - now)
            state["due"] = max(state["due"] + period, time.perf_counter())
        return wait

    def run(self):
        try:
            if self.settings["link"] is not None and not self.sim:
                self.play_link()
            elif self.sim:
                self.play_sim()
            else:
                self.play_realtime()
        except Stop:
            pass

    # -- simulations --

    def play_sim(self):
        speed = self.opts.speed
        wait = self.pacer(0 if speed <= 0 else 1.0 / (self.settings["dps"] * speed))
        reply = self.call("reset")
        self.see_obs(reply, True)
        points, ticks = 0.0, 0
        while True:
            wait()
            reply = self.call("step", a=self.brain.act(self.epsilon, self.rng))
            points += reply.get("points") or 0.0
            ticks += 1
            ended = reply.get("game_over") or reply.get("truncated")
            self.see_obs(reply, bool(ended))
            if ended:
                done = (points, ticks)
                points, ticks = 0.0, 0
                self.count_round(*done)

    # -- real time (also Vyrn Link) --

    def prepare_keys(self):
        """With --allow-keys and a game that needs keys: the countdown, then lock onto the window in front."""
        uses_keys = bool((self.spec.get("uses") or {}).get("keys"))
        if not self.opts.allow_keys:
            if uses_keys:
                self.out.say("Keys are only described, never pressed. Add --allow-keys to let the AI press them.")
            return
        if not uses_keys:
            return
        for left in range(int(COUNTDOWN_S), 0, -1):
            self.out.say(f"Hands off: click your game window. The AI starts pressing keys in {left}...")
            time.sleep(1.0)
        try:
            reply = self.call("lock_focus")
        except PlayError as err:
            raise PlayError(err.message, (err.hint + "\n" if err.hint else "") + "Without it the AI can't tell whether "
                            "your game is in front, so it won't press keys. Leave out --allow-keys to just watch "
                            "what it would do.") from None
        self.out.say(f"Locked onto the window \"{reply.get('title') or reply.get('app_id') or '?'}\": the AI only "
                     "presses keys while it is in front.")

    def game_attach(self, fields, state, events=True):
        """Vyrn Link: the game's newest state and events ride along with the request (protocol.md section 3.4)."""
        if self.server is None:
            return fields
        fields["connected"] = self.server.connected()
        fields["game"] = state.value if state is not None else {}
        if events:
            fields["game_events"] = [event.value for event in self.server.drain_events()]
        return fields

    def latest(self):
        return self.server.latest_state() if self.server is not None else None

    def restart_round(self, wait):
        """restart(), then check_over() until the game is running again."""
        if self.server is not None and self.settings["link"]["lockstep"]:
            self.server.send_stop()
        self.call("restart", **self.game_attach({}, self.latest()))
        started = since = time.monotonic()
        polled = self.latest()
        fresh = True
        while True:
            wait()
            if self.server is not None:
                if not self.server.connected():
                    return False
                state = self.latest()
                if not fresh and state is polled:
                    continue                      # the game has said nothing new: wait for it
                polled = state
            fresh = False
            now = time.monotonic()
            if now - started >= RESTART_GIVE_UP_S:
                raise PlayError("restart() didn't get the game going again: game_over() stayed True for "
                                f"{RESTART_GIVE_UP_S:g} seconds.", "Check that restart() really starts a new round.")
            if now - since >= RESTART_EVERY_S:
                self.call("restart", **self.game_attach({}, self.latest()))
                since, fresh = now, True
                continue
            if not self.call("check_over", **self.game_attach({}, polled)).get("game_over"):
                return True

    def next_state(self, answered):
        """Vyrn Link: (state to answer, ok). Lockstep waits for the game's next state; otherwise the newest."""
        if self.settings["link"]["lockstep"]:
            kind, state = self.server.wait_state(answered, 0.25)
            return state, kind != "disconnected"
        state = self.latest()
        return state, self.server.connected()

    def wait_for_game(self):
        url = f"ws://{self.address}:{self.server.port}/"
        self.out.say(f"Waiting for the game: connect it to {url} with the pairing code {self.server.code}"
                     + ("" if self.address == "127.0.0.1"
                        else f" (on this computer: ws://127.0.0.1:{self.server.port}/)") + ".")
        while not self.server.wait_connected(0.25):
            self.notices()
        self.notices()
        self.out.say("The game is connected.")

    def notices(self):
        for line in self.server.take_notices():
            self.out.say("Vyrn Link: " + line)

    def play_link(self):
        from .. import _link_server
        link = self.settings["link"]
        code, saved = _link_server.folder_code(self.folder)
        if not saved:
            self.out.warn("Vyrn couldn't save the pairing code in this folder, so it changes every time you start.")
        self.server = _link_server.LinkServer(
            link["port"], code, project=self.settings["name"] or os.path.basename(self.folder),
            buttons=self.spec.get("buttons") or [], lockstep=link["lockstep"],
            frames_per_step=link["frames_per_step"], tick_hz=self.settings["dps"])
        try:
            self.server.start()
        except _link_server.LinkError as err:
            self.server = None
            raise PlayError(str(err)) from None
        addresses = _link_server.lan_addresses()
        self.address = addresses[0] if addresses else "127.0.0.1"
        self.prepare_keys()
        self.rounds_loop()

    def play_realtime(self):
        self.prepare_keys()
        self.rounds_loop()

    def rounds_loop(self):
        link = self.server is not None
        lockstep = link and self.settings["link"]["lockstep"]
        wait = self.pacer(1.0 / self.settings["dps"])
        need_restart = True
        while True:
            if link and not self.server.connected():
                self.wait_for_game()
            if need_restart:
                if not self.restart_round(wait):
                    continue                       # the game left while restarting
            need_restart = True
            first, segment, answered = True, False, 0
            points, ticks = 0.0, 0
            while True:
                if link:
                    state, ok = self.next_state(answered)
                    if not ok:
                        self.host.release_keys()
                        break                      # the game left: wait for it, then restart
                    if state is None:
                        if not lockstep:
                            wait()
                        continue
                    if not lockstep:
                        wait()
                        state = self.latest() or state
                else:
                    wait()
                    state = None
                reply = self.call("observe", first=first, **self.game_attach({}, state))
                if reply.get("focused") is False:     # the game window isn't in front: hold, keys are up
                    self.host.release_keys()
                    segment = False
                    time.sleep(HOLD_POLL_S)
                    continue
                if not first:
                    points += reply.get("points") or 0.0
                    ticks += 1
                    over = reply.get("game_over")
                    if over or ticks >= self.round_ticks:
                        if link and state is not None:
                            self.server.send_stop() if lockstep else self.server.send_input(state.frame, [], [])
                        need_restart = bool(over)
                        self.count_round(points, ticks)
                        break
                first = False
                self.see_obs(reply, not segment)
                segment = True
                act = self.call("act", a=self.brain.act(self.epsilon, self.rng))
                if link and state is not None:
                    answered = state.seq
                    given = act.get("game_input") or {}
                    self.server.send_input(state.frame, given.get("hold") or [], given.get("tap") or [])
                    self.notices()
