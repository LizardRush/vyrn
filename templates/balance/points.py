# points.py: a point for every tick the pole is still up, and a big loss when it falls.
def points(world):
    return -5 if world.fallen else 1


def game_over(world):
    return world.fallen
