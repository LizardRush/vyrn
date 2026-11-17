# Take your AI out of Vyrn

Your AI learned inside Vyrn, but it doesn't have to stay there. **Compile** turns it into a small folder with a
Python program in it. Give that folder to anyone with Python and they can watch your AI play, without the Vyrn app.

## Make the folder

1. Open your project and click **Compile** in the toolbar.
2. Pick which AI goes in: **Best** (its best moment) or **Latest**. Choose where the folder should go.
3. Leave **Include the vyrn package** ticked unless you're sure the other computer already has it.
4. Click **Compile**, then **Show in folder**.

Prefer the terminal? `vyrn-cli compile <project> --out MyAI-play` does the same.

You get a folder like `Catch-play` with `play.py` (starts the AI), `brain.vrl` (what it learned), your scripts, a
`README.txt` and a file ending in `.whl` (the part of Vyrn it needs to run). Copy the whole folder anywhere.

## Run it

On the other computer you need Python 3.10 or newer. Open a terminal in the folder and type:

```bash
pip install vyrn-0.1.0-py3-none-any.whl    # once
python3 play.py
```

It prints the points of every round, and Ctrl+C stops it. If `pip` complains that the system is "externally
managed", make a private Python first: `python3 -m venv venv`, then use `venv/bin/pip` and `venv/bin/python`.

A few things you can add after `play.py`:

| Add this | What it does |
|---|---|
| `--rounds 20` | play 20 rounds, then stop |
| `--speed 0` | a game inside the computer (a simulation): go as fast as possible |
| `--view` | open a small window that shows what the AI sees |
| `--seed 7` | make its few random moves repeatable |
| `--json` | print one line of data per round, for other programs to read |
| `--allow-keys` | let it press real keys (see below) |

## Real games and keys

If your AI plays a game on the screen with the keyboard, `play.py` starts out **only describing** the keys it would
press. That way nothing surprising happens on someone else's computer. When you do want it to play for real, add
`--allow-keys`: you get a 3 second countdown to click on your game, and the AI only presses keys while that window
is in front. Hands off the keyboard while it plays.

A game connected with Vyrn Link works too: `play.py` prints the address and the pairing code to type into the game,
just like the app does.

## Good to know

- The compiled AI **only plays**. It doesn't learn any more; to keep teaching it, go back to Vyrn.
- It runs with the same scripts you wrote. If you change `inputs.py` or `responses.py` so the AI sees something
  different or has different choices, `play.py` says why the brain doesn't fit instead of playing badly.
- A project you imported and haven't trusted can't be compiled: read its scripts and press **Trust this project**
  first.
- It needs only Python, numpy and Pillow. No torch, no Vyrn app.
