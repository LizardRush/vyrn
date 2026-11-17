#!/usr/bin/env python3
"""Draw Vyrn's app icon: the white pixel logo (packaging/vyrn.svg) on a black rounded square.

    python3 packaging/make_icon.py                     writes packaging/vyrn.png (256x256)
    python3 packaging/make_icon.py --size 1024 --out /tmp/vyrn-1024.png

Needs only Pillow. The logo lives on a 32x32 grid of whole pixels, so it stays sharp at any size.
"""

import argparse
import os

from PIL import Image, ImageDraw

# The logo: (x, y, width, height) boxes on a 32x32 grid, white. The same shapes as packaging/vyrn.svg.
LOGO = [(0, 0, 5, 15), (13, 0, 6, 15), (27, 0, 5, 15), (0, 15, 8, 17), (11, 15, 10, 17), (24, 15, 8, 17)]
SUPERSAMPLE = 4


def draw_icon(size):
    full = size * SUPERSAMPLE
    image = Image.new("RGBA", (full, full), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle([0, 0, full - 1, full - 1], radius=round(full * 0.22), fill=(0, 0, 0, 255))
    # The logo takes the middle 56% of the square; each of its 32 cells is a whole number of pixels when it can be.
    cell = max(1, round(full * 0.56 / 32))
    side = cell * 32
    ox = oy = (full - side) // 2
    for x, y, w, h in LOGO:
        draw.rectangle([ox + x * cell, oy + y * cell, ox + (x + w) * cell - 1, oy + (y + h) * cell - 1],
                       fill=(255, 255, 255, 255))
    return image.resize((size, size), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--size", type=int, default=256)
    parser.add_argument("--out", default=os.path.join(here, "vyrn.png"))
    args = parser.parse_args()
    draw_icon(args.size).save(args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
