"""python -m vyrn play <folder> [options]: play a compiled Vyrn AI (see vyrn.runtime)."""

import sys


def main(argv):
    if len(argv) >= 2 and argv[0] == "play":
        import vyrn.runtime
        return vyrn.runtime.main(argv[1], argv[2:])
    print("usage: python -m vyrn play <folder> [--rounds N] [--speed X] [--allow-keys] [--json] ...\n"
          "<folder> is a folder made by Vyrn's Compile (it holds play.py and brain.vrl).", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
