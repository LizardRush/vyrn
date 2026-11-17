# points.py: how the AI scores.
import vyrn

def points():
    if finished():
        return 100        # reached the end of the level!
    if game_over():
        return -10        # hit a spike
    return 1              # still running: +1 every tick

def game_over():
    return vyrn.game.get("dead", False) or finished()

def finished():
    return any(event.get("name") == "complete" for event in vyrn.events)

def restart():
    vyrn.send("restart")  # the game starts the level again
