# world.py — balance a pole on a cart (the classic CartPole-v1 game)
import math
import random

GRAVITY = 9.8            # m/s²
CART_MASS = 1.0          # kg
POLE_MASS = 0.1          # kg
POLE_LENGTH = 0.5        # m, from the hinge to the middle of the pole (half its full length)
SECONDS_PER_TICK = 0.02

def restart(world):
    world.x = random.uniform(-0.05, 0.05)           # cart position (m)
    world.speed = random.uniform(-0.05, 0.05)       # cart speed (m/s)
    world.angle = random.uniform(-0.05, 0.05)       # pole angle (radians, 0 = straight up)
    world.turn_speed = random.uniform(-0.05, 0.05)  # how fast the pole tips over (radians/s)
    world.force = 0.0                               # set by push_left / push_right

def step(world):
    total_mass = CART_MASS + POLE_MASS
    cos = math.cos(world.angle)
    sin = math.sin(world.angle)
    temp = (world.force + POLE_MASS * POLE_LENGTH * world.turn_speed ** 2 * sin) / total_mass
    turn_acceleration = (GRAVITY * sin - cos * temp) / (
        POLE_LENGTH * (4 / 3 - POLE_MASS * cos ** 2 / total_mass))
    acceleration = temp - POLE_MASS * POLE_LENGTH * turn_acceleration * cos / total_mass

    # Move everything forward by one tick (Euler steps, like CartPole-v1)
    world.x += SECONDS_PER_TICK * world.speed
    world.speed += SECONDS_PER_TICK * acceleration
    world.angle += SECONDS_PER_TICK * world.turn_speed
    world.turn_speed += SECONDS_PER_TICK * turn_acceleration
