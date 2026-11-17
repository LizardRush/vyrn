# Vyrn

Vyrn teaches a small AI to play games on a Raspberry Pi 5, a Mac or a Windows PC. You describe the game in three
short Python files and Vyrn does the learning.

- `inputs.py`: **what the AI sees** (a picture, such as part of the screen, or a few numbers)
- `responses.py`: **what its choices do** (`left = "a"` holds the A key, `jump = "tap space"` taps Space)
- `points.py`: **how it scores** (a number every tick: +1 for staying alive, −10 for crashing)

Think of Python as the microcontroller and the app as the chip. Your scripts just wire the game to the AI. Vyrn's
C++ core (the chip) builds the neural network (a DQN), plays thousands of rounds and learns which choice earns the
most points. You never touch the network.

A project with a `world.py` is a **simulation**: the game is written in Python and runs inside Vyrn, much faster
than real time. Without one, the AI plays a **real game** on your screen, in real time. It looks at the screen and
presses keys.

Vyrn is a Qt 6 desktop app with an editor, a dashboard and a points chart, plus a command-line tool (`vyrn-cli`)
and an MCP server for AI coding agents. It runs on Raspberry Pi OS (Debian 13) with the labwc Wayland desktop, on
macOS 13 or newer with Apple Silicon, and on Windows 10/11 (x64). You build it on the computer that runs it: an
install script does everything.

## Install on macOS / Windows / Linux

You can safely run each script again: it checks every step and only fixes what's missing. The first run downloads
about 2 GB and compiles for 15–30 minutes. Details and troubleshooting for the Mac and Windows:
[Vyrn on a Mac or a Windows PC](docs/user/08-mac-and-windows.md).

### The quick way: one line

Each of these downloads Vyrn into a temporary folder, builds it, installs the app and deletes the download. (If you've
already cloned the repository, skip to the steps below.)

```bash
# Linux (Raspberry Pi OS): asks for your password once, for the system packages
curl -fsSL https://raw.githubusercontent.com/LizardRush/vyrn/main/get-linux.sh | bash
# macOS (Apple Silicon): needs Homebrew, no password
curl -fsSL https://raw.githubusercontent.com/LizardRush/vyrn/main/get-mac.sh | bash
```

```powershell
# Windows, in PowerShell
irm https://raw.githubusercontent.com/LizardRush/vyrn/main/get-windows.ps1 | iex
```

On Linux the temporary folder is in RAM when you have plenty to spare, and on disk otherwise. Add `VYRN_KEYBOARD=1`
in front of `bash` to also set up key presses for real games.

### Updating

Vyrn checks once a day whether a newer version exists and, if so, shows "A newer version of Vyrn is available" on the home screen. **Update now** opens a terminal that runs the same one-line command as above (the build takes 15-30 minutes the first time, less after), then Vyrn reopens. You can also use **Check for updates** on the home screen, or turn the daily check off with "Don't check automatically" (or in a project's Settings). A copy built from a git checkout has no version stamp, so it can't tell.

### macOS (Apple Silicon)

Install [Homebrew](https://brew.sh) first. Then, in Terminal, from this folder:

```bash
./install-mac.sh --deps   # developer tools check, Homebrew packages (Qt, QScintilla, Python, numpy, Pillow), libtorch
./install-mac.sh --app    # builds Vyrn and installs ~/Applications/Vyrn.app; vyrn-cli goes to ~/.local/bin
```

`./install-mac.sh` with no option does both. No sudo needed. For real games, macOS has to allow Vyrn two things in
**System Settings → Privacy & Security** (simulations need neither):

- **Accessibility**, so the AI can press keys;
- **Screen Recording** (macOS 15: **Screen & System Audio Recording**), so it can see the game.

Click **+**, choose **Vyrn** in `~/Applications`, switch it on. Vyrn's setup window checks both and opens the right
page. After a rebuild, remove Vyrn from both lists (**−**) and add it again.

### Windows 10/11

Install **Visual Studio 2022 Build Tools** with *Desktop development with C++* (it brings CMake and Ninja) and
**Python 3.10 or newer** ([python.org](https://www.python.org/downloads/), tick *Add python.exe to PATH*). Then, in
PowerShell, from this folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Deps   # numpy, Pillow, Qt 6.8, QScintilla, libtorch
powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -App    # builds Vyrn, installs it + Start menu entry
```

With no option it does both. Everything goes into `%LOCALAPPDATA%\Vyrn` (libraries) and
`%LOCALAPPDATA%\Programs\Vyrn` (Vyrn itself); the script needs no administrator rights. Windows asks no permissions
for keys or the screen, but a game that runs as administrator ignores Vyrn's keys.

### Linux (Raspberry Pi OS)

From this folder, in a terminal:

```bash
./install.sh --deps       # system packages (asks for your password once, sudo)
./install.sh --app        # builds Vyrn and installs it to ~/.local: Vyrn appears in the menu (Programming)
./install.sh --keyboard   # only for real games: lets Vyrn press keys, adds the panic keys (sudo)
```

`./install.sh` with no option does all three. After `--keyboard` you may need to log out and back in once (it says
so).

### Then

Open **Vyrn** (Linux: the menu, Programming; macOS: Launchpad or Spotlight; Windows: the Start menu), click **New
project**, pick **Demo: Catch (pictures)** and press **Train**. Within a few minutes the AI usually catches almost
every fruit. (Want a different first game? **Demo: Balance** is a pole on a cart that the AI has to keep upright.) [Your first AI in 5 minutes](docs/user/01-first-ai.md) walks you through it.

## Templates

| Template | What it is |
|---|---|
| **Blank simulation** | An empty simulated game: write your own in `world.py` (starts as a tiny placeholder that already trains) |
| **My game** | Any game on your screen: pick its area and map keys to choices |
| **Demo: Balance** | A simulation: keep a pole upright on a cart (learns in a minute or two) |
| **Demo: Catch (pictures)** | Your first AI: catch falling fruit from a 10×10 picture (a simulation; learns in minutes) |
| **Demo: Catch (numbers)** | The same game, but the AI sees three numbers instead of a picture |
| **Demo: Demo game** | A simple "dodge the blocks" game for trying real-game training, no edits needed |

## The command line

`vyrn-cli` (in `~/.local/bin` on Linux and macOS; on Windows `%LOCALAPPDATA%\Programs\Vyrn\bin\vyrn-cli.exe`) does
everything the app does, without a window:

```bash
vyrn-cli check <project>                     # load the scripts and play a few random steps
vyrn-cli train <project> --minutes 10        # train (Ctrl+C stops and saves the brain)
vyrn-cli watch <project>                     # play the trained brain without learning
vyrn-cli bench                               # time the neural network on this computer
vyrn-cli mcp                                 # the MCP server for AI coding agents (stdin/stdout)
vyrn-cli pack <project>                      # save a project as a .vrlp file
vyrn-cli export-brain <project>              # save a trained brain as a .vrl file
vyrn-cli inspect <file.vrl|file.vrlp>        # show what a shared file holds (runs nothing)
vyrn-cli import <file.vrlp>                  # open a shared project (it starts untrusted)
vyrn-cli import <file.vrl> --into <project>  # add a shared brain to a project
vyrn-cli trust <project>                     # let Vyrn run an imported project's Python (read it first)
```

`vyrn-cli --help` lists every option. Real games need `--allow-input` to press keys, and simulations never press
any. Without the app at all, `python3 python/tools/check.py <project>` runs a Check too.

## Coding agents

Vyrn includes an MCP server, so Claude Code, Codex or Gemini CLI can create projects, write the scripts, run Check,
train and watch simulations, and see what the AI sees. Connect one with a single line (the **Connect an AI agent**
button in Vyrn's start window shows these with Copy buttons):

```bash
claude mcp add --scope user vyrn -- ~/.local/bin/vyrn-cli mcp
codex mcp add vyrn -- ~/.local/bin/vyrn-cli mcp
gemini mcp add vyrn ~/.local/bin/vyrn-cli mcp
```

On Windows (PowerShell), use the full path instead:
`claude mcp add --scope user vyrn -- "$env:LOCALAPPDATA\Programs\Vyrn\bin\vyrn-cli.exe" mcp` (the same for codex
and gemini).

Agents never train or watch a real game, and never press keys: you press Train yourself. A Check that looks at the
screen needs your permission per project (Settings → AI coding agents). Every template has an `AGENTS.md` with notes
for agents. More: [Let a coding agent help](docs/user/05-ai-agents.md).

## Sharing

- A **`.vrlp`** file is a whole project (scripts, pictures, settings and the trained AI): **Share ▸ Share project
  (.vrlp)…**.
- A **`.vrl`** file is only a trained brain, for a project with the same inputs and choices: **Brain ▾ → Export
  this brain (.vrl)…**.

Double-click a received file to open it. A `.vrl` is only numbers and never runs code. A `.vrlp` holds someone
else's Python, so it opens **untrusted**: Vyrn shows the scripts but won't run them until you read them and press
**Trust this project**. More: [Share what you trained](docs/user/06-sharing.md).

**Compile** (toolbar, or `vyrn-cli compile`) turns a trained AI into a folder that plays with plain Python, no Vyrn app needed: [Take your AI out of Vyrn](docs/user/09-compile.md).

## Vyrn Link and Geometry Dash

Some games can tell Vyrn exactly what's happening. With **Vyrn Link**, a game (or a mod inside it) connects to Vyrn
over a WebSocket (port 7457 by default, with a 6-digit pairing code for other computers), sends its numbers and
gets the AI's buttons back. Nothing is read from the screen and no keys are pressed, so Geometry Dash can run on your
PC while Vyrn learns on the Pi. You can try it first with the fake runner game in `tests/fixtures/link-runner`. The
Geometry Dash mod (with Geode) is yours to write.

- [Geometry Dash with Vyrn Link](docs/user/07-geometry-dash.md)
- [The Vyrn Link protocol, for mod writers](docs/user/vyrn-link.md); `tools/link/link_test_server.py` stands in for
  Vyrn while you write one.

## Safety with real games

When a real game trains, the AI presses keys on your desktop. Vyrn keeps that under control:

- The AI presses keys only in the game window you clicked during the countdown. **Click Vyrn** (or any other
  window) and it holds at once and releases every key; click the game to let it continue.
- The **panic keys** stop any AI that is pressing keys, right away: **Ctrl+Alt+Esc** on Linux (from
  `./install.sh --keyboard`) and Windows, **Control+Option+Esc** on macOS (built into Vyrn there, nothing to set up).
- **Stop** in Vyrn, closing Vyrn, or Python crashing also releases every key.
- Vyrn's virtual keyboard only has ordinary keys: no Alt, Super, Caps Lock or power keys.

More: [Teaching it a real game](docs/user/03-real-games.md).

## First real-game test

A checklist for the first time you train a real game (about an hour). It needs `./install.sh --keyboard` first on
Linux, and the two permissions on macOS.

1. **Demo game, zero edits.** **New project** → **Demo game** → **Create**, then **Train**. Vyrn starts the game
   (its Start program, `python3 demo_game.py`), checks the scripts and asks "The AI will press keys". If the game
   hasn't appeared by itself, start it now from a terminal: `cd ~/Vyrn/Demo-game && python3 demo_game.py` (the
   folder New project showed; on Windows `py demo_game.py`; the game fills the screen and Esc quits it). Go back to
   Vyrn (Alt+Tab; ⌘ Tab on a Mac), click **Start training**, and during the 3-second countdown click the game. The
   AI starts sliding left and right.
2. **Click-away hold.** Click Vyrn's window: the AI holds and releases every key ("Keys released: you clicked
   Vyrn"). Click the game: it goes on. Switch to a terminal: it holds, and nothing is typed into the terminal.
3. **Panic key.** While it trains, press the panic keys (**Ctrl+Alt+Esc**; **Control+Option+Esc** on a Mac):
   Python stops in under half a second and Vyrn says "Panic stop".
4. **Kill test.** Train again. In a terminal, run `sleep 5; pkill -9 -f vyrn_worker` and click the game within the
   5 seconds (Windows, in PowerShell: `Start-Sleep 5; Get-CimInstance Win32_Process -Filter "CommandLine like
   '%vyrn_worker%'" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }`). After the kill, no key stays down
   (the player stops sliding, nothing repeats). On Linux, `grep -c "Vyrn AI keyboard" /proc/bus/input/devices`
   also prints `0`.
5. **It learns.** Train for 30–60 minutes. The average line in the **Points** chart should end up clearly higher
   than at the start: roughly twice as long a survival time as random play. That is a rough guide; every run is a
   little different.

## Building and testing

The install scripts build into `build/`. By hand:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j3
ctest --test-dir build -L unit    # C++ and Python unit tests
ctest --test-dir build -L gui     # the app, offscreen
ctest --test-dir build -L e2e     # end to end: learning, sharing, the MCP server, Vyrn Link
ctest --test-dir build -L slow    # long runs, such as real-time learning
```

On macOS and Windows, run the install script's `--deps` / `-Deps` step first: CMake then finds libtorch in
`~/Library/Vyrn/libtorch` or `%LOCALAPPDATA%\Vyrn\libtorch` by itself (or pass `-DTorch_DIR=<libtorch>/share/cmake/Torch`).
Qt needs `-DCMAKE_PREFIX_PATH`: `$(brew --prefix qt)` on a Mac, `%LOCALAPPDATA%\Vyrn\Qt\6.8.3\msvc2022_64` on Windows
(where the build runs in an *x64 Native Tools* prompt, and the tests need libtorch's `lib` and Qt's `bin` and `lib`
folders on PATH). There, CMake downloads nlohmann_json, GoogleTest and zlib when they aren't installed
(`VYRN_FETCH_DEPS`, pinned versions checked by SHA-256); Linux uses the system packages. The Python unit tests need
`pytest` (macOS: `python3 -m pip install --user --break-system-packages pytest`; Windows: `py -m pip install pytest`).

**CI:** [.github/workflows/ci.yml](.github/workflows/ci.yml) builds Vyrn and runs the unit tests on Ubuntu 24.04,
macOS 14 (Apple Silicon) and Windows Server 2022, with the install scripts on the Mac and Windows. It has **not run
yet**: it was written before the repository was on GitHub, so expect to fix a few things on its first run. Until
then, the Mac and Windows builds are checked only by compiling them there (docs/dev/platforms.md, section 4).

## Documentation

For people using Vyrn (`docs/user/`):

1. [Your first AI in 5 minutes](docs/user/01-first-ai.md)
2. [The three files](docs/user/02-three-files.md)
3. [Teaching it a real game](docs/user/03-real-games.md)
4. [Why isn't it learning?](docs/user/04-not-learning.md)
5. [Let a coding agent help](docs/user/05-ai-agents.md)
6. [Share what you trained](docs/user/06-sharing.md)
7. [Geometry Dash with Vyrn Link](docs/user/07-geometry-dash.md), and [the Vyrn Link protocol](docs/user/vyrn-link.md)
8. [Vyrn on a Mac or a Windows PC](docs/user/08-mac-and-windows.md)

For developers (`docs/dev/`):

- [PLAN.md](docs/dev/PLAN.md): the design, milestones and test plan
- [platforms.md](docs/dev/platforms.md): Linux, macOS and Windows: what differs and where the code for each lives
- [protocol.md](docs/dev/protocol.md): the app ↔ Python worker protocol (the binding contract)
- [python-internals.md](docs/dev/python-internals.md), [core-internals.md](docs/dev/core-internals.md),
  [app-internals.md](docs/dev/app-internals.md): how each part is built
- [mcp.md](docs/dev/mcp.md): the MCP server and its safety rules
- [package.md](docs/dev/package.md): `.vrl`/`.vrlp` files, importing and trust
- [link.md](docs/dev/link.md): Vyrn Link inside Vyrn
- [bench.md](docs/dev/bench.md): neural network speed on the Pi 5
