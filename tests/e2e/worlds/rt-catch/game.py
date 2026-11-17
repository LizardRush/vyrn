# game.py — a small Catch game that runs by itself in a thread, about 20 steps a second, like a real program.
# There is no world.py, so Vyrn plays it in real time: see() reads the game, the choices set its input.
import random
import threading
import time

STEP_SECONDS = 0.05   # 20 steps a second
WIDTH = 10            # the fruit falls in one of 10 columns...
BOTTOM = 9            # ...and lands on row 9


class Catch:
    def __init__(self):
        self.lock = threading.Lock()
        self.points = 0.0   # earned but not handed to points() yet
        self.new_fruit()
        threading.Thread(target=self.run, name="catch-game", daemon=True).start()

    def new_fruit(self):
        self.basket = 5                     # 1..8; it catches the fruit 1 column left or right too
        self.fruit_x = random.randint(0, WIDTH - 1)
        self.fruit_y = 0
        self.move = 0                       # the input: -1, 0 or +1 for the next step
        self.over = False

    def run(self):
        next_step = time.monotonic()
        while True:
            next_step += STEP_SECONDS
            delay = next_step - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_step = time.monotonic()   # running late: no burst of steps
            with self.lock:
                if self.over:
                    continue                    # the game-over screen waits for restart()
                self.basket = min(8, max(1, self.basket + self.move))
                self.move = 0
                self.fruit_y += 1
                if self.fruit_y >= BOTTOM:
                    self.over = True
                    self.points += 1.0 if abs(self.fruit_x - self.basket) <= 1 else -1.0

    # ---- what the scripts use

    def state(self):
        with self.lock:
            return self.fruit_x, self.fruit_y, self.basket

    def push(self, direction):
        with self.lock:
            self.move = direction

    def take_points(self):
        with self.lock:
            points, self.points = self.points, 0.0
            return points

    def is_over(self):
        # Over once points() has handed out the catch or the miss, so they count for the round that earned them.
        with self.lock:
            return self.over and self.points == 0.0

    def restart(self):
        with self.lock:
            self.new_fruit()


GAME = Catch()
