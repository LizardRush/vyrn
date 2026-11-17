#!/usr/bin/env python3
"""A stand-in for the vyrn-focus helper (protocol.md section 6), driven by a JSON control file it polls.

The control file is named by $VYRN_FAKE_FOCUS_CONTROL (or by the first argument):

    {"active": {"id": 1, "app_id": "game", "title": "My Game"} or null,
     "closed": [ids],             windows that closed: "closed<TAB>id" is printed once for each
     "error": "message" or null,  print "error<TAB>message" and exit with status 2
     "ready": true,               false: never print "ready" (the initial state is never complete)
     "log": "/path/focus.log"}    lifecycle log for tests: "start <pid>", then "exit <reason>"

On start it prints the current state ("active..." or "none"), then "ready"; after that one line per change it sees
while polling the file every 10 ms. Like the real helper it exits when its stdin reaches EOF or on SIGTERM.
Tabs and newlines inside titles are replaced by spaces.
"""

import json
import os
import select
import signal
import sys
import time

POLL_SECONDS = 0.01
MAX_LIFETIME = 600.0  # never outlive a broken test run by much


def clean(text):
    return str(text).replace("\t", " ").replace("\r", " ").replace("\n", " ")


class Helper:
    def __init__(self, control):
        self.control = control
        self.log_path = None
        self.active = "unknown"  # sentinel: the first sync always prints the state
        self.closed = set()
        self.ready = False

    def read(self):
        try:
            with open(self.control, encoding="utf-8") as file:
                config = json.load(file)
        except (OSError, ValueError):
            return None  # missing, or caught halfway through a write: try again next poll
        self.log_path = config.get("log") or self.log_path
        return config

    def log(self, message):
        if self.log_path:
            with open(self.log_path, "a", encoding="utf-8") as file:
                file.write(f"{message}\n")

    def emit(self, line):
        try:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            self.log("exit stdout-closed")
            os._exit(0)

    def sync(self, config):
        if config.get("error"):
            self.emit("error\t" + clean(config["error"]))
            self.log("exit error")
            sys.exit(2)
        for window in config.get("closed") or []:
            if window not in self.closed:
                self.closed.add(window)
                self.emit(f"closed\t{int(window)}")
        active = config.get("active")
        state = None
        if active:
            state = (int(active["id"]), clean(active.get("app_id", "")), clean(active.get("title", "")))
        if state != self.active:
            self.active = state
            self.emit("none" if state is None else "active\t%d\t%s\t%s" % state)
        if not self.ready and config.get("ready", True):
            self.ready = True
            self.emit("ready")


def main():
    control = os.environ.get("VYRN_FAKE_FOCUS_CONTROL") or (sys.argv[1] if len(sys.argv) > 1 else "")
    helper = Helper(control)

    def on_sigterm(signum, frame):
        helper.log("exit sigterm")
        sys.exit(0)

    signal.signal(signal.SIGTERM, on_sigterm)
    config = helper.read() or {}
    helper.log(f"start {os.getpid()}")
    helper.sync(config)

    started = time.monotonic()
    while time.monotonic() - started < MAX_LIFETIME:
        readable, _, _ = select.select([0], [], [], POLL_SECONDS)
        if readable:
            try:
                data = os.read(0, 4096)
            except OSError:
                data = b""
            if not data:
                helper.log("exit stdin-eof")
                return 0
        config = helper.read()
        if config is not None:
            helper.sync(config)
    helper.log("exit lifetime")
    return 0


if __name__ == "__main__":
    sys.exit(main())
