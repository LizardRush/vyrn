#!/usr/bin/env python3
"""Plays the app side for the parent-death tests (protocol.md section 5, watchdog).

    fake_app.py <project_dir> <choice_index> [<stuck_marker>]

Starts a worker with tools/engine.py (environment inherited from this process), loads the project with input
allowed and a very slow decision rate (so the dead-man timer cannot interfere) and chooses <choice_index> (a held
key). With <stuck_marker>, it then sends "observe" from a background thread and waits until that file exists:
the project's points() creates it and loops forever, so the worker is busy in user code with the key still held.
Finally it prints {"pid": <worker pid>} on one line and sleeps until the test kills this process with SIGKILL.
$FAKE_APP_TRANSPORT=tcp connects the worker the Windows way (TCP + token + control connection) instead of fd 3.
"""

import json
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path[:0] = [os.path.join(REPO, "python"), os.path.join(REPO, "python", "tools")]

import engine  # noqa: E402


def main():
    project, choice = sys.argv[1], int(sys.argv[2])
    marker = sys.argv[3] if len(sys.argv) > 3 else None
    worker = engine.Worker(project, env=dict(os.environ), transport=os.environ.get("FAKE_APP_TRANSPORT") or None)
    worker.load(allow_input=True, decisions_per_second=0.05)  # dead-man timer: max(0.5 s, 60 s)
    worker.request("act", a=choice)
    if marker:
        threading.Thread(target=worker.request, args=("observe",), kwargs={"timeout": 600, "first": False},
                         daemon=True).start()
        deadline = time.monotonic() + 20
        while not os.path.exists(marker) and time.monotonic() < deadline:
            time.sleep(0.01)
    print(json.dumps({"pid": worker.pid}), flush=True)
    while True:
        time.sleep(60)


if __name__ == "__main__":
    main()
