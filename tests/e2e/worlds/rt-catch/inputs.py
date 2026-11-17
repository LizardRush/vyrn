# inputs.py — what the AI sees: three numbers read from the running game
from game import GAME


def see():
    fruit_x, fruit_y, basket = GAME.state()
    return {"fruit x": fruit_x, "fruit y": fruit_y, "basket": basket}
