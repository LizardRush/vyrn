# Teaching it a real game

Without a `world.py`, your project plays a **real program** on your screen. It looks at the screen and presses keys
a few times a second (10 by default, set in Settings).

## Try the demo game first

1. **New project** → **Demo game**. It comes with a simple "dodge the blocks" game, `demo_game.py`.
2. Press **Train**. Vyrn starts the game for you (its **Start program** in Settings is `python3 demo_game.py`),
   waits a moment for it to open, then checks the scripts. If the game doesn't show up, start it from a
   terminal in the project folder: `python3 demo_game.py` (on Windows: `py demo_game.py`). It fills the screen;
   **Esc** quits it.
3. A 3-second countdown appears: **click the game window** and take your hands off the keyboard.
4. The AI presses keys only while that window is active. Click anywhere else (Vyrn, a terminal) and it pauses
   right away and lets go of every key.
5. **Stop** ends the training and closes the game, but only if Vyrn started it. A game you started yourself stays
   open.

Real-time learning is slow, because the AI only gets 10 tries a second. After 30–60 minutes it should play
noticeably better, and it keeps improving for hours. (On a Mac, grabbing the screen takes longer, so start with 5
decisions a second in Settings.)

## Your own game

1. **New project** → **My game**.
2. Start your game. In Vyrn, click **Pick area** and drag over the game. That fills in the `vyrn.screen(...)` line
   in `inputs.py`.
3. In `responses.py`, write one line per choice with your game's keys (`left = "a"`, `jump = "tap space"`, …).
   Keep a `wait = None`.
4. Make a game-over check:
   - When the game-over screen shows, click **Snapshot** and drag over a part of it that only appears then (for
     example the "GAME OVER" text). Name it `game_over.png`.
   - Vyrn inserts `vyrn.on_screen("game_over.png", x=…, y=…)` for you. Use it in `game_over()`.
5. Fill in `restart()` with the keys that start a new round.
6. Press **Check**. It shows what the AI sees and any problems, and it doesn't press anything.

Tip: put the command that starts your game in **Settings → Start program** (for example `chromium chrome://dino` on
the Pi). Then Vyrn starts it when you press Check, Train or Watch (unless the one it started is still open) and
closes it on **Stop**. It never closes a game you started yourself.

### Ideas for points

- **Staying alive:** `return 1` every tick, and `-10` on game over.
- **Progress:** compare a pixel or a score area with the last tick. `vyrn.pixel` reads colours, and `vyrn.show`
  lets you watch the values while it runs.
- Watch out for rewards the AI can farm by doing something silly, like moving left and right forever.

## Keyboard setup (once)

Pressing keys needs your permission, once. If Check or Train asks for it:

- **Raspberry Pi (Linux)**: run this in a terminal, in Vyrn's folder. It asks for your password.

  ```bash
  ./install.sh --keyboard
  ```

  It also adds the **panic keys**. **Ctrl+Alt+Esc** stops any AI that's pressing keys, instantly.
- **Mac**: in **System Settings → Privacy & Security**, add Vyrn to **Accessibility** (keys) and to **Screen
  Recording** (seeing the game). Vyrn's setup window opens the right page. The panic keys are
  **Control+Option+Esc**, built into Vyrn.
- **Windows**: nothing to set up. The panic keys are **Ctrl+Alt+Esc**, built into Vyrn. A game that runs as
  administrator ignores the AI's keys, so start it normally.

More about the Mac and Windows: [Vyrn on a Mac or a Windows PC](08-mac-and-windows.md).

## Stopping safely

The AI stops pressing keys when you:
- click Vyrn or any other window,
- press **Stop**,
- press the panic keys: **Ctrl+Alt+Esc** (on a Mac **Control+Option+Esc**).

Vyrn's AI keyboard only has normal keys (no Alt or Option, no Windows, ⌘ or power keys), and it lets go of every
key when the AI stops.
