# inputs.py: what the AI sees. Two numbers are enough: how far the pole leans, and how fast it is tipping.
import vyrn


def see(world):
    return {"lean": world.tilt, "tipping": world.spin}
