# Geometry Dash with Vyrn Link

Some games can be modded. A mod can tell Vyrn exactly what's going on in the game and press the buttons the AI
picks. That's **Vyrn Link**. The game connects to Vyrn over your home network and sends its numbers: where the
player is, how fast it falls, what's coming up. Vyrn sends the AI's buttons back. Nothing is read from the screen
and no keys are pressed. So Geometry Dash can run on your gaming PC while Vyrn learns on another computer, like a
Raspberry Pi. They can also share one Mac or Windows PC.

You'll need:

- Geometry Dash with [Geode](https://geode-sdk.org) on your PC, plus **your own Vyrn mod**. You have to write it
  yourself. [The Vyrn Link protocol](vyrn-link.md) lists everything it must send and do.
- If Vyrn runs on another computer (the Pi, say), both computers on the same network.

Vyrn has no Geometry Dash project built in. You make a normal project and write its three files for your game's
numbers, the way the link-runner example does.

## Try it without a game first

Vyrn comes with a tiny fake game that speaks Vyrn Link, so you can watch the whole thing work on one computer. The
project for it is the folder `tests/fixtures/link-runner` (it isn't in **New project**).

1. Copy that folder into your Vyrn folder (`~/Vyrn`) and open it from the launcher.
2. Open a terminal in Vyrn's folder and start the fake game. It keeps retrying until Vyrn is listening:

   ```bash
   python3 tools/link/link_fake_game.py ws://127.0.0.1:7457/ --reconnect
   ```

   On Windows, write `py` instead of `python3`. In an installed Vyrn the file is in `share/vyrn/tools/link/`: on
   the Pi `~/.local/share/vyrn/tools/link/`, on a Mac `~/Applications/Vyrn.app/Contents/Resources/vyrn/tools/link/`,
   on Windows `%LOCALAPPDATA%\Programs\Vyrn\share\vyrn\tools\link\`.
3. Press **Train**. The fake game connects within a couple of seconds and the AI starts jumping over spikes. Give
   it 10 to 15 minutes. It should clear more and more spikes and now and then reach the end of the level. Every run
   is a bit different, so take that as a rough guide.

## With a real game

1. **Install your mod.** Put the `.geode` file in Geode's mods folder and start Geometry Dash.
2. In Vyrn, open your Vyrn Link project (a copy of the link-runner folder is a good start).
3. **Address and pairing code.** The blue **Vyrn Link** bar above the editor shows both, like
   `ws://192.168.1.20:7457/` and `483920`. Type them into your mod's settings. **Copy address** puts the address on
   the clipboard. Settings → **Vyrn Link** shows them too.
4. Press **Train**. Vyrn says **Waiting for the game** until your mod connects, then the AI plays. Open a level in
   Geometry Dash. The bar shows which game is connected.
5. **Stop** saves the brain, as always. **Watch** plays without learning, and your mod reconnects by itself.

The pairing code keeps other computers on your network from playing with your Vyrn. Each project has its own, and it
never leaves this computer: shared projects don't include it. A game on Vyrn's own computer (`ws://127.0.0.1:...`)
doesn't need it, unless it's a web page in a browser. **New code** in Settings → Vyrn Link makes a fresh one.

## Real time or lockstep?

- **Real time** (the default): the game runs at its normal speed and Vyrn decides 30 times a second
  (Settings → **Decisions per second**). Network delays make the timing a bit uneven.
- **Lockstep** (Settings → Vyrn Link → **Lockstep**): the game waits for the AI at every step, like a simulation.
  Training is repeatable and can run faster than real time. **Frames per step** sets how many of the game's frames
  pass between two decisions (2 at 60 frames a second is like 30 decisions a second). Your mod has to support it.

When the AI stops playing (you press **Stop**, or an error in your scripts pauses it), Vyrn tells the game to let
go of every button and, in lockstep, to play on by itself.

## When it doesn't connect

- **Wrong pairing code**: Vyrn refuses the game, and the console says so. Check the six digits. After 5 wrong codes
  in a minute Vyrn waits a minute before it listens to that computer again.
- **The game never shows up**: the address must be the one of the computer that runs Vyrn, exactly as the bar shows it, with
  the same port as in Settings → Vyrn Link. Both computers must be on the same network. A firewall on Vyrn's
  computer must allow the port: on the Pi `sudo ufw allow 7457/tcp` (if you use `ufw`); on Windows and macOS, allow
  Vyrn when the system asks (on Windows: **Private networks**). On the gaming PC, allow Geometry Dash through the
  firewall if it asks.
- **Busy**: only one game can play a project at a time. If your game crashed, start it again: Vyrn lets the new
  connection in once the old one stops answering (a second or two).
- **Port in use**: another program, or another Vyrn project that is training, already listens on that port. Pick
  another one in Settings → Vyrn Link, and the same one in your mod.
- **"It stopped sending its state"**: the game is paused or in a menu. The AI waits; it goes on when the game does.

Writing the mod? Read [the Vyrn Link protocol](vyrn-link.md), and test it with `tools/link/link_test_server.py`
before you train.
