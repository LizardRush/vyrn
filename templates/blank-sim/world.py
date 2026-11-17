# world.py: your game. restart() sets up a new round, step() moves the game forward by one tick.
# Keep everything the game remembers on `world` (world.anything = ...). The other scripts read it too.
import random


def restart(world):
    world.x = random.uniform(-1.0, 1.0)   # placeholder game: a dot on a line, the goal is x = 0
    world.move = 0.0                      # this tick's move, set by responses.py


def step(world):
    world.x += world.move * 0.1
    world.move = 0.0
