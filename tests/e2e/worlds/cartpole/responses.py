# responses.py — push the cart one way or the other on every tick
def push_left(world):
    world.force = -10.0

def push_right(world):
    world.force = 10.0
