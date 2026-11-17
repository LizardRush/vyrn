"""Play a trained Vyrn AI without the app (docs/dev/standalone.md): numpy only, no torch.

    import vyrn.runtime
    vyrn.runtime.main(folder, argv)      # what a compiled AI's play.py calls

or from a terminal:  python -m vyrn play <folder> [options]
"""

import argparse
import os
import sys

__all__ = ["main"]


def _parser(folder):
    p = argparse.ArgumentParser(
        prog="play.py", description="Play this trained AI. Press Ctrl+C to stop.",
        epilog="Real games: keys are only described unless you add --allow-keys.")
    p.add_argument("--brain", metavar="FILE", help="the brain to use (default: brain.vrl next to play.py)")
    p.add_argument("--rounds", type=int, metavar="N", help="stop after N rounds (default: keep playing)")
    p.add_argument("--speed", type=float, default=1.0, metavar="X",
                   help="simulations: X times the usual speed; 0 = as fast as possible (default 1)")
    p.add_argument("--explore", type=float, metavar="E", help="chance of a random move, 0 to 1 (default 0.01)")
    p.add_argument("--seed", type=int, metavar="N", help="make the random choices repeatable")
    p.add_argument("--allow-keys", action="store_true",
                   help="let the AI press real keys (after a 3 second countdown). Without it keys are only described")
    p.add_argument("--view", action="store_true", help="show a small window with what the AI sees")
    p.add_argument("--quiet", action="store_true", help="print only problems")
    p.add_argument("--json", action="store_true", help="print one JSON line per round")
    return p


def main(folder, argv=None):
    """Play the compiled AI in `folder`. Returns the exit status: 0 fine (or Ctrl+C), 1 a problem, 2 wrong options."""
    folder = os.path.abspath(str(folder))
    opts = _parser(folder).parse_args(list(sys.argv[1:] if argv is None else argv))
    if opts.rounds is not None and opts.rounds < 1:
        print("--rounds needs a number of 1 or more.", file=sys.stderr)
        return 2
    if opts.speed < 0 or opts.speed != opts.speed:
        print("--speed needs a number of 0 or more (0 means as fast as possible).", file=sys.stderr)
        return 2
    if opts.explore is not None and not 0.0 <= opts.explore <= 1.0:
        print("--explore needs a number from 0 to 1.", file=sys.stderr)
        return 2
    brain = os.path.abspath(opts.brain) if opts.brain else os.path.join(folder, "brain.vrl")
    if not os.path.isdir(folder):
        print(f"There is no folder {folder}.", file=sys.stderr)
        return 1

    from . import _play, _vrl
    player = _play.Player(folder, brain, opts)
    try:
        player.setup()
        player.run()
    except KeyboardInterrupt:
        pass
    except _vrl.BrainFileError as err:
        print(err.args[0], file=sys.stderr)
        return 1
    except _play.PlayError as err:
        print(err.message + (f"\n{err.hint}" if err.hint else ""), file=sys.stderr)
        return 1
    finally:
        player.close()
    player.out.summary()
    return 0
