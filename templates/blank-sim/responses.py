# responses.py: the AI's choices. Each name is one thing it can do (None = do nothing).
def left(world):
    world.move = -1.0


def right(world):
    world.move = 1.0


stay = None
