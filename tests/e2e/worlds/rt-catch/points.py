# points.py — how it scores: +1 for a catch, -1 for a miss; then the game shows its game-over screen
from game import GAME


def points():
    return GAME.take_points()


def game_over():
    return GAME.is_over()


def restart():
    GAME.restart()
