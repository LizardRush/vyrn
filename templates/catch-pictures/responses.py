# responses.py
def left(world):
    world.basket = max(1, world.basket - 1)

def right(world):
    world.basket = min(8, world.basket + 1)

stay = None
