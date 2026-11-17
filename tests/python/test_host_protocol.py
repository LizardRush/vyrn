"""The worker's side of the protocol rules: broken frames, unknown or misplaced requests, bad choice numbers and
replies too big for one frame (protocol.md sections 2, 3 and 3.7). Every test talks to a real worker."""

import struct

import pytest

from vyrn import _proto

BIG_PICTURE = {
    "inputs.py": """
        import numpy as np

        def see(world):
            return np.zeros((4096, 4096, 3), dtype=np.uint8)
        """,
    "points.py": """
        def points(world):
            return 1

        def game_over(world):
            return True
        """,
}


def assert_protocol_error(error, fatal=True):
    assert error["t"] == "error" and error["kind"] == "protocol" and error["fatal"] is fatal, error
    assert error["message"].strip()


def test_a_broken_frame_gets_a_protocol_error_and_the_worker_exits(spawn, make_project):
    rig = spawn(make_project())
    sock = rig.worker.sock
    sock.settimeout(5)
    sock.sendall(b"XXXX" + struct.pack("<II", 2, 0) + b"{}")
    reply = _proto.recv(sock)
    assert reply["id"] == 0, "an error for a request the worker couldn't read has id 0"
    assert_protocol_error(reply)
    assert rig.worker.wait(2.0) == 2


def test_unknown_requests_are_refused_and_ids_are_echoed(spawn, make_project):
    rig = spawn(make_project())
    error = rig.error("dance")  # tools/engine.py itself refuses a reply whose id isn't the request's
    assert_protocol_error(error)
    status = rig.status()
    assert status["t"] == "ok" and status["id"] == error["id"] + 1


def test_requests_before_load_and_a_second_load(spawn, make_project):
    rig = spawn(make_project())
    assert_protocol_error(rig.error("step", a=0))
    assert_protocol_error(rig.error("reset"))
    assert rig.status()["t"] == "ok", "status runs no user code, so it works before load"
    assert rig.load()["t"] == "spec"
    assert_protocol_error(rig.error("load", project=str(rig.project)))
    assert rig.reset()["t"] == "result", "the first load still stands"


@pytest.mark.parametrize("request_type, fields", [("observe", {"first": True}), ("act", {"a": 0}), ("restart", {}),
                                                  ("check_over", {}), ("lock_focus", {})])
def test_real_time_requests_are_refused_in_a_simulation(spawn, make_project, request_type, fields):
    rig = spawn(make_project())
    rig.load(allow_input=True)
    assert_protocol_error(rig.error(request_type, **fields))
    assert not rig.keys.exists() or rig.keys.actions() == []


@pytest.mark.parametrize("request_type, fields", [("reset", {}), ("step", {"a": 0})])
def test_simulation_requests_are_refused_in_real_time(spawn, make_project, request_type, fields):
    rig = spawn(make_project(sim=False))
    rig.load()
    assert_protocol_error(rig.error(request_type, **fields))


@pytest.mark.parametrize("a", [2, 99, -1, True, 1.0, "0", None])
def test_a_choice_number_that_does_not_exist_is_refused(spawn, make_project, a):
    rig = spawn(make_project())
    rig.load()
    rig.reset()
    assert_protocol_error(rig.error("step", a=a))


def test_close_before_load(spawn, make_project):
    rig = spawn(make_project())
    assert rig.request("close")["t"] == "bye"
    assert rig.worker.wait(2.0) == 0


def test_a_sim_picture_too_big_for_a_round_end_is_refused_at_load(spawn, make_project):
    # A round-end result carries final_obs and the next round's obs in one frame: 2 x 48 MiB > 64 MiB. Refusing
    # at load (where Check sees it) beats failing at the first round end.
    project = make_project(BIG_PICTURE)
    error = spawn(project).load_error()
    assert error["kind"] == "bad_return" and error["fatal"] is True
    assert (error["script"], error["line"], error["func"]) == ("inputs.py", project.line("inputs.py", "def see"),
                                                               "see")
    assert "4096×4096" in error["message"] and "simulation" in error["message"]


def test_a_sim_picture_that_fits_twice_in_a_frame_works(spawn, make_project):
    rig = spawn(make_project(dict(BIG_PICTURE, **{"inputs.py": """
        import numpy as np

        def see(world):
            return np.zeros((2048, 2048, 3), dtype=np.uint8)
        """})))
    assert rig.load()["obs"]["shape"] == [2048, 2048, 3]
    rig.reset()
    result = rig.step(1)  # a round end: final_obs + the next round's obs, 24 MiB
    assert result["game_over"] is True and result["final_obs"].shape == (2048, 2048, 3)


def test_a_real_time_picture_of_the_largest_size_works(spawn, make_project):
    # Real time sends one picture per result, so 4096 x 4096 (48 MiB) fits in a frame.
    rig = spawn(make_project({"inputs.py": """
        import numpy as np

        def see():
            return np.zeros((4096, 4096, 3), dtype=np.uint8)
        """}, sim=False))
    assert rig.load()["obs"]["shape"] == [4096, 4096, 3]
    assert rig.observe(first=True)["obs"].shape == (4096, 4096, 3)


def test_show_pictures_too_big_for_one_frame_are_reported(spawn, make_project):
    rig = spawn(make_project({"inputs.py": """
        import numpy as np
        import vyrn

        def see(world):
            if world.n > 0:
                for number in range(2):  # 2 x 48 MiB: more than one frame holds
                    vyrn.show(f"view {number}", np.full((4096, 4096, 3), number, dtype=np.uint8))
            return [world.n]
        """}))
    rig.load()
    rig.reset()
    error = rig.error("step", a=0)
    assert error["kind"] == "exception" and error["fatal"] is False and "vyrn.show()" in error["message"]
    assert rig.status()["t"] == "ok", "the worker still answers"


def test_too_much_text_for_one_frame_is_reported_plainly(spawn, make_project):
    rig = spawn(make_project({"inputs.py": """
        import vyrn

        def see(world):
            for number in range(60000):
                vyrn.show(f"value {number}", number)
            return [world.n]
        """}))
    rig.load()
    error = rig.error("reset")
    assert error["fatal"] is False and error["kind"] != "protocol"
    assert "vyrn.show()" in f"{error['message']} {error['hint']}"
    assert "picture" not in error["message"]
    assert rig.status()["t"] == "ok"


def test_a_spec_too_big_to_send_is_a_fatal_load_error(spawn, make_project):
    responses = "RESPONSES = {f'choice number {n} ' + 'x' * 40: None for n in range(30000)}\n"
    error = spawn(make_project({"responses.py": responses})).load_error()
    assert error["kind"] != "protocol" and error["message"].strip()
