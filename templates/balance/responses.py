# responses.py: the AI's choices. Each name is one thing it can do.
def left(world):
    world.push = -1.0


def right(world):
    world.push = 1.0


stay = None
