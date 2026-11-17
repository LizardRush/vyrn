#!/usr/bin/env python3
"""Dodge: the little game the Demo game project teaches an AI to play.

Blocks fall down eight lanes. Hold A or D (or the arrow keys) to slide left or right and stay out
of their way. When a block hits you, the whole window turns red: press Space to play again.
Esc quits.

It is built so an AI can learn it from a small, blurry screenshot: big simple shapes, a bright
player, a gap in every row of blocks, and a red screen when you are hit (points.py checks that).

    python3 demo_game.py                fullscreen (what the Demo game project expects)
    python3 demo_game.py --windowed     in a 640x480 window

Only Python's own tkinter is used, and everything runs on Tk's after() timers (no threads).
"""

import argparse
import random
import signal
import sys
import time
import tkinter as tk

# ---- the rules (sizes are fractions of the window, so any screen size works) ------------------

LANES = 8
MOST_BLOCKED = 5          # at most 5 of the 8 lanes are blocked in a row: there is always a gap
BLOCK_HEIGHT = 0.11       # over 1/10 of the window, so blocks stay visible in the AI's small view
PLAYER_TOP = 0.85
PLAYER_HEIGHT = 0.09
PLAYER_WIDTH = 0.7        # of a lane
STEP_SECONDS = 0.15       # a held key moves the player one lane this often
FIRST_ROW_SECONDS = 1.0   # a short breather before the first row of a new game
SLICE_SECONDS = 0.01      # the rules run in small slices, so a block can never skip past you


# Difficulty grows for about a minute, then stays put. Tuned so a random player (10 decisions per
# second) lasts about 4 s and standing still about 6 s, while a player that looks ahead survives for
# good: even at full speed, the next gap is always reachable before its row arrives.

def fall_speed(seconds):
    """How fast blocks fall (window heights per second) after surviving this many seconds."""
    return min(0.55 + 0.004 * seconds, 0.75)


def row_spacing(seconds):
    """Seconds between two rows of blocks."""
    return max(1.35 - 0.005 * seconds, 1.1)


def most_blocked(seconds):
    """The most lanes one row may block: a few more as the game goes on."""
    return min(3 + int(seconds // 20), MOST_BLOCKED)


class Game:
    """The rules of Dodge, with no drawing (so they are easy to test and to simulate)."""

    def __init__(self, rng=None):
        self.rng = rng or random.Random()
        self.direction = 0              # -1 left, +1 right, 0 still: what the held keys say
        self.restart()

    def restart(self):
        """Start a new game."""
        self.over = False
        self.seconds = 0.0              # time survived: the score
        self.lane = self.rng.choice((3, 4))
        self.rows = []                  # [top, blocked lanes]; top is a fraction of the height
        self.until_row = FIRST_ROW_SECONDS
        self.until_step = STEP_SECONDS

    def steer(self, direction):
        """Hold left (-1), right (+1) or nothing (0). A new direction moves one lane right away."""
        if direction == self.direction:
            return
        self.direction = direction
        if direction and not self.over:
            self.until_step = STEP_SECONDS
            self._move()

    def advance(self, seconds):
        """Run the game forward by this many seconds (nothing happens after a hit)."""
        while seconds > 1e-9 and not self.over:
            dt = min(seconds, SLICE_SECONDS)
            seconds -= dt
            self._tick(dt)

    def _tick(self, dt):
        self.seconds += dt
        speed = fall_speed(self.seconds)
        for row in self.rows:
            row[0] += speed * dt
        self.rows = [row for row in self.rows if row[0] < 1.0]
        self.until_row -= dt
        if self.until_row <= 0:
            count = self.rng.randint(1, most_blocked(self.seconds))
            self.rows.append([-BLOCK_HEIGHT, frozenset(self.rng.sample(range(LANES), count))])
            self.until_row += row_spacing(self.seconds)
        if self.direction:
            self.until_step -= dt
            if self.until_step <= 0:
                self.until_step += STEP_SECONDS
                self._move()
        self._check_hit()

    def _move(self):
        self.lane = min(max(self.lane + self.direction, 0), LANES - 1)
        self._check_hit()

    def _check_hit(self):
        for top, blocked in self.rows:
            level_with_player = top < PLAYER_TOP + PLAYER_HEIGHT and top + BLOCK_HEIGHT > PLAYER_TOP
            if level_with_player and self.lane in blocked:
                self.over = True
                return


# ---- the window -------------------------------------------------------------------------------

FPS = 30
WINDOWED_SIZE = (640, 480)
RELEASE_DELAY_MS = 25     # X11 key auto-repeat sends release+press pairs: wait this long to tell

NAVY = "#0b1633"          # the background while playing (never red)
LANE_LINE = "#16244d"
BLOCK = "#e2e8f0"         # white / light gray blocks
BLOCK_EDGE = "#aab6c8"
PLAYER = "#2ef2b8"        # bright green-cyan
SCORE = "#8ea2c8"
RED = "#dc2626"           # game over fills the window: red > 200 and green < 80 (see points.py)
WHITE = "#ffffff"
FONT = "DejaVu Sans"

LEFT_KEYS = frozenset({"a", "left"})
RIGHT_KEYS = frozenset({"d", "right"})


class App:
    """Draws the game on a Tk canvas and turns held keys into steering."""

    def __init__(self, windowed=False, seconds=None, keylog=None, seed=None):
        self.game = Game(random.Random(seed))
        self.keylog = keylog
        self.held = set()               # direction keys that are down ("a", "left", ...)
        self.pending = {}               # key -> after() job that will finish its release
        self.closed = False
        self.shown_over = None          # whether the screen currently shows game over
        self.shown_score = None
        self.visible_blocks = 0
        self.blocks = []                # rectangles, reused from frame to frame

        self.root = tk.Tk(className="dodge")        # the window's class / app id is "Dodge"
        self.root.title("Dodge - Vyrn demo game")
        self.root.configure(background=NAVY)
        if windowed:
            self.root.geometry("%dx%d" % WINDOWED_SIZE)
        else:
            width, height = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
            self.root.geometry(f"{width}x{height}+0+0")
            self.root.attributes("-fullscreen", True)
        self.width, self.height = WINDOWED_SIZE

        self.canvas = tk.Canvas(self.root, background=NAVY, highlightthickness=0, borderwidth=0,
                                takefocus=1)
        self.canvas.pack(fill="both", expand=True)
        canvas = self.canvas
        self.lane_lines = [canvas.create_line(0, 0, 0, 0, fill=LANE_LINE, width=2) for _ in range(LANES - 1)]
        self.player = canvas.create_rectangle(0, 0, 0, 0, fill=PLAYER, outline="")
        self.score = canvas.create_text(0, 0, anchor="ne", fill=SCORE)
        self.over_title = canvas.create_text(0, 0, fill=WHITE, justify="center", state="hidden",
                                             text="GAME OVER - press space")
        self.over_info = canvas.create_text(0, 0, fill=WHITE, justify="center", state="hidden")

        canvas.bind("<Configure>", self.on_resize)
        self.root.bind_all("<KeyPress>", lambda event: self.on_key(event, True))
        self.root.bind_all("<KeyRelease>", lambda event: self.on_key(event, False))
        self.root.bind("<FocusOut>", self.on_focus_out)
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        for signum in (signal.SIGINT, signal.SIGTERM):      # Ctrl+C or Vyrn's Stop: close tidily
            signal.signal(signum, self.on_signal)
        canvas.focus_set()

        self.stop_job = self.root.after(round(seconds * 1000), self.quit) if seconds else None
        self.clock = self.next_frame = time.monotonic()
        self.frame_job = None
        self.frame()

    def run(self):
        self.root.mainloop()

    def on_signal(self, signum, frame):
        if not self.closed:
            self.root.after_idle(self.quit)

    def quit(self):
        if self.closed:
            return
        self.closed = True
        for job in (self.frame_job, self.stop_job, *self.pending.values()):
            if job is not None:
                self.root.after_cancel(job)
        self.root.destroy()
        if self.keylog:
            self.keylog.close()

    # -- time --

    def catch_up(self):
        """Run the rules up to this moment."""
        now = time.monotonic()
        self.game.advance(now - self.clock)
        self.clock = now

    def frame(self):
        self.catch_up()
        self.draw()
        # Aim for steady frames; after a stall, carry on from now instead of rushing to catch up.
        self.next_frame = max(self.next_frame + 1 / FPS, self.clock)
        delay = round((self.next_frame - time.monotonic()) * 1000)
        self.frame_job = self.root.after(max(1, delay), self.frame)

    # -- keys --

    def on_key(self, event, down):
        if self.closed:
            return
        if self.keylog:
            self.keylog.write(f"{time.monotonic():.6f} {'press' if down else 'release'} "
                              f"{event.keysym} {event.time}\n")
            self.keylog.flush()
        key = event.keysym.lower()
        if key in LEFT_KEYS or key in RIGHT_KEYS:
            if down:
                self.key_down(key)
            else:
                self.key_up(key)
        elif down and key == "space" and self.game.over:
            self.catch_up()
            self.game.restart()
            self.draw()
        elif down and key == "escape":
            self.quit()

    def key_down(self, key):
        job = self.pending.pop(key, None)
        if job is not None:
            self.root.after_cancel(job)     # an auto-repeat pair: the key never really went up
        elif key not in self.held:
            self.held.add(key)
            self.steer()

    def key_up(self, key):
        if key in self.held and key not in self.pending:
            self.pending[key] = self.root.after(RELEASE_DELAY_MS, self.finish_release, key)

    def finish_release(self, key):
        if self.closed:
            return
        self.pending.pop(key, None)
        self.held.discard(key)
        self.steer()

    def on_focus_out(self, event):
        """Another window took the keyboard: it won't tell us about key releases, so let go now."""
        if self.closed:
            return
        for job in self.pending.values():
            self.root.after_cancel(job)
        self.pending.clear()
        if self.held:
            self.held.clear()
            self.steer()

    def steer(self):
        self.catch_up()
        direction = bool(self.held & RIGHT_KEYS) - bool(self.held & LEFT_KEYS)
        self.game.steer(direction)

    # -- drawing --

    def on_resize(self, event):
        self.width, self.height = max(event.width, 1), max(event.height, 1)
        width, height, canvas = self.width, self.height, self.canvas
        for lane, line in enumerate(self.lane_lines, 1):
            x = round(lane * width / LANES)
            canvas.coords(line, x, 0, x, height)
        big = max(12, min(height // 11, width // 18))
        canvas.itemconfigure(self.over_title, font=(FONT, -big, "bold"), width=round(width * 0.9))
        canvas.coords(self.over_title, width / 2, height * 0.42)
        canvas.itemconfigure(self.over_info, font=(FONT, -max(10, big // 2)), width=round(width * 0.9))
        canvas.coords(self.over_info, width / 2, height * 0.42 + big * 1.3)
        canvas.itemconfigure(self.score, font=(FONT, -max(10, height // 28), "bold"))
        canvas.coords(self.score, width - max(8, width // 60), max(6, height // 60))
        self.draw()

    def draw(self):
        game, canvas = self.game, self.canvas
        width, height = self.width, self.height
        lane_width = width / LANES

        if game.over != self.shown_over:
            self.shown_over = game.over
            background = RED if game.over else NAVY
            self.root.configure(background=background)
            canvas.configure(background=background)
            playing = "hidden" if game.over else "normal"
            for item in (*self.lane_lines, self.player):
                canvas.itemconfigure(item, state=playing)
            for item in (self.over_title, self.over_info):
                canvas.itemconfigure(item, state="normal" if game.over else "hidden")
            canvas.itemconfigure(self.score, fill=WHITE if game.over else SCORE)
            if game.over:
                lasted = int(game.seconds)
                canvas.itemconfigure(self.over_info, text=f"You lasted {lasted} second{'' if lasted == 1 else 's'}")

        count = 0
        if not game.over:
            pad = lane_width * 0.06
            for top, blocked in game.rows:
                for lane in blocked:
                    if count == len(self.blocks):
                        item = canvas.create_rectangle(0, 0, 0, 0, fill=BLOCK, outline=BLOCK_EDGE, width=2)
                        canvas.tag_lower(item, self.player)     # under the player and the texts
                        self.blocks.append(item)
                    canvas.coords(self.blocks[count], lane * lane_width + pad, top * height,
                                  (lane + 1) * lane_width - pad, (top + BLOCK_HEIGHT) * height)
                    count += 1
            middle = (game.lane + 0.5) * lane_width
            half = PLAYER_WIDTH * lane_width / 2
            canvas.coords(self.player, middle - half, PLAYER_TOP * height,
                          middle + half, (PLAYER_TOP + PLAYER_HEIGHT) * height)
        for item in self.blocks[self.visible_blocks:count]:
            canvas.itemconfigure(item, state="normal")
        for item in self.blocks[count:self.visible_blocks]:
            canvas.itemconfigure(item, state="hidden")
        self.visible_blocks = count
        score = f"{int(game.seconds)} s"
        if score != self.shown_score:
            self.shown_score = score
            canvas.itemconfigure(self.score, text=score)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Dodge, the Vyrn demo game: hold A/D or the arrow keys to dodge the falling "
                    "blocks. Space plays again after a hit; Esc quits.")
    parser.add_argument("--windowed", action="store_true", help="play in a 640x480 window instead of fullscreen")
    parser.add_argument("--keylog", metavar="PATH",
                        help='append every key press and release to PATH as lines '
                             '"<time.monotonic()> press|release <keysym> <event time>" (for testing)')
    parser.add_argument("--seconds", type=float, metavar="N", help="quit by itself after N seconds (for testing)")
    parser.add_argument("--seed", type=int, help="random seed: the same blocks every time")
    args = parser.parse_args(argv)
    if args.seconds is not None and not args.seconds > 0:
        parser.error("--seconds needs a number above 0")

    keylog = None
    if args.keylog:
        try:
            keylog = open(args.keylog, "a", encoding="utf-8")
        except OSError as err:
            print(f"Dodge can't write the key log: {err}", file=sys.stderr)
            return 1
    try:
        app = App(windowed=args.windowed, seconds=args.seconds, keylog=keylog, seed=args.seed)
    except tk.TclError as err:
        print(f"Dodge can't open its window ({err}). It needs to run on the desktop.", file=sys.stderr)
        return 1
    app.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
