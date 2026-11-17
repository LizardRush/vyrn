# points.py
def points(world):
    if world.fruit_y < 9:
        return 0          # still falling
    if abs(world.fruit_x - world.basket) <= 1:
        return 1          # caught it
    return -1             # missed

def game_over(world):
    return world.fruit_y == 9
