# Python tests (`tests/python`)

Tests for the Python side of Vyrn: the `vyrn` library, the worker (`python/vyrn_worker.py` + `vyrn/_host.py`) and
the tools. They are written against the contracts in `docs/dev/protocol.md`, `docs/dev/python-internals.md` and
`docs/dev/PLAN.md` section 1, and talk to the implementation only through documented interfaces:

- protocol-level tests start real worker processes on throwaway projects (in pytest's `tmp_path`) through
  `python/tools/engine.py`, the pure-Python "app side";
- unit tests use the module interfaces (`vyrn._proto`, `_effects`, `_keys`, `_screen`, `_focus`, `_canvas`, ...).

## Running

From the repository root:

```bash
python3 -B -m pytest tests/python -q -p no:cacheprovider              # everything
python3 -B -m pytest tests/python -q -p no:cacheprovider -m "not slow"   # skip the few tests that take > 2 s
python3 -B -m pytest tests/python/test_effects.py -q -p no:cacheprovider   # one file
```

Needs Python 3.13 with numpy, Pillow and pytest (`python3-pytest`). `python3-evdev` is not needed: the tests block
it on purpose. Use `-p no:cacheprovider` (as CMake does) to keep `.pytest_cache` out of the repo, and `-B` (or
`PYTHONDONTWRITEBYTECODE=1`) to keep out `tests/python/__pycache__`: pytest caches `conftest.py` before that file
can switch bytecode off. CMake runs the
fast tests as `python_unit` (label `unit`) and the slow ones as `python_slow` (label `slow`).

Tests marked `@pytest.mark.slow` take more than 2 seconds each.

## Safety: nothing touches the real desktop

`conftest.py` sets this up for the test process and for every process a test starts (workers, `check.py`, fakes):

| What | How |
|---|---|
| Keys | `VYRN_KEYS_BACKEND=record:<file>`: key events are appended to a file (`<monotonic> down\|up KEY_X`) instead of going to `/dev/uinput`. On top of that the real `evdev` module cannot be imported (`fakes/evdev_guard/` is first on `PYTHONPATH`, and `sys.modules["evdev"] = None` in the test process), so no test can ever create a real virtual keyboard. |
| Screen | `VYRN_GRIM=<wrapper for fakes/fake_grim.py>` (it selects the grim backend on every system); `WAYLAND_DISPLAY` is a dummy name and `DISPLAY` is unset. Pillow's `ImageGrab` can't be imported in the test process (`sys.modules["PIL.ImageGrab"] = None`): `test_platform.py` gives the ImageGrab backend a fake grab function. |
| Focus | `load`'s `focus_helper` points at a wrapper for `fakes/fake_focus.py` (a named helper is used on every system). |
| macOS / Windows backends | `test_platform.py` runs them on Linux against fakes (user32, CoreGraphics, the objc runtime, kqueue, ImageGrab) and runs Windows' transport for real: `spawn(project, transport="tcp")` / `engine.Worker(..., transport="tcp")`. |
| Pid files | `XDG_RUNTIME_DIR` points into `tmp_path`. |

Nothing here opens a window. If you add a test that does (for example the Tk demo game), run it under
`xvfb-run -a python3 -m pytest ...`.

## The fakes (`fakes/`)

- **`fake_grim.py`**: accepts grim's arguments (`-g "x,y wxh" -t ppm -`) and writes a PPM. The screen comes from the
  JSON file named by `$VYRN_FAKE_GRIM_CONTROL`: size and colour, a `"coords"` pattern (red = x, green = y, handy
  for checking crops), a PNG to use as the screen, rectangles, a delay or a failure. Every run is logged (one JSON
  line with the arguments), which is how the tests count captures per request.
- **`fake_focus.py`**: prints the `vyrn-focus` lines of protocol.md section 6 (`ready`, `active<TAB>id<TAB>app_id<TAB>
  title`, `none`, `closed<TAB>id`, `error<TAB>message`) as the JSON control file named by `$VYRN_FAKE_FOCUS_CONTROL`
  changes (it polls it every 10 ms). It exits on stdin EOF or SIGTERM like the real helper and logs its lifecycle.
- **`fake_app.py`**: plays the app side in its own process for the parent-death test (the test SIGKILLs it and
  checks that the worker releases its keys and exits within a second).
- **`testkit.py`**: shared helpers: `Rig` (a worker started through `engine.Worker` with its own fake devices),
  `KeyLog`, `FakeGrim`, `FakeFocus`, project builders, `wait_until`.

## Writing a new test

```python
def test_something(spawn, make_project):
    project = make_project({"points.py": "def points(world):\n    return world.n * 5\n"})  # a sim
    rig = spawn(project)               # worker + recorded keys + fake grim + fake focus helper
    spec = rig.load()                  # engine.Worker.load(...) -> the "spec" reply
    rig.reset()
    assert rig.step(1)["points"] == 5  # choice 1 is "stay"; the default world.py adds 1 per step


def test_a_bad_return(spawn, make_project):
    rig = spawn(make_project({"points.py": "def points(world):\n    return 'lots'\n"}))
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)     # expects an "error" reply and returns it
    assert (error["kind"], error["line"]) == ("bad_return", 1)
```

`make_project(files, sim=False)` builds a real-time project (no `world.py`). `rig.keys`, `rig.grim` and
`rig.focus` control and inspect the fakes; `rig.project.line("points.py", "def points")` finds line numbers.
