# points.py
import vyrn

def points():
    if game_over():
        return -10                # lost
    return 1                      # still playing

def game_over():
    # take a Snapshot of your game-over screen
    return vyrn.on_screen("game_over.png", x=0, y=0)

def restart():
    vyrn.press("space")           # the key that starts a new game
