# Why isn't it learning?

## First, give it time

| Project | Usually learns within |
|---|---|
| Catch (numbers or pictures) | 2–10 minutes |
| A small game in `world.py` | 10–60 minutes |
| A real game on screen (10 decisions/s) | 1 hour to see progress, many hours to get good |

At the start the AI explores. "Exploring 100%" means it's choosing at random. The number drops as it learns.

## Then check these, in order

1. **Does it see what matters?** Look at **What the AI sees**. If you can't tell what to do from that picture or
   those numbers, the AI can't either.
   - Pick a smaller area around the action.
   - Return numbers instead of a picture.
2. **Do the points reward the right thing?** Watch the **Points** chart and use `vyrn.show("points", value)` to see
   what `points()` returns while it plays.
   - Points that never change teach nothing.
   - Points that only come at the very end are hard to learn from. Add small rewards for progress.
3. **Do the choices work?** Press **Check**. It says how many choices the AI has, and the **Choices** panel names
   them. To see what each one does ("hold A", "tap Space"), run `vyrn-cli check <project folder>` in a terminal.
   - Is there a do-nothing choice?
   - Does the game react at your decisions per second? Some games ignore very short taps. Try holding.
4. **Does a round end when it should?** If `game_over()` never turns True, every round runs to the time limit.
   Check warns you if it never saw a game over. (A real game rarely ends during a short Check, so there it only
   adds a note.)
5. **Is it fast enough?** The status bar shows decisions per second. If `see()` is slow (say, a very big screen
   area), a real game may not keep up. Pick a smaller area.

## Starting over

Big changes (a different picture size, new choices, a new idea for `points()`) may need a fresh brain. Use
**Brain ▾ → Start over (new brain)…**. Vyrn keeps the old brain, so you can switch back.
