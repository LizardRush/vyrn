# Compile: a trained AI as a standalone Python program (binding)

The person asked: a **Compile** button that turns the trained AI into "a functioning python script that is
independent from the launcher", needing only the `.vrl` and the `vyrn` Python package, "which launches it instead of
the app". So the compiled output runs with plain Python (no Vyrn app, no C++, no torch): the `vyrn` package holds a
numpy-only runtime that loads the `.vrl` and plays.

## 1. Output (one folder, built as `<out>.part/` and renamed into place)

```
<Name>-play/
  play.py                         launcher (from the template, §3)
  brain.vrl                       the trained brain (export_brain; Best by default)
  vyrn.json                       settings, sanitized exactly like a .vrlp (package.md §5)
  inputs.py responses.py points.py [world.py] [helper .py files] [pictures/ ...]
                                  same file allow-list and exclusions as a .vrlp (package.md §5)
  README.txt                      how to run it (from the template)
  vyrn-<version>-py3-none-any.whl the vyrn package, unless "include the vyrn package" is off
```

Built by `vyrn::export_standalone()` in core/src/package (C++: it reuses the .vrlp file rules and export_brain), used by
`vyrn-cli compile <project> [--out DIR] [--brain best|latest] [--no-wheel] [--force] [--json]` and by the app's
**Compile** toolbar button (dialog: Best/Latest, output folder, include the vyrn package; then "Show in folder").
Untrusted projects can't be compiled (same message as Check). Templates come from the data dir:
`<data>/python/vyrn/runtime/templates/{play.py.in,README.txt.in}` with `@NAME@`, `@VERSION@`, `@WHEEL@` replaced.
The wheel comes from `<data>/wheels/vyrn-<version>-py3-none-any.whl` (built at build time, §4).

## 2. The runtime (`vyrn.runtime`, pure Python + numpy; Pillow only where pictures/screens need it)

- `vyrn.runtime.main(folder, argv) -> int` and `python -m vyrn play <folder>`; play.py calls `main`.
- Options: `--brain FILE` (default brain.vrl), `--rounds N` (default: until Ctrl+C), `--speed X` (sims: × the
  project's decisions/s, 0 = as fast as possible; default 1), `--explore E` (default the brain's eps_watch, 0.01),
  `--seed N`, `--allow-keys` (real games: without it keys are described, never pressed), `--view` (a Tk window showing
  what the AI sees; optional), `--quiet`, `--json` (one JSON line per round).
- Loads the scripts **in-process with the same discovery and conversion as the worker** (hooks, choices in file
  order, effects, observation rules, errors with file:line). Reuse `_host`'s functions; don't fork the semantics.
- Reads the `.vrl` with `zipfile` + its own safetensors reader (numpy `frombuffer`, never pickle), checks the
  manifest, the SHA-256 and every tensor name/shape against the net built from the manifest's shape (package.md §3–4).
- Fit check before playing: the scripts' spec (obs kind/size, choices and their order, number names) vs the brain's
  shape, with the same plain-English reasons as `brain::compare` (“This brain doesn't fit these scripts: …”).
- **Exact parity with the C++ brain** (core/src/brain/{nets,preprocess}.cpp): the same three nets (MLP, small CNN,
  DER CNN: layers, padding, strides, ReLU placement, flatten order, /255 inside the picture nets), the same picture
  geometry (native size ≤ 32 px with nearest resize to the saved size; else the same 84×84 area resize), RGB, frame
  stacking (oldest → newest channels, the first frame repeated at a segment start), and number normalization
  (`clip((x-mean)/sqrt(var+1e-8), ±10)` with the saved stats). Greedy action = argmax Q (ties → lowest index), with
  `--explore` chance of a random choice.
- Play loops:
  - **sim** (world.py): restart → per tick see → act → step → points/game_over → round end → restart; paced at
    decisions/s × speed; prints each round's points (and the 20-round mean).
  - **real time** (no world.py): ticks at decisions/s; screen via the platform backends; keys only with
    `--allow-keys`, after a 3 s countdown ("click your game window"), locked to the active window with the platform
    focus guard, held whenever it isn't active; restart()/game_over polling like the app; Ctrl+C or any error releases
    every key first.
  - **Vyrn Link** (vyrn.json "link"): a Python Vyrn Link v1 server (docs/user/vyrn-link.md, frozen; reuse
    tools/link/link_test_server.py's code inside the package as `vyrn._link_server`), pairing code printed in the
    terminal, real time and lockstep, Origin rule and quiet-connection replacement as in the C++ server.
- No training in the runtime (play only). No torch import anywhere in the package.

## 3. play.py (template; the whole file)

```python
#!/usr/bin/env python3
"""@NAME@: a trained Vyrn AI. Run it with:  python3 play.py   (Ctrl+C stops it)

Needs Python 3.10+ and the vyrn package:  pip install @WHEEL@
"""
import pathlib
import sys

try:
    import vyrn.runtime
except ImportError:
    sys.exit("This AI needs the vyrn Python package. Install it with:  pip install @WHEEL@")

sys.exit(vyrn.runtime.main(pathlib.Path(__file__).resolve().parent, sys.argv[1:]))
```

## 4. The vyrn package as a wheel

- `python/pyproject.toml` (name `vyrn`, version = the project version, `requires-python >=3.10`, dependencies
  `numpy`, `Pillow`; optional extra `keys` = `evdev; sys_platform == "linux"`), so `pip install ./python` works.
- `python/tools/make_wheel.py --out-dir DIR [--version V]`: builds `vyrn-<V>-py3-none-any.whl` with the standard
  library only (zip + dist-info METADATA/WHEEL/RECORD with sha256), deterministic, offline. It packs `python/vyrn/`
  (not the worker entry or tools). CMake runs it at build time and installs the wheel to `share/vyrn/wheels/`.

## 5. Tests

- Golden parity: a C++ test writes fixtures (for MLP, small CNN and DER brains: the exported `.vrl`, random inputs,
  preprocessing outputs for pictures of sizes 1×1, 10×10, 32×32, 33×33, 640×480, and Q-values); a pytest loads the
  `.vrl` with `vyrn.runtime` and must match preprocessing exactly (uint8) and Q-values to 1e-4 relative.
- e2e: train catch-numbers and catch-pictures briefly with vyrn-cli, `vyrn-cli compile`, install the wheel into a temp
  dir (PYTHONPATH), run `python3 play.py --rounds 100 --speed 0 --json`: the mean points match vyrn-cli watch's within
  noise. rt-catch (real time, function choices) and link-runner (with tools/link/link_fake_game.py) run a few rounds.
  Keys only with the record backend.
