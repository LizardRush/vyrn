# responses.py — what its choices do: each one sets the game's input for its next step (no keys)
from game import GAME


def left():
    GAME.push(-1)


def right():
    GAME.push(+1)


wait = None
