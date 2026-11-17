# world.py — a tiny game Vyrn can run very fast
import random

def restart(world):
    world.basket = 5
    world.fruit_x = random.randint(0, 9)
    world.fruit_y = 0

def step(world):
    world.fruit_y += 1
