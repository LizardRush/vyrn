# points.py
import math

def points(world):
    return 1                      # one point for every tick the pole stays up

def game_over(world):
    return abs(world.x) > 2.4 or abs(world.angle) > math.radians(12)
