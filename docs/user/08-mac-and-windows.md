# Vyrn on a Mac or a Windows PC

Vyrn started on the Raspberry Pi. It works the same way on a **Mac with Apple Silicon** (M1 or newer, macOS 13 or
newer) and on a **Windows 10 or 11 PC**. Your projects, scripts and shared files are the same everywhere.

You build Vyrn on the computer that will run it. An install script does it all for you. Plan on about an hour the
first time, mostly downloading (about 2 GB) and compiling. You can safely run a script again: it skips what's
already done.

## On a Mac

1. Open **Terminal** (press ⌘ Space, type *Terminal*, press Return).
2. Install **Homebrew** if you don't have it yet: go to [brew.sh](https://brew.sh), copy the one line shown there
   into Terminal and press Return. It asks for your Mac password.
3. Go to Vyrn's folder (the one with `install-mac.sh` in it) and start the script:

   ```bash
   cd ~/Downloads/vyrn      # wherever Vyrn's folder is
   ./install-mac.sh
   ```

   If Apple's developer tools are missing, a window asks to install them: click **Install**, wait until it's done,
   and run `./install-mac.sh` again. Then the script installs Qt, Python, numpy and Pillow with Homebrew, downloads
   libtorch (the neural network library) into `~/Library/Vyrn/libtorch`, builds Vyrn and puts **Vyrn.app** in the
   **Applications** folder inside your home folder (`~/Applications`).
4. Open Vyrn from **Launchpad**, or with Spotlight (⌘ Space, type *Vyrn*).

Want only one half? `./install-mac.sh --deps` does the downloads, and `./install-mac.sh --app` builds and installs.

The script also puts `vyrn-cli` in `~/.local/bin`. If Terminal says `command not found: vyrn-cli`, run this once and
open a new Terminal window:

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zprofile
```

### Two permissions, for real games

Simulations (projects with a `world.py`) need nothing. To play a **real game**, macOS has to allow Vyrn two things.
Open **System Settings → Privacy & Security**, then:

- **Accessibility**: lets the AI press keys. Click **+**, choose **Vyrn** in your home folder's **Applications**
  folder, and switch it on.
- **Screen Recording** (on macOS 15 it's called **Screen & System Audio Recording**): lets the AI see the game. Add
  Vyrn the same way. macOS may ask you to quit and reopen Vyrn.

Vyrn's setup window checks both and has buttons that open the right page.

After you build Vyrn again, macOS may not recognise the new one, even though the switch still shows as on. Select Vyrn
in both lists, remove it with **−**, and add it again.

If you train a real game with `vyrn-cli` in Terminal instead of the app, **Terminal** needs the two permissions.

## On Windows

1. Install two tools once:
   - **Visual Studio 2022 Build Tools** (the C++ compiler; it also brings CMake and Ninja). Download *Build Tools for
     Visual Studio 2022* from [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)
     and tick **Desktop development with C++**. Or, in PowerShell:

     ```powershell
     winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
     ```

     The Visual Studio installer asks for permission to change your PC; nothing else here does.
   - **Python 3.10 or newer** from [python.org](https://www.python.org/downloads/). Tick **Add python.exe to PATH**
     in the first window of the installer. Or: `winget install Python.Python.3.12`.
2. Open **PowerShell** in Vyrn's folder (in File Explorer, open the folder, then **File → Open Windows PowerShell**,
   or right-click inside the folder → **Open in Terminal**), and run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install-windows.ps1
   ```

   It checks the tools, installs numpy and Pillow, downloads Qt and libtorch and builds QScintilla (the editor part)
   into `%LOCALAPPDATA%\Vyrn`, then builds Vyrn and installs it into `%LOCALAPPDATA%\Programs\Vyrn`.
3. Open Vyrn from the **Start menu** (press the Windows key, type *Vyrn*).

Want only one half? Add `-Deps` (the downloads) or `-App` (build and install) at the end of the command.

Windows needs no permissions for keys or the screen. Double-clicking a `.vrlp` or `.vrl` file opens Vyrn. `vyrn-cli`
is `%LOCALAPPDATA%\Programs\Vyrn\bin\vyrn-cli.exe`.

## Updating

Vyrn checks once a day whether a newer version exists and, if so, shows "A newer version of Vyrn is available" on the home screen. **Update now** opens a terminal that runs the same one-line command from the install page (the build takes 15-30 minutes the first time, less after), then Vyrn reopens. You can also use **Check for updates** on the home screen, or turn the daily check off with "Don't check automatically" (or in a project's Settings). A copy built from a git checkout has no version stamp, so it can't tell.

## What's different

| | Linux (Raspberry Pi) | macOS | Windows |
|---|---|---|---|
| Open Vyrn | menu → Programming → Vyrn | Launchpad or Spotlight | Start menu |
| Panic keys | **Ctrl+Alt+Esc** | **Control+Option+Esc** | **Ctrl+Alt+Esc** |
| Allow keys and screen | `./install.sh --keyboard` | Accessibility + Screen Recording | nothing to do |
| Your projects | `~/Vyrn` | `~/Vyrn` | `C:\Users\<you>\Vyrn` |
| `vyrn-cli` | `~/.local/bin/vyrn-cli` | `~/.local/bin/vyrn-cli` | `%LOCALAPPDATA%\Programs\Vyrn\bin\vyrn-cli.exe` |
| Python | `/usr/bin/python3` | Homebrew's `python3` | `python.exe`, else `py` |

- **The panic keys** stop any AI that's pressing keys, right away. On a Mac and on Windows they're built into Vyrn.
  There's nothing to set up, and they work while Vyrn is open.
- **Grabbing the screen is slower on a Mac.** For a real game, start with about **5 decisions per second**
  (**Settings → Decisions per second**). If Check says Vyrn keeps up, try more.
- **Shared files work everywhere.** A `.vrlp` from a Pi opens on a Mac, and the other way round. A real-game project
  usually needs its screen area picked again (**Pick area**), because screens differ.
- **Start program**: the command that starts your game differs between systems. On Windows, if
  `python3 demo_game.py` doesn't start the demo game, change it to `py demo_game.py` in **Settings → Start program**.
- **Vyrn Link** works the same everywhere. Vyrn and the game can even be on the same Mac or PC.

## When something goes wrong

- **Mac: "Vyrn can't be opened because it is from an unidentified developer"** (or "Apple could not verify Vyrn").
  That happens with a Vyrn.app someone else built. Right-click (or Control-click) Vyrn.app → **Open** → **Open**.
  If there is no Open button (macOS 15): **System Settings → Privacy & Security**, scroll down, **Open Anyway**. A
  Vyrn you built with `install-mac.sh` opens normally.
- **Mac: the AI presses nothing, or Check shows a black picture**: Vyrn is missing **Accessibility** (keys) or
  **Screen Recording** (screen). Add it as shown above (after a rebuild: remove and add it again), then quit and
  reopen Vyrn.
- **Mac: Python, numpy or Pillow is missing**: run `./install-mac.sh --deps` again.
- **Windows: the game ignores the AI's keys**: a game, or its launcher, that runs **as administrator** doesn't take
  keys from normal programs. Start the game normally, not with *Run as administrator*.
- **Windows: "running scripts is disabled on this system"**: use the command exactly as above
  (`powershell -ExecutionPolicy Bypass -File .\install-windows.ps1`).
- **Windows: the script says Visual Studio, CMake or Python is missing**: install what it names (the message says
  how), open a **new** PowerShell window, and run the script again.
- **Windows: the firewall asks about Vyrn**: only Vyrn Link with a game on another computer needs it. Allow
  **Private networks**.
- **A double-clicked `.vrlp` opens something else**: right-click it → **Open with** → choose Vyrn. Or drag it onto
  Vyrn's window, or use **Open shared file…** in the launcher.
- **The build stops with an error**: the Mac and Windows versions are new, so a first build on a new system may need
  a fix. Find the first line with `error` in it, copy it with the lines around it, and ask for help.

Next: [Your first AI in 5 minutes](01-first-ai.md).
