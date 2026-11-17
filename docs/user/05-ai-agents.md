# Let a coding agent help (Claude Code, Codex, Gemini CLI)

Vyrn has an MCP server, so an AI coding agent can help you build and train projects. It can create projects, write
the three files, run Check, train and watch simulations, read progress, and see what the AI sees.

## Connect it (once)

In Vyrn's start window, **Connect an AI agent** (at the top) shows these lines, each with a Copy button:

```bash
claude mcp add --scope user vyrn -- ~/.local/bin/vyrn-cli mcp
```
```bash
codex mcp add vyrn -- ~/.local/bin/vyrn-cli mcp
```
```bash
gemini mcp add vyrn ~/.local/bin/vyrn-cli mcp
```

These work on the Pi and on a Mac. On Windows, in PowerShell, use the full path of `vyrn-cli.exe` instead:

```powershell
claude mcp add --scope user vyrn -- "$env:LOCALAPPDATA\Programs\Vyrn\bin\vyrn-cli.exe" mcp
```

Then ask your agent something like: *"Use the vyrn tools to make a Catch project that learns faster, then train it
for 5 minutes and tell me how it did."*

## What agents can and can't do

- They only work on projects in your Vyrn folder (`~/Vyrn`; on Windows `C:\Users\<you>\Vyrn`).
- They can write `.py` files in a project and run Check. **That runs Python code on your computer**, the same as
  pressing Check yourself. Only connect agents you trust.
- They can train and watch **simulations** (projects with a `world.py`).
- They can **never** train or watch a real game, because that would press keys on your desktop. You press Train
  yourself.
- They can only look at your screen (Check on a real-game project) if you allow it in that project's
  **Settings → AI coding agents**. A project that doesn't look at the screen, like a Vyrn Link game, can be
  checked without that.
- While an agent trains a project, Vyrn shows "An AI agent is training this project" with a **Stop** button.
- They can share a project (`.vrlp`) or a trained AI (`.vrl`) and open files people send you. A project made from
  someone else's file stays untrusted until **you** trust it ([Share what you trained](06-sharing.md)). Agents can
  read and explain it, but never run it.
