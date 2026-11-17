# world.py: a pole standing on a cart. It always wants to tip over; the AI keeps it up by shoving the cart.
import random


def restart(world):
    world.tilt = random.uniform(-0.05, 0.05)   # how far the pole leans (0 = straight up, + = to the right)
    world.spin = 0.0                           # how fast it is tipping
    world.push = 0.0                           # this tick's shove, set by responses.py
    world.fallen = False


def step(world):
    world.spin += 0.12 * world.tilt - 0.05 * world.push   # gravity pulls it over, a shove under it pulls it back
    world.push = 0.0
    world.tilt += world.spin
    world.fallen = abs(world.tilt) > 0.5
