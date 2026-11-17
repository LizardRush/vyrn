# points.py: how the AI is scored each tick, and when a round ends.
def points(world):
    return 1 if abs(world.x) < 0.1 else -0.1   # reward for sitting near the middle


def game_over(world):
    return abs(world.x) > 2.0                  # the dot ran off the line
