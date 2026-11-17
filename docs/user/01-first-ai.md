# Your first AI in 5 minutes

We'll teach an AI to balance a pole on a cart. The pole wants to fall over, and the AI has to keep shoving the cart
so it doesn't. There's no code to write yet. The project is ready to go.

1. **Open Vyrn.** On the Raspberry Pi it's in the menu (Programming → Vyrn). On a Mac, press ⌘ Space and type
   *Vyrn*. On Windows, open the Start menu and type *Vyrn*. Not installed yet? See the README, or
   [Vyrn on a Mac or a Windows PC](08-mac-and-windows.md).
2. Click **New project**, pick **Demo: Balance**, name it `balance` and click **Create**.
3. Press **Train** (▶). Vyrn checks your scripts first, then the AI starts playing.
   - At the start it just flails around ("Exploring 100%"). That's on purpose: it has to try things to learn.
   - Keep an eye on the **Points** chart at the bottom. Every dot is one round, and the thick line is the average.
4. After a minute or two the line climbs, because the pole stays up longer and longer. Press **Stop** (■) whenever
   you like. The brain saves itself.
5. Press **Watch** (👁) to see the trained AI play at normal speed. The **Watch speed** slider makes it faster or
   slower.

## What just happened?

Your project is a few small files, one tab each:

| File | What it says |
|---|---|
| `world.py` | The game itself: a pole that leans a bit more every tick, and a cart you can shove |
| `inputs.py` | **What the AI sees:** two numbers, how far the pole leans and how fast it's tipping |
| `responses.py` | **What its choices do:** `left` and `right` shove the cart, `stay` does nothing |
| `points.py` | **How it scores:** +1 for every tick the pole is up, −5 when it falls |

Vyrn is the **chip**. It builds a neural network, plays thousands of rounds and works out which choice tends to earn
the most points. Your Python is the **microcontroller**: it only tells Vyrn what the AI sees, what its choices do
and how it scores.

## Try changing something

- In `points.py`, make falling hurt more: `return -20`. Press **Train** again and the same brain keeps learning. If
  you'd rather begin from scratch, use **Brain ▾ → Start over (new brain)…**.
- Look at the **What the AI sees** panel on the right. Whatever's in there is all the network ever gets to know.
- Want to see the same idea with a picture instead of numbers? Open **Demo: Catch (pictures)** from New project.
  The AI catches falling fruit using only a tiny 10×10 image.
- Next up: [The three files](02-three-files.md).
