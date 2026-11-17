# inputs.py
def see(world):
    return {
        "cart x": world.x,
        "cart speed": world.speed,
        "pole angle": world.angle,
        "pole turn speed": world.turn_speed,
    }
