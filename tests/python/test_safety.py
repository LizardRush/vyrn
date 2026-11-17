"""Held keys are always released, within a second: pause, close, errors, SIGTERM, the SIGUSR1 freeze, the dead-man
timer and the death of the app (protocol.md section 5). Keys are recorded to a file, never pressed."""

import json
import select
import signal
import subprocess
import sys
import time

import pytest

from testkit import FAKES, Background, Fakes, build_project, kill_worker_group, proc_gone, wait_until

PLAYER = {
    "inputs.py": """
        def see():
            return [1.0, 2.0]
        """,
    "responses.py": """
        left = "a"
        right = "shift+d"
        jump = "tap space"
        wait = None
        """,
    "points.py": """
        import vyrn

        def points():
            return 1

        def restart():
            vyrn.press("enter")
        """,
}
LEFT, RIGHT, JUMP, WAIT = range(4)


@pytest.fixture
def player(spawn, make_project):
    """A real-time worker allowed to press (recorded) keys. Its decision rate is so slow that the dead-man timer
    (max(0.5 s, 3 / rate) = 60 s) cannot release keys behind the back of the release path under test."""

    def start(files=None, **load):
        rig = spawn(make_project(dict(PLAYER, **(files or {})), sim=False))
        rig.load(**dict({"allow_input": True, "decisions_per_second": 0.05}, **load))
        return rig

    return start


def hold_left(rig):
    rig.act(LEFT)
    assert rig.keys.held() == {"KEY_A"}
    return rig.keys.events()[-1].t


def send_only(rig, t, **fields):
    """Send a request like the app does, without waiting for (or reading) its reply. Returns its id."""
    worker = rig.worker
    worker._next_id += 1
    worker._send(dict({"t": t, "id": worker._next_id}, **fields))
    return worker._next_id


def test_pause_releases_every_key(player):
    rig = player()
    rig.act(RIGHT)
    assert rig.request("pause")["t"] == "ok"
    assert rig.keys.actions(2) == [("up", "KEY_D"), ("up", "KEY_LEFTSHIFT")], "released by the time pause replies"
    assert rig.status()["held"] == []


def test_pause_cancels_a_pending_tap(player):
    rig = player()
    rig.act(JUMP)
    rig.request("pause")
    time.sleep(0.15)
    assert rig.keys.actions() == [("down", "KEY_SPACE"), ("up", "KEY_SPACE")]


def test_close_releases_keys_and_exits_cleanly(player):
    rig = player()
    rig.act(RIGHT)
    assert rig.worker.close() == 0
    assert rig.keys.held() == set()
    rig.wait_gone(1.0)


@pytest.mark.parametrize("body, kind", [("raise ValueError('boom')", "exception"), ("return 'lots'", "bad_return")])
def test_errors_release_keys_before_they_are_reported(player, body, kind):
    rig = player({"points.py": f"def points():\n    {body}\n"})
    rig.observe(first=True)
    rig.act(RIGHT)
    error = rig.error("observe")
    assert error["kind"] == kind and error["fatal"] is False
    assert rig.keys.held() == set(), "keys are released before the error is sent"


def test_sigterm_releases_keys_and_exits(player):
    rig = player()
    hold_left(rig)
    sent = time.monotonic()
    rig.signal(signal.SIGTERM)
    rig.wait_gone(1.0)
    events = rig.keys.events()
    assert rig.keys.held(events) == set()
    assert events[-1].t - sent < 1.0


def test_sigusr1_freezes_until_resume(player):
    rig = player()
    hold_left(rig)
    sent = time.monotonic()
    rig.signal(signal.SIGUSR1)
    events = rig.keys.wait_all_up(1.0)
    assert events[-1].t - sent < 1.0
    status = rig.status()
    assert status["frozen"] is True and status["held"] == []

    mark = rig.keys.mark()
    assert rig.act(RIGHT)["t"] == "ok", "key actions are skipped silently while frozen"
    assert rig.request("restart")["t"] == "ok"
    time.sleep(0.1)
    assert rig.keys.actions(mark) == []

    assert rig.request("resume")["t"] == "ok"
    assert rig.status()["frozen"] is False
    rig.act(RIGHT)
    assert rig.keys.actions(mark) == [("down", "KEY_LEFTSHIFT"), ("down", "KEY_D")]


STUCK_POINTS = """
    import time

    def points():
        open({marker!r}, "w").close()
        while True: time.sleep(0.01)
    """


def test_sigusr1_works_while_user_code_is_stuck_and_sigint_reports_the_line(player, tmp_path):
    marker = tmp_path / "stuck"
    rig = player({"points.py": STUCK_POINTS.format(marker=str(marker))})
    rig.observe(first=True)
    hold_left(rig)
    stuck = Background(rig.request, "observe", timeout=20)  # points() never returns, with A held
    wait_until(marker.exists, 5.0, "points() to start looping")

    sent = time.monotonic()
    rig.signal(signal.SIGUSR1)
    events = rig.keys.wait_all_up(1.0)
    assert events[-1].t - sent < 1.0
    assert stuck.running, "the keys were released while user code was still stuck"

    rig.signal(signal.SIGINT)
    stuck.join(5)
    error = getattr(stuck.exc, "error", None)
    assert error is not None, f"expected an error reply, got {stuck.result!r} / {stuck.exc!r}"
    assert error["kind"] == "timeout" and error["func"] == "points"
    assert (error["script"], error["line"]) == ("points.py", rig.project.line("points.py", "while True"))
    assert "too long" in error["message"]


def test_dead_man_timer_releases_keys(player):
    rig = player(decisions_per_second=10)  # timeout max(0.5 s, 3 / 10 s) = 0.5 s
    pressed = hold_left(rig)
    events = rig.keys.wait_all_up(2.0)
    assert 0.4 <= events[-1].t - pressed <= 1.3, events[-1].t - pressed
    assert rig.status()["held"] == []
    rig.act(LEFT)
    assert rig.keys.held() == {"KEY_A"}, "the worker keeps working after the timer fired"


def test_decisions_keep_the_dead_man_timer_quiet(player):
    rig = player(decisions_per_second=10)
    rig.observe(first=True)
    hold_left(rig)
    for _ in range(8):
        time.sleep(0.15)
        rig.observe()
        rig.act(LEFT)
    assert rig.keys.held() == {"KEY_A"} and rig.keys.actions() == [("down", "KEY_A")]
    rig.request("pause")
    assert rig.keys.held() == set()


def test_status_polls_do_not_keep_keys_down(player):
    # The app polls "status" while it holds, when the keys must be up anyway: polls don't feed the dead-man timer.
    rig = player(decisions_per_second=10)  # timeout max(0.5 s, 3 / 10 s) = 0.5 s
    pressed = hold_left(rig)
    deadline = time.monotonic() + 2.0
    while rig.keys.held() and time.monotonic() < deadline:
        rig.status()
        time.sleep(0.1)
    events = rig.keys.events()
    assert rig.keys.held(events) == set(), "released although status was polled all the time"
    assert events[-1].t - pressed <= 1.3


@pytest.mark.slow
def test_dead_man_timer_follows_the_decision_rate(player):
    rig = player(decisions_per_second=2)  # timeout max(0.5 s, 3 / 2 s) = 1.5 s
    pressed = hold_left(rig)
    time.sleep(1.0)
    assert rig.keys.held() == {"KEY_A"}
    events = rig.keys.wait_all_up(2.0)
    assert 1.4 <= events[-1].t - pressed <= 2.3


@pytest.mark.parametrize("request_type", ["restart", "check_over"])
def test_between_rounds_nothing_is_held(player, request_type):
    """restart and check_over run between rounds (restart polling): held keys are released first, so they can't
    stay down for the 30 s the app allows restart() (nor while user code there holds the GIL)."""
    rig = player()
    hold_left(rig)
    reply = rig.request(request_type)
    assert reply["t"] == "ok"
    events = rig.keys.wait_all_up(1.0)
    assert ("up", "KEY_A") in [(e.action, e.code) for e in events]
    if request_type == "restart":
        actions = rig.keys.actions()
        assert actions.index(("up", "KEY_A")) < actions.index(("down", "KEY_ENTER")), "released before restart()"


def test_keys_are_released_when_a_round_ends(player):
    rig = player({"points.py": """
        import vyrn

        def points():
            return 1

        def game_over():
            return vyrn.tick >= 2
        """})
    rig.observe(first=True)
    hold_left(rig)
    assert rig.observe()["game_over"] is False and rig.keys.held() == {"KEY_A"}
    assert rig.observe()["game_over"] is True
    assert rig.keys.held() == set(), "released before the game-over reply"
    rig.act(LEFT)
    rig.observe(first=True)  # a new round (e.g. after the round limit) starts with nothing held
    assert rig.keys.held() == set()


def test_nothing_is_pressed_after_close_and_the_worker_exits(player):
    """A thread the user's scripts started keeps pressing keys: after "bye" nothing is pressed (no new virtual
    keyboard either) and the worker exits instead of waiting for that thread forever."""
    rig = player({"inputs.py": """
        import threading
        import time
        import vyrn

        def _keep_pressing():
            while True:
                try:
                    vyrn.press("x")
                except Exception:
                    pass
                time.sleep(0.02)

        _started = []

        def see():
            if not _started:
                _started.append(threading.Thread(target=_keep_pressing))  # not a daemon thread
                _started[0].start()
            return [1.0]
        """})
    rig.keys.wait_count(4)  # it presses while the worker runs
    worker = rig.worker
    reply_id = send_only(rig, "close")
    assert worker._read(2.0, expect_id=reply_id, interrupt=False, types=("bye",))["t"] == "bye"
    bye = time.monotonic()
    assert worker.wait(1.0) == 0, "the worker exits although a thread of the scripts still runs"
    time.sleep(0.2)
    assert [e for e in rig.keys.events() if e.t > bye] == [], "nothing is pressed after bye"
    assert rig.keys.held() == set()


@pytest.mark.parametrize("thread", [False, True], ids=["normal-exit", "thread-still-running"])
def test_a_log_the_scripts_leave_open_is_complete_after_close(player, tmp_path, thread):
    log = tmp_path / "log.csv"
    rig = player({"points.py": f"""
        import threading
        import time

        LOG = open({str(log)!r}, "w")         # never closed or flushed by the script
        if {thread}:
            threading.Thread(target=lambda: time.sleep(600)).start()

        def points():
            LOG.write("tick\\n")
            return 1
        """})
    rig.observe(first=True)
    for _ in range(3):
        rig.observe()
    assert rig.worker.close(timeout=2.0) == 0
    assert log.read_text() == "tick\n" * 3


def test_an_error_message_that_hangs_cannot_keep_keys_down(player, tmp_path):
    """Building an error reply may run the user's own code (an exception's __str__): keys are released first, and
    the app's SIGINT can stop it like any other stuck user code."""
    marker = tmp_path / "describing"
    rig = player({"points.py": f"""
        import time

        class Slow(Exception):
            def __str__(self):
                open({str(marker)!r}, "w").close()
                time.sleep(30)
                return "never"

        def points():
            raise Slow()
        """})
    rig.observe(first=True)
    hold_left(rig)
    stuck = Background(rig.request, "observe", timeout=20)
    wait_until(marker.exists, 5.0, "the exception's __str__ to start")
    assert rig.keys.held() == set(), "released before the error was described"
    rig.signal(signal.SIGINT)
    stuck.join(5)
    error = getattr(stuck.exc, "error", None)
    assert error is not None and error["kind"] == "timeout", (stuck.result, stuck.exc)
    assert (error["script"], error["line"]) == ("points.py", rig.project.line("points.py", "time.sleep(30)"))


@pytest.mark.parametrize("value", ["false", 0, 1, "yes", [True]])
def test_allow_input_must_be_a_real_boolean(spawn, make_project, value):
    rig = spawn(make_project(PLAYER, sim=False))
    error = rig.load_error(allow_input=value)
    assert error["kind"] == "protocol"
    assert not rig.keys.exists() or rig.keys.actions() == []


@pytest.mark.parametrize("value", ["false", 0, 1, [False]])
def test_screen_must_be_a_real_boolean(spawn, make_project, value):
    rig = spawn(make_project(PLAYER, sim=False))
    assert rig.load_error(screen=value)["kind"] == "protocol"


def test_a_load_without_screen_permission_refuses_the_screen(spawn, make_project):
    """An agent's Check without the person's permission: a project that reads the screen gets an "input" error at
    load, before any capture; one that doesn't (a Vyrn Link game, say) loads as usual."""
    looks = spawn(make_project({"inputs.py": "import vyrn\n\ndef see():\n    return vyrn.screen(0, 0, 4, 4)\n",
                                "responses.py": "left = None\nright = None\n",
                                "points.py": "def points():\n    return 0\n"}, sim=False))
    error = looks.load_error(check=True, screen=False)
    assert error["kind"] == "input" and error["fatal"]
    assert "can't read the screen in this run" in error["message"]
    assert (error["script"], error["line"]) == ("inputs.py", 4)
    assert looks.grim.count() == 0

    blind = spawn(make_project({"inputs.py": "import vyrn\n\ndef see():\n    return vyrn.game.get('y', 0)\n",
                                "responses.py": "jump = 'game jump'\nwait = None\n",
                                "points.py": "def points():\n    return 1\n"}, sim=False))
    spec = blind.load(check=True, screen=False)
    assert spec["mode"] == "realtime" and spec["uses"]["screen"] is False


def test_allow_input_null_means_no(spawn, make_project):
    rig = spawn(make_project(PLAYER, sim=False))
    rig.load(allow_input=None)
    assert rig.error("act", a=LEFT)["kind"] == "input"


@pytest.mark.slow
def test_a_gil_holding_call_cannot_keep_keys_down_forever(spawn, make_project, tmp_path):
    """User code stuck in one long C call keeps the GIL, so the watchdog can't run: when the app is gone too, a C-level
    timer (faulthandler) ends the worker after 2 x max(1 s, 5 ticks) + 5 s, and the kernel drops its keys."""
    marker = tmp_path / "stuck"
    rig = spawn(make_project(dict(PLAYER, **{"points.py": f"""
        import re

        def points():
            open({str(marker)!r}, "w").close()
            return len(re.findall(r"(a+)+$", "a" * 64 + "!"))   # exponential backtracking, all inside one C call
        """}), sim=False))
    rig.load(allow_input=True, decisions_per_second=10)  # the stuck-exit timer: 2 x 1 s + 5 s = 7 s
    rig.observe(first=True)
    hold_left(rig)
    send_only(rig, "observe", first=False)  # nobody reads the reply: the app is about to vanish
    wait_until(marker.exists, 5.0, "points() to get stuck")
    stuck = time.monotonic()
    rig.worker.sock.close()  # the app is gone; the watchdog would notice, but it can't get the GIL
    wait_until(rig.gone, 12.0, "the worker to end itself")
    assert 6.0 <= time.monotonic() - stuck <= 11.0
    # faulthandler prints which line was stuck (the engine's reader thread may still be draining the pipe)
    wait_until(lambda: "Timeout" in rig.worker.stderr_text(), 2.0, "faulthandler's report on stderr")


@pytest.mark.parametrize("transport", ["fd", "tcp"])
@pytest.mark.parametrize("busy", [False, True], ids=["idle", "stuck-in-user-code"])
def test_app_death_releases_keys_and_ends_the_worker(tmp_path, busy, transport):
    """SIGKILL the app side. Idle, the worker's request loop sees EOF; stuck in user code, only the watchdog can
    notice (POLLRDHUP on fd 3, or EOF on Windows' control connection: transport "tcp"). Either way every key is up and
    the worker is gone within a second."""
    pytest.importorskip("engine")
    fakes = Fakes(tmp_path / "devices")
    marker = tmp_path / "stuck"
    files = dict(PLAYER)
    if busy:
        files["points.py"] = STUCK_POINTS.format(marker=str(marker))
    project = build_project(tmp_path / "project", files, sim=False)
    command = [sys.executable, str(FAKES / "fake_app.py"), str(project), str(LEFT)] + ([str(marker)] if busy else [])
    app = subprocess.Popen(command, env=fakes.env({"FAKE_APP_TRANSPORT": transport}), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
    worker_pid = None
    try:
        ready, _, _ = select.select([app.stdout], [], [], 20)
        assert ready, "the fake app did not start a worker"
        line = app.stdout.readline()
        assert line, app.stderr.read().decode(errors="replace")
        worker_pid = json.loads(line)["pid"]
        assert fakes.keys.held() == {"KEY_A"}
        assert marker.exists() == busy

        killed = time.monotonic()
        app.kill()  # SIGKILL: the app side vanishes without closing anything
        app.wait(5)
        wait_until(lambda: proc_gone(worker_pid), 1.0, "the worker to notice the app died")
        events = fakes.keys.events()
        assert fakes.keys.held(events) == set(), "the worker released every key"
        assert events[-1].t - killed < 1.0
    finally:
        if app.poll() is None:
            app.kill()
            app.wait(5)
        if worker_pid is not None:
            kill_worker_group(worker_pid)
