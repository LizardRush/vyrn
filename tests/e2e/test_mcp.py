"""End to end: the Vyrn MCP server for coding agents (docs/dev/mcp.md, section 6 "Tests").

Each test spawns  $VYRN_MCP --projects-dir <temporary folder>  and speaks newline-delimited JSON-RPC with it:
- the legacy flow: initialize, tools/list, create a copy of the catch-numbers template, Check, a broken and a fixed
  points.py, train 1 minute, status shows rounds, stop (the brain is saved), watch, see;
- the modern flow (2026-07-28): server/discover and the same calls with _meta, resultType/ttlMs/cacheScope, -32022
  for a bogus version, and the PNGs of vyrn_check and vyrn_see (decoded with Pillow);
- refusals: paths outside the projects folder (also through a symlink), real-game train/watch/Check, script names,
  settings, the lock of another trainer, protocol errors; notifications/cancelled is harmless;
- shutdown: at most 2 trainings; stdin EOF, SIGTERM and SIGINT stop the trainings, save the brains and free the
  locks (the signals also with the server run as `vyrn-cli mcp`);
- the stop marker: <project>/.vyrn-stop (the app's Stop button) ends one training, which saves; the server goes on;
- sharing: vyrn_share, vyrn_inspect_package, vyrn_import_package; an imported project is read, never run, until the
  person trusts it (no tool does that); a .vrl goes only into a trusted project whose scripts fit it.

Environment: VYRN_MCP = the vyrn-mcp program, VYRN_DATA = the folder with python/ and templates/ (the repo),
VYRN_CLI = the vyrn-cli program (optional: without it the `vyrn-cli mcp` cases skip themselves).
ctest runs it as e2e_mcp (label e2e). By hand:
    VYRN_MCP=build/cli/mcp/vyrn-mcp VYRN_CLI=build/cli/vyrn-cli VYRN_DATA=$PWD /usr/bin/python3 -B -m pytest tests/e2e/test_mcp.py -q -p no:cacheprovider
"""

import base64
import io
import json
import os
import queue
import signal
import subprocess
import threading
import time
from pathlib import Path

import pytest

MODERN = "2026-07-28"
TOOLS = ["vyrn_guide", "vyrn_list_templates", "vyrn_list_projects", "vyrn_create_project", "vyrn_read_project",
         "vyrn_write_script", "vyrn_check", "vyrn_train", "vyrn_status", "vyrn_stop", "vyrn_watch", "vyrn_see",
         "vyrn_settings", "vyrn_share", "vyrn_inspect_package", "vyrn_import_package"]
REPLY_TIMEOUT_S = 120


def program():
    exe = os.environ.get("VYRN_MCP")
    if not exe:
        pytest.skip("set VYRN_MCP (the vyrn-mcp program) and VYRN_DATA (the repo folder) to run the MCP tests")
    assert Path(exe).is_file(), f"VYRN_MCP={exe} is not a file: build vyrn-mcp first"
    return exe


class Server:
    """One vyrn-mcp process (or another `command` that runs the server) and a tiny JSON-RPC client for it."""

    def __init__(self, tmp_path, modern=False, extra=(), command=None):
        self.projects = tmp_path / "projects"
        self.stderr_path = tmp_path / f"stderr-{time.monotonic_ns()}.txt"
        self.modern = modern
        self.next_id = 0
        self.replies = {}
        self.bad_lines = []
        self.lines = queue.Queue()
        self.stderr = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen([*(command or [program()]), "--projects-dir", str(self.projects), *extra],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            self.lines.put(raw)
        self.lines.put(None)

    def send(self, message):
        self.proc.stdin.write((json.dumps(message) + "\n").encode())
        self.proc.stdin.flush()

    def send_raw(self, text):
        self.proc.stdin.write(text.encode() + b"\n")
        self.proc.stdin.flush()

    def meta(self, params):
        params = dict(params or {})
        params["_meta"] = {"io.modelcontextprotocol/protocolVersion": MODERN,
                           "io.modelcontextprotocol/clientInfo": {"name": "test_mcp", "version": "1"},
                           "io.modelcontextprotocol/clientCapabilities": {}}
        return params

    def request(self, method, params=None, modern=None, timeout=REPLY_TIMEOUT_S):
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if modern if modern is not None else self.modern:
            params = self.meta(params)
        if params is not None:
            message["params"] = params
        self.send(message)
        return self.reply(self.next_id, timeout)

    def reply(self, rid, timeout=REPLY_TIMEOUT_S):
        deadline = time.monotonic() + timeout
        while rid not in self.replies:
            left = deadline - time.monotonic()
            assert left > 0, f"no reply to request {rid} within {timeout} s; stderr:\n{self.stderr_text()}"
            try:
                raw = self.lines.get(timeout=left)
            except queue.Empty:
                continue
            assert raw is not None, f"vyrn-mcp closed stdout; stderr:\n{self.stderr_text()}"
            self.take(raw)
        return self.replies.pop(rid)

    def take(self, raw):
        assert raw.endswith(b"\n") and b"\n" not in raw[:-1]
        try:
            message = json.loads(raw)
        except ValueError:
            self.bad_lines.append(raw)
            raise AssertionError(f"stdout carried something that isn't JSON: {raw[:200]!r}")
        if isinstance(message, dict) and message.get("id") is not None:
            self.replies[message["id"]] = message
        else:
            self.replies.setdefault("other", []).append(message)

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)

    def call(self, tool, timeout=REPLY_TIMEOUT_S, modern=None, **arguments):
        reply = self.request("tools/call", {"name": tool, "arguments": arguments}, modern=modern, timeout=timeout)
        assert "result" in reply, reply
        return reply["result"]

    def initialize(self, version="2025-06-18"):
        reply = self.request("initialize", {"protocolVersion": version, "capabilities": {},
                                            "clientInfo": {"name": "test_mcp", "version": "1"}})
        self.notify("notifications/initialized")
        return reply["result"]

    def stderr_text(self):
        if not self.stderr.closed:
            self.stderr.flush()
        return self.stderr_path.read_text(errors="replace")[-3000:]

    def close(self, timeout=90):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise AssertionError(f"vyrn-mcp didn't exit {timeout} s after stdin closed; stderr:\n"
                                     f"{self.stderr_text()}")
        self.stderr.close()
        return self.proc.returncode


@pytest.fixture
def server(tmp_path):
    s = Server(tmp_path)
    yield s
    s.close()


@pytest.fixture
def modern_server(tmp_path):
    s = Server(tmp_path, modern=True)
    yield s
    s.close()


def text(result):
    return result["content"][0]["text"]


def data(result):
    assert "structuredContent" in result, result
    return result["structuredContent"]


def png_images(result):
    from PIL import Image
    images = []
    for item in result["content"]:
        if item["type"] == "image":
            assert item["mimeType"] == "image/png"
            image = Image.open(io.BytesIO(base64.b64decode(item["data"])))
            image.load()
            images.append(image)
    return images


def wait_status(s, project, until, timeout=150):
    deadline = time.monotonic() + timeout
    while True:
        status = data(s.call("vyrn_status", project=project))
        if until(status):
            return status
        assert time.monotonic() < deadline, f"status never got there: {status}"
        assert status["state"] not in ("finished", "error"), status
        time.sleep(1)


def lock_of(path):
    lock = Path(path) / ".vyrn-lock"
    return json.loads(lock.read_text()) if lock.exists() else None


def saved_brains(path):
    return sorted(p.parent.parent.name for p in (Path(path) / "brains").glob("*/latest/*"))


# ---- the legacy flow -----------------------------------------------------------------------------------------

def test_legacy_flow(server):
    init = server.initialize("2025-06-18")
    assert init["protocolVersion"] == "2025-06-18"
    assert init["serverInfo"]["name"] == "vyrn"
    assert "vyrn_guide" in init["instructions"]
    assert "tools" in init["capabilities"] and "resources" in init["capabilities"]
    assert server.request("ping")["result"] == {}

    tools = server.request("tools/list")["result"]["tools"]
    assert [t["name"] for t in tools] == TOOLS
    assert all(t["inputSchema"]["type"] == "object" and t["description"] for t in tools)
    assert "resultType" not in server.request("tools/list")["result"]

    guide = server.call("vyrn_guide")
    assert not guide["isError"] and "points.py" in text(guide) and "responses.py" in text(guide)
    assert "vyrn.show" in text(guide)

    templates = data(server.call("vyrn_list_templates"))["templates"]
    assert {"catch-numbers", "my-game"} <= {t["id"] for t in templates}

    created = server.call("vyrn_create_project", name="Catch numbers", template="catch-numbers")
    assert not created["isError"], text(created)
    project = data(created)
    path = Path(project["path"])
    assert path.parent == server.projects.resolve() and project["mode"] == "sim"
    assert {"inputs.py", "responses.py", "points.py", "world.py"} <= set(project["files"])
    again = server.call("vyrn_create_project", name="Catch numbers", template="catch-numbers")
    assert again["isError"] and "already exists" in text(again)

    listed = data(server.call("vyrn_list_projects"))["projects"]
    assert [p["folder"] for p in listed] == [path.name]

    check = data(server.call("vyrn_check", project=path.name))
    assert check["ok"], check
    assert check["mode"] == "sim"
    assert [c["name"] for c in check["choices"]] == ["left", "right", "stay"]
    assert check["sees"]["kind"] == "numbers" and check["sees"]["names"] == ["fruit x", "fruit y", "basket"]

    good = (path / "points.py").read_text()
    broken = server.call("vyrn_write_script", project=path.name, file="points.py",
                         content="def points(world):\n    return scor\n")
    assert not broken["isError"], text(broken)
    result = data(broken)
    assert result["saved"] and not result["check"]["ok"]
    assert result["check"]["errors"] and result["check"]["errors"][0].startswith("points.py:2"), result
    fixed = data(server.call("vyrn_write_script", project=path.name, file="points.py", content=good))
    assert fixed["check"]["ok"], fixed

    read = data(server.call("vyrn_read_project", project=str(path)))
    assert read["scripts"]["points.py"] == good
    assert list(read["scripts"])[:3] == ["inputs.py", "responses.py", "points.py"]
    assert read["last_check"]["ok"] and read["status"]["state"] == "idle"

    started = server.call("vyrn_train", project=path.name, minutes=1)
    assert not started["isError"], text(started)
    assert data(started)["started"]
    status = wait_status(server, path.name, lambda st: st["rounds_done"] >= 5)
    assert status["state"] == "running" and status["training_by"] == "agent"
    assert status["recent_points"] and status["mean20"] is not None and 0 < status["minutes_left"] <= 1
    lock = lock_of(path)
    assert lock and lock["by"] == "agent" and lock["pid"] == server.proc.pid
    busy = server.call("vyrn_train", project=path.name)
    assert busy["isError"] and "already training" in text(busy)

    # notifications/cancelled for a request that isn't running is harmless
    server.notify("notifications/cancelled", {"requestId": 9999, "reason": "test"})
    assert server.request("ping")["result"] == {}

    stopped = data(server.call("vyrn_stop", project=path.name))
    assert stopped == {"stopped": True, "saved": True}, stopped
    assert lock_of(path) is None
    assert saved_brains(path)
    assert data(server.call("vyrn_status", project=path.name))["state"] == "finished"

    watched = server.call("vyrn_watch", project=path.name, rounds=5, brain="latest")
    assert not watched["isError"], text(watched)
    w = data(watched)
    assert w["rounds"] == 5 and len(w["per_round"]) == 5 and w["mean_points"] is not None

    seen = server.call("vyrn_see", project=path.name)
    assert not seen["isError"], text(seen)
    assert data(seen)["kind"] == "numbers" and len(data(seen)["numbers"]) == 3
    assert [c["name"] for c in data(seen)["choices"]] == ["left", "right", "stay"]


def test_old_versions_get_no_structured_content(server):
    assert server.initialize("2025-03-26")["protocolVersion"] == "2025-03-26"
    result = server.call("vyrn_list_templates")
    assert "structuredContent" not in result
    assert json.loads(text(result).split("\n\n", 1)[1])["templates"]   # the JSON is in the text
    tools = server.request("tools/list")["result"]["tools"]
    assert "title" not in tools[0] and "annotations" in tools[0]
    unknown = server.initialize("1999-01-01")
    assert unknown["protocolVersion"] == "2025-11-25"


# ---- the modern flow -----------------------------------------------------------------------------------------

def check_modern(result, listed=False):
    assert result["resultType"] == "complete"
    assert result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "vyrn"
    if listed:
        assert result["cacheScope"] == "private" and isinstance(result["ttlMs"], int)


def test_modern_flow(modern_server):
    s = modern_server
    discover = s.request("server/discover")["result"]
    check_modern(discover, listed=True)
    assert discover["supportedVersions"][0] == MODERN and "2024-11-05" in discover["supportedVersions"]
    assert discover["serverInfo"]["name"] == "vyrn" and "vyrn_guide" in discover["instructions"]

    tools = s.request("tools/list")["result"]
    check_modern(tools, listed=True)
    assert [t["name"] for t in tools["tools"]] == TOOLS
    check_modern(s.request("ping")["result"])

    created = s.call("vyrn_create_project", name="Catch pictures", template="catch-pictures")
    check_modern(created)
    path = Path(data(created)["path"])

    checked = s.call("vyrn_check", project=path.name)
    check_modern(checked)
    assert data(checked)["ok"] and data(checked)["sees"]["kind"] == "picture"
    images = png_images(checked)
    assert len(images) == 1 and images[0].size[0] >= 10 and images[0].mode == "RGB"

    seen = s.call("vyrn_see", project=path.name)
    assert not seen["isError"], text(seen)
    see_images = png_images(seen)
    assert len(see_images) == 1 and see_images[0].size == images[0].size
    assert data(seen)["source"] == "check" and data(seen)["width"] == 10

    resources = s.request("resources/list")["result"]
    check_modern(resources, listed=True)
    uris = [r["uri"] for r in resources["resources"]]
    assert uris[:2] == ["vyrn://guide", "vyrn://api"]
    script_uri = f"vyrn://projects/{path.name}/points.py"
    assert script_uri in uris
    read = s.request("resources/read", {"uri": script_uri})["result"]
    check_modern(read, listed=True)
    assert read["contents"][0]["text"] == (path / "points.py").read_text()
    assert read["contents"][0]["mimeType"] == "text/x-python"
    api = s.request("resources/read", {"uri": "vyrn://api"})["result"]["contents"][0]
    assert api["mimeType"] == "text/markdown" and "vyrn.show" in api["text"]

    # Train, status, stop and watch with _meta too (a copy of catch-numbers).
    numbers = Path(data(s.call("vyrn_create_project", name="Catch numbers", template="catch-numbers"))["path"])
    started = s.call("vyrn_train", project=numbers.name, minutes=1)
    check_modern(started)
    assert data(started)["started"]
    status = wait_status(s, numbers.name, lambda st: st["rounds_done"] >= 3)
    assert status["training_by"] == "agent"
    stopped = s.call("vyrn_stop", project=numbers.name)
    check_modern(stopped)
    assert data(stopped)["saved"]
    watched = s.call("vyrn_watch", project=numbers.name, rounds=3, brain="latest")
    check_modern(watched)
    assert data(watched)["rounds"] == 3

    bogus = s.request("tools/list", {"_meta": {"io.modelcontextprotocol/protocolVersion": "1999-01-01"}},
                      modern=False)
    assert bogus["error"]["code"] == -32022
    assert bogus["error"]["data"]["requested"] == "1999-01-01" and MODERN in bogus["error"]["data"]["supported"]

    # A legacy request in the same process still works without _meta (no initialize: the newest legacy rules).
    plain = s.request("tools/list", modern=False)["result"]
    assert "resultType" not in plain and len(plain["tools"]) == len(TOOLS)


# ---- refusals ------------------------------------------------------------------------------------------------

def test_refusals(server, tmp_path):
    server.initialize("2025-11-25")
    sim = Path(data(server.call("vyrn_create_project", name="Sim", template="catch-numbers"))["path"])
    real = Path(data(server.call("vyrn_create_project", name="Real", template="my-game"))["path"])

    # Paths: outside the projects folder, also through a symlink.
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "inputs.py").write_text("def see():\n    return 1\n")
    (server.projects / "sneaky").symlink_to(outside, target_is_directory=True)
    for project in ("../outside", str(outside), "/etc", "sneaky", f"{server.projects}/../outside", ".."):
        result = server.call("vyrn_read_project", project=project)
        assert result["isError"], (project, text(result))
    assert "outside" in text(server.call("vyrn_read_project", project="sneaky"))

    # Script names: only *.py directly in the project folder.
    for name in ("../evil.py", "sub/evil.py", ".hidden.py", "notes.txt", "/tmp/evil.py", "..", "evil.py\n"):
        result = server.call("vyrn_write_script", project=sim.name, file=name, content="x = 1\n")
        assert result["isError"], (name, text(result))
    assert not (server.projects / "evil.py").exists() and not (tmp_path / "evil.py").exists()
    too_big = server.call("vyrn_write_script", project=sim.name, file="big.py", content="#" * (100 * 1024 + 1))
    assert too_big["isError"] and "100 KB" in text(too_big)
    (sim / "linked.py").symlink_to(outside / "inputs.py")
    linked = server.call("vyrn_write_script", project=sim.name, file="linked.py", content="x = 1\n")
    assert linked["isError"] and (outside / "inputs.py").read_text().startswith("def see")

    # Real games: never trained or watched; Check only with the user's screen opt-in.
    train = server.call("vyrn_train", project=real.name)
    assert train["isError"] and "presses keys" in text(train) and "Vyrn app" in text(train)
    watch = server.call("vyrn_watch", project=real.name)
    assert watch["isError"] and "presses keys" in text(watch)
    check = server.call("vyrn_check", project=real.name)
    assert check["isError"] and "Settings" in text(check) and "screen" in text(check)
    written = server.call("vyrn_write_script", project=real.name, file="points.py",
                          content="def points():\n    return 1\n")
    assert not written["isError"] and data(written)["saved"] and data(written)["check"] is None
    assert not (real / ".vyrn-lock").exists()

    # Settings: agents.* and unknown keys are refused; good values merge into vyrn.json.
    for values in ({"agents": {"screen": True}}, {"agents.screen": True}, {"python": "/bin/sh"},
                   {"round_ticks": 0}, {"advanced": {"nonsense": 1}}, {"advanced": {"gamma": 2}}):
        result = server.call("vyrn_settings", project=sim.name, values=values)
        assert result["isError"], (values, text(result))
    assert "agents" not in json.loads((sim / "vyrn.json").read_text())
    changed = data(server.call("vyrn_settings", project=sim.name,
                               values={"round_ticks": 200, "advanced": {"explore_ticks": 5000}}))
    on_disk = json.loads((sim / "vyrn.json").read_text())
    assert on_disk["round_ticks"] == 200 and on_disk["advanced"] == {"explore_ticks": 5000}
    assert on_disk["template"] == "catch-numbers" and changed["settings"] == on_disk

    # Another trainer's lock (the app's): no training, no stopping.
    (sim / ".vyrn-lock").write_text(json.dumps({"pid": os.getpid(), "by": "app", "since": "2026-11-13T17:05:00"}))
    blocked = server.call("vyrn_train", project=sim.name)
    assert blocked["isError"] and "Vyrn app is training" in text(blocked)
    assert server.call("vyrn_stop", project=sim.name)["isError"]
    (sim / ".vyrn-lock").unlink()

    # Arguments and protocol errors.
    assert server.call("vyrn_train", project=sim.name, minutes=61)["isError"]
    assert server.call("vyrn_train", project=sim.name, minute=1)["isError"]
    assert server.call("vyrn_watch", project=sim.name, brain="worst")["isError"]
    assert server.request("tools/call", {"name": "vyrn_nope", "arguments": {}})["error"]["code"] == -32602
    assert server.request("tools/call", {"arguments": {}})["error"]["code"] == -32602
    assert server.request("resources/read", {})["error"]["code"] == -32602
    assert server.request("resources/read", {"uri": "vyrn://projects/../etc/passwd"})["error"]["code"] == -32002
    assert server.request("prompts/list")["error"]["code"] == -32601
    server.send_raw("{this is not json")
    server.send({"jsonrpc": "2.0", "id": "after-garbage", "method": "ping"})
    assert server.reply("after-garbage")["result"] == {}
    assert server.replies["other"][0]["error"]["code"] == -32700
    server.notify("notifications/cancelled", {"requestId": "nothing-runs"})
    server.notify("notifications/something_new")
    assert server.request("ping")["result"] == {}


# ---- a script error pauses a training; fixing the script and vyrn_train resume it ---------------------------------

BREAKS_LATER = '''import vyrn

def points(world):
    if vyrn.round > 30:
        return scor
    if world.fruit_y < 9:
        return 0
    return 1 if abs(world.fruit_x - world.basket) <= 1 else -1

def game_over(world):
    return world.fruit_y == 9
'''


def test_script_error_pauses_and_resume(server):
    server.initialize()
    path = Path(data(server.call("vyrn_create_project", name="Fragile", template="catch-numbers"))["path"])
    good = (path / "points.py").read_text()
    assert data(server.call("vyrn_write_script", project=path.name, file="points.py", content=BREAKS_LATER))["check"]["ok"]
    assert not server.call("vyrn_train", project=path.name, minutes=2)["isError"]
    deadline = time.monotonic() + 120
    while True:
        status = data(server.call("vyrn_status", project=path.name))
        if status["state"] == "error":
            break
        assert status["state"] != "finished" and time.monotonic() < deadline, status
        time.sleep(0.5)
    assert "points.py:5" in status["last_error"] and "scor" in status["last_error"], status
    assert lock_of(path)["by"] == "agent"   # paused, still ours
    fixed = data(server.call("vyrn_write_script", project=path.name, file="points.py", content=good))
    assert fixed["check"]["ok"] and "paused by an error" in fixed["training"]
    resumed = data(server.call("vyrn_train", project=path.name))
    assert resumed["resumed"] and resumed["minutes_left"] > 0
    rounds = status["rounds_done"]
    wait_status(server, path.name, lambda st: st["state"] == "running" and st["rounds_done"] >= rounds + 20)
    assert data(server.call("vyrn_stop", project=path.name))["saved"]


# ---- a long call: pings are answered, a cancel ends it without a reply ------------------------------------------

def test_ping_and_cancel_during_a_long_call(server):
    server.initialize()
    path = Path(data(server.call("vyrn_create_project", name="Slow", template="catch-numbers"))["path"])
    with open(path / "world.py", "a") as world:   # the user makes every step slow: Check takes 10 s or more
        world.write("\nimport time\n_step = step\n\ndef step(world):\n    time.sleep(0.5)\n    _step(world)\n")
    server.next_id += 1
    slow_id = server.next_id
    server.send({"jsonrpc": "2.0", "id": slow_id, "method": "tools/call",
                 "params": {"name": "vyrn_check", "arguments": {"project": path.name}}})
    time.sleep(2)
    t0 = time.monotonic()
    assert server.request("ping", timeout=5)["result"] == {}   # answered while the Check runs
    assert time.monotonic() - t0 < 2
    server.notify("notifications/cancelled", {"requestId": slow_id, "reason": "the user changed their mind"})
    t0 = time.monotonic()
    tools = server.request("tools/list", timeout=30)["result"]["tools"]   # waits for the call to end
    assert len(tools) == len(TOOLS) and time.monotonic() - t0 < 8
    time.sleep(0.5)
    while not server.lines.empty():
        server.take(server.lines.get())
    assert slow_id not in server.replies, "a cancelled request got a reply"


# ---- trainings at once, and shutdown -------------------------------------------------------------------------

def test_two_trainings_then_eof_saves_both(server):
    server.initialize("2025-11-25")
    paths = [Path(data(server.call("vyrn_create_project", name=f"Catch {i}", template="catch-numbers"))["path"])
             for i in range(3)]
    for p in paths[:2]:
        assert not server.call("vyrn_train", project=p.name, minutes=5)["isError"]
    third = server.call("vyrn_train", project=paths[2].name)
    assert third["isError"] and "2 trainings" in text(third)
    for p in paths[:2]:
        wait_status(server, p.name, lambda st: st["rounds_done"] >= 3)
        assert lock_of(p)["by"] == "agent"
    code = server.close(timeout=90)   # stdin EOF: stop both, save both
    assert code == 0, server.stderr_text()
    for p in paths[:2]:
        assert lock_of(p) is None
        assert saved_brains(p), f"no brain saved in {p}"


def cli_mcp_command():
    """`vyrn-cli mcp`: the same server inside vyrn-cli (needs VYRN_CLI)."""
    exe = os.environ.get("VYRN_CLI")
    if not exe:
        pytest.skip("set VYRN_CLI (the vyrn-cli program) to run the server as `vyrn-cli mcp` too")
    assert Path(exe).is_file(), f"VYRN_CLI={exe} is not a file: build vyrn-cli first"
    return [exe, "mcp"]


# A thread that a shared library starts before main() doesn't block the stop signals, so the kernel delivers them
# there (the server's own threads block them): the server's handler must catch them there too, not die of them.
@pytest.mark.parametrize("signum", [signal.SIGTERM, signal.SIGINT], ids=lambda s: s.name)
@pytest.mark.parametrize("via", ["vyrn-mcp", "vyrn-cli mcp"])
def test_a_stop_signal_saves_and_exits(tmp_path, via, signum):
    server = Server(tmp_path, command=cli_mcp_command() if via == "vyrn-cli mcp" else None)
    try:
        server.initialize()
        path = Path(data(server.call("vyrn_create_project", name="Catch", template="catch-numbers"))["path"])
        assert not server.call("vyrn_train", project=path.name, minutes=5)["isError"]
        wait_status(server, path.name, lambda st: st["rounds_done"] >= 3)
        server.proc.send_signal(signum)
        assert server.proc.wait(timeout=90) == 128 + signum, server.stderr_text()
        assert lock_of(path) is None
        assert saved_brains(path)
    finally:
        server.close()


# ---- the stop marker (what the app's Stop button does) --------------------------------------------------------

def test_the_stop_marker_ends_a_training_and_the_server_carries_on(server):
    server.initialize()
    path = Path(data(server.call("vyrn_create_project", name="Catch", template="catch-numbers"))["path"])
    assert not server.call("vyrn_train", project=path.name, minutes=5)["isError"]
    wait_status(server, path.name, lambda st: st["rounds_done"] >= 3)
    (path / ".vyrn-stop").write_text("")
    deadline = time.monotonic() + 60
    while (status := data(server.call("vyrn_status", project=path.name)))["state"] != "finished":
        assert time.monotonic() < deadline, status
        time.sleep(0.5)
    assert status["saved"] is True, status
    assert any(".vyrn-stop" in line for line in status["console_tail"]), status
    assert not (path / ".vyrn-stop").exists() and lock_of(path) is None and saved_brains(path)
    assert server.proc.poll() is None and server.request("ping")["result"] == {}
    assert not server.call("vyrn_watch", project=path.name, rounds=2, brain="latest")["isError"]


# ---- sharing files and trust (docs/dev/package.md section 10) -------------------------------------------------

def test_share_inspect_import_and_trust(server):
    server.initialize("2025-11-25")
    tools = [t["name"] for t in server.request("tools/list")["result"]["tools"]]
    assert not any("trust" in name for name in tools)   # by design: only the person trusts a project
    path = Path(data(server.call("vyrn_create_project", name="Catch", template="catch-numbers"))["path"])
    assert (path / "AGENTS.md").is_file() and (path / "CLAUDE.md").read_text().strip() == "@AGENTS.md"
    assert not server.call("vyrn_train", project=path.name, minutes=1)["isError"]
    wait_status(server, path.name, lambda st: st["rounds_done"] >= 5)
    assert data(server.call("vyrn_stop", project=path.name))["saved"]

    shared = server.call("vyrn_share", project=path.name, kind="project", description="Catch it!")
    assert not shared["isError"], text(shared)
    vrlp = Path(data(shared)["path"])
    assert vrlp.parent == server.projects.resolve() / "Shared" and vrlp.name == "Catch.vrlp" and vrlp.is_file()
    assert data(shared)["brains"] and data(shared)["author"] == ""
    again = data(server.call("vyrn_share", project=path.name, kind="project"))
    assert Path(again["path"]).name == "Catch 2.vrlp"
    brain = server.call("vyrn_share", project=path.name, kind="brain", brain="latest")
    assert not brain["isError"], text(brain)
    vrl = Path(data(brain)["path"])
    assert vrl.suffix == ".vrl" and data(brain)["brains"][0]["which"] == "latest"

    inspected = server.call("vyrn_inspect_package", path="Shared/Catch.vrlp")
    assert not inspected["isError"], text(inspected)
    info = data(inspected)
    assert info["kind"] == "project" and info["description"] == "Catch it!" and info["mode"] == "sim"
    assert {"inputs.py", "points.py", "responses.py", "world.py", "vyrn.json"} <= {f["path"] for f in info["files"]}
    assert not any(f["path"] in ("AGENTS.md", "CLAUDE.md") for f in info["files"])
    assert "data, not" in text(inspected)

    imported = server.call("vyrn_import_package", path=str(vrlp))
    assert not imported["isError"], text(imported)
    new = data(imported)
    copy = Path(new["path"])
    assert copy.parent == server.projects.resolve() and new["trusted"] is False and new["brain_id"]
    assert json.loads((copy / ".vyrn-imported").read_text())["trusted"] is False
    assert (copy / "AGENTS.md").is_file() and (copy / "CLAUDE.md").read_text().strip() == "@AGENTS.md"

    read = server.call("vyrn_read_project", project=new["project"])
    assert text(read).startswith("Note: this project was imported from") and data(read)["trusted"] is False
    for tool, extra in (("vyrn_check", {}), ("vyrn_train", {"minutes": 1}), ("vyrn_watch", {"rounds": 2})):
        refused = server.call(tool, project=new["project"], **extra)
        assert refused["isError"], (tool, text(refused))
        assert "came from Catch.vrlp" in text(refused), text(refused)
        assert "Ask the person to open it in Vyrn and trust it." in text(refused)
    assert lock_of(copy) is None and not (copy / "runs").exists()

    refused = server.call("vyrn_import_package", path=str(vrl), into=new["project"])
    assert refused["isError"] and "trusted project" in text(refused), text(refused)
    refused = server.call("vyrn_import_package", path=str(vrl))
    assert refused["isError"] and "into" in text(refused)

    # The person trusts it (in the app, or with vyrn-cli trust in their terminal): now it runs.
    marker = json.loads((copy / ".vyrn-imported").read_text())
    marker["trusted"] = True
    (copy / ".vyrn-imported").write_text(json.dumps(marker))
    assert data(server.call("vyrn_check", project=new["project"]))["ok"]
    watched = server.call("vyrn_watch", project=new["project"], rounds=3)
    assert not watched["isError"], text(watched)

    fresh = Path(data(server.call("vyrn_create_project", name="Fresh", template="catch-numbers"))["path"])
    added = server.call("vyrn_import_package", path=str(vrl), into=fresh.name)
    assert not added["isError"], text(added)
    assert json.loads((fresh / "vyrn.json").read_text())["brain"] == data(added)["brain_id"]
    assert not server.call("vyrn_watch", project=fresh.name, rounds=2)["isError"]
    pictures = Path(data(server.call("vyrn_create_project", name="Pictures", template="catch-pictures"))["path"])
    misfit = server.call("vyrn_import_package", path=str(vrl), into=pictures.name)
    assert misfit["isError"] and "doesn't fit" in text(misfit), text(misfit)
    assert not (pictures / "brains").exists()


def test_a_folder_inside_an_imported_project_is_never_run(server, tmp_path):
    """A .vrlp can hide a runnable "project" in a sub-folder; it belongs to the untrusted import."""
    import hashlib
    import zipfile
    server.initialize("2025-11-25")
    proof = tmp_path / "nested-code-ran.txt"
    template = Path(os.environ.get("VYRN_DATA", ".")) / "templates" / "catch-numbers"
    top = {p.name: p.read_bytes() for p in template.iterdir() if p.is_file() and p.suffix in (".py", ".json")}
    files = dict(top)
    for name, body in top.items():
        files["levels/sub/" + name] = body
    files["levels/sub/inputs.py"] = f"open({str(proof)!r}, 'w').write('ran')\n".encode() + top["inputs.py"]
    files["levels/sub/vyrn.json"] = json.dumps({"name": "Sub", "agents": {"screen": True}}).encode()
    listed = [{"path": p, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()} for p, d in sorted(files.items())]
    package = tmp_path / "Nested.vrlp"
    with zipfile.ZipFile(package, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), b"application/x-vyrn-project")
        z.writestr("vyrn-project.json", json.dumps({"format": "vyrn-project", "format_version": 1, "name": "Nested",
                                                    "files": listed, "brains": []}))
        for path, body in sorted(files.items()):
            z.writestr("project/" + path, body, compress_type=zipfile.ZIP_DEFLATED)

    imported = server.call("vyrn_import_package", path=str(package))
    assert not imported["isError"], text(imported)
    project = Path(data(imported)["path"])
    sub = project / "levels" / "sub"
    assert (sub / "inputs.py").is_file()
    for tool, extra in (("vyrn_check", {}), ("vyrn_train", {"minutes": 1}), ("vyrn_watch", {"rounds": 2})):
        refused = server.call(tool, project=str(sub), **extra)
        assert refused["isError"], (tool, text(refused))
        assert "came from Nested.vrlp" in text(refused) and f"vyrn-cli trust {project})" in text(refused)
    written = server.call("vyrn_write_script", project=str(sub), file="points.py", content=top["points.py"].decode())
    assert not written["isError"] and data(written)["check"] is None, text(written)
    assert not proof.exists() and lock_of(sub) is None and not (sub / "runs").exists()
