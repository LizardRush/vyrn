# Share what you trained

Trained an AI you're proud of? Send it to a friend. Vyrn makes two kinds of file:

| File | What's inside | Send it when |
|---|---|---|
| **`.vrlp`**, a Vyrn project | the scripts, pictures and settings, plus the trained AI | your friend should get the whole game and watch your AI play it |
| **`.vrl`**, a trained AI | only the brain: what it learned, what it sees, its choices | your friend already has the same project and wants your AI in it |

Both are ordinary files. Send them by email, chat or USB stick.

## Share a project

1. Open the project and choose **Share ▸ Share project (.vrlp)…**.
2. Add your name if you like (Vyrn never fills it in for you) and a short description.
3. Pick which trained AI goes in: **Best** (its best moment), **Latest**, or **Don't include**.
4. Click **Save…**. Vyrn says something like "Saved Catch.vrlp (412 KB)"; **Show in folder** takes you there.

Vyrn leaves out what only makes sense on your computer: the `brains/` and `runs/` folders (the AI goes in its own
way), hidden files, Python's cache, the `python` setting, and whether coding agents may look at your screen. Your
**Start program** goes along, but only as a hint.

## Share a trained AI

In a project, choose **Brain ▾ → Export this brain (.vrl)…**. The file fits any project whose scripts **see the same
thing** (the same picture size, or the same numbers) and have **the same choices in the same order**.

## Open what someone sends you

- **A project (`.vrlp`)**: double-click it, drag it onto Vyrn, or use **Open shared file…** in the launcher. Vyrn
  first shows you what's inside: the name, who made it, the files, and a little chart of how the AI learned.
  **Import** makes a new project in your Vyrn folder, `~/Vyrn` (on Windows `C:\Users\<you>\Vyrn`). It never
  replaces one of yours.
- **A trained AI (`.vrl`)**: in a project, choose **Brain ▾ → Import a brain (.vrl)…** (or drop the file on the
  window). Vyrn checks that it fits your scripts. If they see something else or have other choices, it tells you
  why and changes nothing. Your old brain stays in `brains/`.

A `.vrl` file is only numbers, so opening one never runs any code.

## Before you trust a project

A `.vrlp` holds Python code someone else wrote, and Python can do anything on your computer. So an imported project
starts **untrusted**. Vyrn shows its scripts but won't run them (no Check, Train or Watch) until you press
**Trust this project**. Read the scripts first. Most are short.

- `inputs.py`, `responses.py`, `points.py` and `world.py` should only describe the game.
- Be careful with lines like these, and ask someone if you can't tell why they're there:
  - `import os`, `import subprocess`, `import shutil`: running programs, deleting or moving files;
  - `open(...)` that writes files, `socket`, `urllib`, `requests`: talking to the internet;
  - `eval(...)`, `exec(...)`, or long scrambled text you can't read.
- A real-game project (no `world.py`) presses keys in the window you choose while it plays.
- The author's Start program is only a hint. Vyrn never starts it by itself. To use it, set it in Settings →
  Start program.

Not sure? Don't trust it. You can still read the scripts, or ask an AI coding agent to explain them. An agent can
open and explain a shared project, but it can't trust it for you.

## From a terminal

```bash
vyrn-cli pack ~/Vyrn/Catch --author "Ada"     # saves Catch.vrlp in this folder
vyrn-cli export-brain ~/Vyrn/Catch            # saves Catch-brain.vrl
vyrn-cli inspect Catch.vrlp                   # shows what's inside; runs nothing
vyrn-cli import Catch.vrlp                    # a new project in ~/Vyrn (Catch-2 if Catch exists)
vyrn-cli trust ~/Vyrn/Catch-2                 # only after reading its scripts
vyrn-cli import Catch-brain.vrl --into ~/Vyrn/Catch
```

On Windows, in PowerShell, write `$HOME\Vyrn\Catch` where it says `~/Vyrn/Catch`, and use the full path of
`vyrn-cli.exe` (see [Vyrn on a Mac or a Windows PC](08-mac-and-windows.md)).

Add `--json` for the details as JSON. Next: back to [your first AI](01-first-ai.md), or
[let a coding agent help](05-ai-agents.md).
