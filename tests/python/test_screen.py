"""Screen capture: grim, the once-per-request union cache, pixel and on_screen (python-internals.md section 9).

Captures come from fakes/fake_grim.py; its call log counts how often "grim" ran.
"""

import os
from pathlib import Path

import numpy as np
import pytest
from PIL import Image
from PIL.PngImagePlugin import PngInfo

import vyrn
from vyrn import _screen, _state


def new_request(state):
    """What the host does at the start of every request."""
    state.generation += 1


def coords(x, y, w, h, blue=7):
    """What fake grim's "coords" pattern shows in a region: red = x, green = y."""
    ys, xs = np.mgrid[y:y + h, x:x + w]
    return np.stack([xs % 256, ys % 256, np.full_like(xs, blue)], axis=-1).astype(np.uint8)


def contains(outer, inner):
    ox, oy, ow, oh = outer
    ix, iy, iw, ih = inner
    return ox <= ix and oy <= iy and ix + iw <= ox + ow and iy + ih <= oy + oh


@pytest.fixture
def screen(state, fakes):
    fakes.grim.set(width=64, height=48, pattern="coords", color=(0, 0, 7))
    new_request(state)
    return _screen.Screen()


# ---- PPM ----------------------------------------------------------------------------------------------------------

def test_parse_ppm():
    picture = _screen.parse_ppm(b"P6\n3 2\n255\n" + bytes(range(18)))
    assert picture.dtype == np.uint8 and picture.shape == (2, 3, 3)
    assert picture.tobytes() == bytes(range(18))


@pytest.mark.parametrize("data", [b"P3\n1 1\n255\n0 0 0\n", b"P6\n1 1\n65535\n" + bytes(6),
                                  b"P6\n2 2\n255\n" + bytes(5), b"", b"not a picture"],
                         ids=["ascii-ppm", "16-bit", "cut-short", "empty", "not-a-picture"])
def test_parse_ppm_refuses_other_data(data):
    with pytest.raises(Exception):
        _screen.parse_ppm(data)


def test_probe(monkeypatch):
    assert _screen.probe() == ("grim", None)


def test_probe_without_a_desktop(monkeypatch):
    monkeypatch.delenv("WAYLAND_DISPLAY")
    monkeypatch.delenv("VYRN_GRIM")
    name, problem = _screen.probe()
    assert name is None and isinstance(problem, str) and problem.strip()


def test_probe_needs_the_desktop_even_with_a_grim_override(monkeypatch):
    # python-internals.md section 9: ("grim", None) only when WAYLAND_DISPLAY is set and grim ($VYRN_GRIM) runs.
    monkeypatch.delenv("WAYLAND_DISPLAY")
    name, problem = _screen.probe()
    assert name is None and "WAYLAND_DISPLAY" in problem


def test_probe_with_a_broken_grim(monkeypatch):
    monkeypatch.setenv("VYRN_GRIM", "/nonexistent/grim")
    name, problem = _screen.probe()
    assert name is None and isinstance(problem, str) and problem.strip()


# ---- capture and crop ---------------------------------------------------------------------------------------------

def test_grab_a_region(screen):
    picture = screen.grab(5, 4, 10, 8)
    assert picture.dtype == np.uint8 and picture.shape == (8, 10, 3)
    np.testing.assert_array_equal(picture, coords(5, 4, 10, 8))


def test_grab_the_whole_screen(screen, fakes):
    picture = screen.grab()
    np.testing.assert_array_equal(picture, coords(0, 0, 64, 48))
    assert screen.size() == (64, 48)
    assert fakes.grim.calls()[-1]["geometry"] is None, "the whole screen is captured without -g"


def test_pixel_returns_plain_ints(screen):
    value = screen.pixel(6, 5)
    assert value == (6, 5, 7)
    assert isinstance(value, tuple) and all(type(channel) is int for channel in value)


def test_one_capture_per_request_with_a_growing_union(screen, state, fakes):
    count = fakes.grim.count

    # First request. (An implementation may capture the whole screen here to learn its size.)
    base = count()
    np.testing.assert_array_equal(screen.grab(5, 4, 10, 8), coords(5, 4, 10, 8))
    np.testing.assert_array_equal(screen.grab(7, 6, 3, 2), coords(7, 6, 3, 2))
    assert screen.pixel(6, 5) == (6, 5, 7)
    assert count() == base + 1, "one capture for the first request"

    new_request(state)
    base = count()
    np.testing.assert_array_equal(screen.grab(5, 4, 10, 8), coords(5, 4, 10, 8))
    assert count() == base + 1
    union = fakes.grim.calls()[-1]["geometry"]
    assert union is not None and contains(union, (5, 4, 10, 8)) and contains(union, (7, 6, 3, 2)), union
    np.testing.assert_array_equal(screen.grab(7, 6, 3, 2), coords(7, 6, 3, 2))
    assert screen.pixel(6, 5) == (6, 5, 7)
    assert count() == base + 1, "areas inside the capture are cropped from it"

    assert screen.pixel(40, 30) == (40, 30, 7)
    assert count() == base + 2, "a new area grows the union and captures again (only this request pays twice)"
    grown = fakes.grim.calls()[-1]["geometry"]
    assert contains(grown, union) and contains(grown, (40, 30, 1, 1)), grown
    np.testing.assert_array_equal(screen.grab(7, 6, 3, 2), coords(7, 6, 3, 2))
    assert count() == base + 2

    new_request(state)
    base = count()
    np.testing.assert_array_equal(screen.grab(20, 15, 4, 4), coords(20, 15, 4, 4))
    assert count() == base + 1, "the next request captures the whole union once"
    assert contains(fakes.grim.calls()[-1]["geometry"], grown)
    assert screen.pixel(40, 30) == (40, 30, 7)
    np.testing.assert_array_equal(screen.grab(5, 4, 10, 8), coords(5, 4, 10, 8))
    assert count() == base + 1

    new_request(state)
    new_request(state)
    assert count() == base + 1, "requests that do not look at the screen capture nothing"
    new_request(state)
    assert screen.pixel(10, 10) == (10, 10, 7)
    assert count() == base + 2


def test_whole_screen_capture_serves_every_area(screen, state, fakes):
    screen.grab()
    after_full = fakes.grim.count()
    np.testing.assert_array_equal(screen.grab(60, 40, 4, 8), coords(60, 40, 4, 8))
    assert screen.pixel(63, 47) == (63, 47, 7)
    assert fakes.grim.count() == after_full
    new_request(state)
    np.testing.assert_array_equal(screen.grab(1, 1, 2, 2), coords(1, 1, 2, 2))
    assert fakes.grim.count() == after_full + 1


def test_new_pictures_each_request(screen, state, fakes):
    fakes.grim.set(color=(10, 20, 30))
    assert screen.pixel(1, 1) == (10, 20, 30)
    fakes.grim.set(color=(40, 50, 60))
    assert screen.pixel(1, 1) == (10, 20, 30), "same request: the cached capture"
    new_request(state)
    assert screen.pixel(1, 1) == (40, 50, 60)


def test_simulations_cannot_use_the_screen(state):
    state.mode = "sim"
    screen = _screen.Screen()
    for call in (lambda: screen.grab(), lambda: screen.grab(0, 0, 2, 2), lambda: screen.pixel(0, 0),
                 lambda: screen.on_screen("x.png", x=0, y=0)):
        with pytest.raises(_state.SimOnlyError) as info:
            call()
        assert info.value.kind == "input"


def test_a_run_without_screen_permission_never_captures(screen, state):
    """load {"screen": false} (an agent's Check the person hasn't allowed to see the screen): every screen function
    refuses with an "input" error, before grim runs."""
    state.screen_allowed = False
    for call in (lambda: screen.grab(), lambda: screen.grab(0, 0, 2, 2), lambda: screen.pixel(0, 0),
                 lambda: screen.on_screen("x.png", x=0, y=0), lambda: screen.size(), lambda: screen._capture(None)):
        with pytest.raises(_state.VyrnUserError) as info:
            call()
        assert info.value.kind == "input"
        assert _screen.NO_SCREEN in info.value.message
        assert "Settings → AI coding agents" in info.value.hint
    assert screen.captures == 0


def test_public_functions_use_the_screen(screen, state):
    state.screen = screen
    np.testing.assert_array_equal(vyrn.screen(5, 4, 10, 8), coords(5, 4, 10, 8))
    np.testing.assert_array_equal(vyrn.screen(x=1, y=2, width=3, height=4), coords(1, 2, 3, 4))
    assert vyrn.pixel(6, 5) == (6, 5, 7)
    assert vyrn.screen().shape == (48, 64, 3)


# ---- on_screen ----------------------------------------------------------------------------------------------------

TARGET = (np.arange(60, dtype=np.uint8).reshape(4, 5, 3) * 4)  # 5 wide, 4 tall, values 0..236


def save_png(path, array, area=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    info = None
    if area is not None:
        info = PngInfo()
        info.add_text("vyrn-area", area)
    Image.fromarray(array).save(path, pnginfo=info)
    return path


@pytest.fixture
def snapshots(state, fakes, tmp_path):
    """A screen with TARGET at (20, 10) and a pictures/ folder with snapshots of it."""
    picture = np.zeros((48, 64, 3), dtype=np.uint8)
    picture[10:14, 20:25] = TARGET
    fakes.grim.set(image=save_png(tmp_path / "screen.png", picture))
    pictures = Path(state.project_dir) / "pictures"
    save_png(pictures / "target.png", TARGET)
    save_png(pictures / "area.png", TARGET, area="20,10,5,4")
    save_png(pictures / "noisy.png", TARGET + 10)
    new_request(state)
    return pictures


def test_on_screen_at_a_position(snapshots):
    screen = _screen.Screen()
    assert screen.on_screen("target.png", x=20, y=10) is True
    assert screen.on_screen("target.png", x=0, y=0) is False
    assert screen.on_screen("target.png", x=21, y=10) is False


def test_on_screen_uses_the_snapshot_area(snapshots):
    screen = _screen.Screen()
    assert screen.on_screen("area.png") is True
    assert screen.on_screen("area.png", x=0, y=0) is False, "x and y win over the saved area"


def test_on_screen_needs_a_position(snapshots):
    with pytest.raises(_state.VyrnUserError):
        _screen.Screen().on_screen("target.png")


def test_on_screen_similarity_threshold(snapshots):
    screen = _screen.Screen()  # mean difference 10 -> similarity 1 - 10/255 = 0.96
    assert screen.on_screen("noisy.png", x=20, y=10) is True
    assert screen.on_screen("noisy.png", x=20, y=10, match=0.95) is True
    assert screen.on_screen("noisy.png", x=20, y=10, match=0.97) is False


def test_missing_snapshot_is_false_with_one_warning(snapshots, state):
    screen = _screen.Screen()
    assert screen.on_screen("game_over.png", x=0, y=0) is False
    assert screen.on_screen("game_over.png", x=0, y=0) is False
    assert len(state.warnings) == 1, state.warnings
    text = str(state.warnings[0])
    assert "game_over.png" in text and "Snapshot" in text


def test_on_screen_notices_a_changed_snapshot(snapshots, state):
    screen = _screen.Screen()
    assert screen.on_screen("target.png", x=20, y=10) is True
    path = snapshots / "target.png"
    save_png(path, 255 - TARGET)
    stat = path.stat()
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns + 5_000_000_000))
    new_request(state)
    assert screen.on_screen("target.png", x=20, y=10) is False


def test_public_on_screen(snapshots, state):
    state.screen = _screen.Screen()
    assert vyrn.on_screen("target.png", 20, 10) is True
    assert vyrn.on_screen("area.png") is True
    assert vyrn.on_screen("noisy.png", x=20, y=10, match=0.97) is False


def test_looking_away_from_where_a_snapshot_was_taken_is_warned_once(snapshots, state):
    # The my-game template's  vyrn.on_screen("game_over.png", x=0, y=0)  overrides a Snapshot's saved position.
    screen = _screen.Screen()
    assert screen.on_screen("area.png", x=0, y=0) is False
    assert screen.on_screen("area.png", x=0, y=0) is False
    assert screen.on_screen("area.png", x=20, y=10) is True, "the saved position itself is fine"
    assert screen.on_screen("target.png", x=0, y=0) is False, "no saved position: nothing to compare"
    assert len(state.warnings) == 1, state.warnings
    message = state.warnings[0]["message"]
    assert "x=20, y=10" in message and "x=0, y=0" in message and "Remove" in message


@pytest.mark.parametrize("call, hint", [("pixel", "world"), ("on_screen", "world"), ("grab", "canvas")])
def test_simulations_get_a_hint_that_fits_the_call(state, call, hint):
    state.mode = "sim"
    with pytest.raises(_state.SimOnlyError) as info:
        getattr(_screen.Screen(), call)(*(["x.png", 1, 2] if call == "on_screen" else [1, 2] if call == "pixel" else []))
    assert hint in info.value.hint
