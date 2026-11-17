# The three files

Every Vyrn project has three Python files. If the game itself is written in Python, there's a fourth.

## inputs.py: what the AI sees

```python
def see():
    return ...
```

`see()` returns either a **picture** or **numbers**:

- **A picture**: the screen (`vyrn.screen(x=0, y=0, width=640, height=480)`), a drawing (`vyrn.canvas(10, 10)`), a
  numpy array of whole numbers 0–255, or a Pillow image.
- **Numbers**: `0.5`, a list like `[x, y, speed]`, or a dict like `{"x": x, "speed": speed}`. The names show up in
  the dashboard.

Vyrn shrinks pictures to an 84×84 colour image (small pictures keep their size) and remembers the last 4, so the AI
can see movement.

## responses.py: what its choices do

Each name you write at the top of `responses.py` is one **choice** the AI can make:

```python
left = "a"            # hold the A key while the AI keeps choosing "left"
right = "d"
jump = "tap space"    # press and release Space once
run_jump = "d+space"  # hold D and Space together
wait = None           # do nothing (always give the AI a way to do nothing)

def turbo(world):     # a function: called each time the AI chooses "turbo"
    world.speed = 2
```

Key names: letters, digits, `space`, `enter`, `tab`, `esc`, `backspace`, arrows (`left`, `right`, `up`, `down`),
`shift`, `ctrl`, `f1`–`f12`, and punctuation. Vyrn won't press keys that could mess with your system (Alt or Option,
the Windows or ⌘ Command key, Caps Lock, power keys). If you misspell one, Check suggests the right name.

Names starting with `_` aren't choices, so use them for helpers: `_speed = 3`.

## points.py: how it scores

```python
import vyrn

def points():
    return 1                    # +1 for every tick it stays alive

def game_over():                # optional: True ends the round
    return vyrn.on_screen("game_over.png")

def restart():                  # optional: how to start a new round
    vyrn.press("space")
```

- `points()` returns a number. If you forget to return one, it counts as 0.
- Big numbers for big things (a catch, finishing a level), small numbers for small progress.
- `game_over()` returning `None` or `False` means "keep going".
- `restart()` is for real games. Vyrn calls it after every game over and waits until `game_over()` is False again.

## world.py: a game written in Python (optional)

If your project has a `world.py`, the game runs inside Vyrn and trains very fast. Any function that takes one
input gets a shared `world` object, where you keep the game's state:

```python
def restart(world):        # a new round
    world.x = 0

def step(world):           # one tick, after the AI's choice
    world.x += 1
```

`see(world)`, `points(world)` and your choice functions get that same `world`.

## The vyrn library

| | |
|---|---|
| `vyrn.screen(x, y, width, height)` | A picture of part of the screen (no arguments = the whole screen) |
| `vyrn.pixel(x, y)` | The colour `(r, g, b)` of one pixel |
| `vyrn.on_screen("name.png", x, y)` | Is that Snapshot showing at that spot? |
| `vyrn.press("space")` | Tap a key right now |
| `vyrn.wait(seconds)` | Wait |
| `vyrn.canvas(width, height)` | A blank picture: `.dot(x, y, color)` and `.box(x, y, w, h, color)` |
| `vyrn.show(name, value)` | Show a number, text or picture on the dashboard |
| `vyrn.choice`, `vyrn.tick`, `vyrn.round` | The last choice, the tick in this round, the round number |
| `vyrn.game`, `vyrn.events`, `vyrn.connected`, `vyrn.send(...)` | For games connected with Vyrn Link: see [Geometry Dash](07-geometry-dash.md) |

Press **F1** on any `vyrn.` name in the editor for its full help.
