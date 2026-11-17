#!/usr/bin/env python3
"""A stand-in for `grim` used by the Vyrn tests (protocol.md section 1: VYRN_GRIM).

It accepts grim's arguments (`-g "x,y wxh"`, `-t ppm`, `-` for stdout; other options are ignored) and writes a
binary PPM (P6) of a fake screen. The screen is described by the JSON file named by $VYRN_FAKE_GRIM_CONTROL:

    {"width": 64, "height": 48,            screen size (default 64 x 48)
     "color": [r, g, b],                   background colour (default [40, 80, 120])
     "pattern": "solid" | "coords",        "coords": red = x % 256, green = y % 256, blue = color[2]
     "image": "/path/screen.png",          use this picture as the whole screen instead (needs Pillow)
     "rects": [{"x": 0, "y": 0, "w": 1, "h": 1, "color": [r, g, b]}],   painted on top, in order
     "delay": 0.0,                         seconds to wait before answering
     "fail": "message",                    print "grim: message" to stderr and exit 1 instead
     "log": "/path/grim.log"}              append one JSON line per run: {"argv", "geometry", "t", "pid"}

The log lets tests count captures (for example the once-per-request union cache of protocol.md / _screen.py).
"""

import json
import os
import re
import sys
import time

DEFAULTS = {"width": 64, "height": 48, "color": [40, 80, 120], "pattern": "solid"}
OPTIONS_WITH_VALUE = {"-g", "-t", "-s", "-o", "-q", "-l", "-T"}


def load_control():
    config = dict(DEFAULTS)
    path = os.environ.get("VYRN_FAKE_GRIM_CONTROL", "")
    if path:
        try:
            with open(path, encoding="utf-8") as file:
                config.update(json.load(file))
        except FileNotFoundError:
            pass
    return config


def parse_args(argv):
    """Return (geometry or None, format, output) from grim-style arguments."""
    geometry, fmt, output = None, "png", None
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in OPTIONS_WITH_VALUE and i + 1 < len(argv):
            if arg == "-g":
                geometry = argv[i + 1]
            elif arg == "-t":
                fmt = argv[i + 1]
            i += 2
            continue
        if arg == "-" or not arg.startswith("-"):
            output = arg
        i += 1
    return geometry, fmt, output


def parse_geometry(text):
    match = re.fullmatch(r"\s*(-?\d+)\s*,\s*(-?\d+)\s+(\d+)\s*x\s*(\d+)\s*", text)
    if not match:
        raise ValueError(f"invalid geometry {text!r}")
    return tuple(int(v) for v in match.groups())


def rgb(value):
    return bytes(int(c) & 255 for c in list(value)[:3])


def render(config, x0, y0, w, h):
    """RGB bytes of the screen region (x0, y0, w, h), which lies inside the screen."""
    if config.get("image"):
        from PIL import Image  # only needed for picture screens

        with Image.open(config["image"]) as picture:
            return bytearray(picture.convert("RGB").crop((x0, y0, x0 + w, y0 + h)).tobytes())

    color = rgb(config.get("color", DEFAULTS["color"]))
    if config.get("pattern") == "coords":
        reds = bytes(x & 255 for x in range(x0, x0 + w))
        blue = bytes([color[2]]) * w
        rows = []
        for y in range(y0, y0 + h):
            row = bytearray(3 * w)
            row[0::3] = reds
            row[1::3] = bytes([y & 255]) * w
            row[2::3] = blue
            rows.append(bytes(row))
        data = bytearray(b"".join(rows))
    else:
        data = bytearray(color * (w * h))

    for rect in config.get("rects") or []:
        rx0, ry0 = max(int(rect["x"]), x0), max(int(rect["y"]), y0)
        rx1 = min(int(rect["x"]) + int(rect["w"]), x0 + w)
        ry1 = min(int(rect["y"]) + int(rect["h"]), y0 + h)
        if rx0 >= rx1 or ry0 >= ry1:
            continue
        fill = rgb(rect["color"]) * (rx1 - rx0)
        for y in range(ry0, ry1):
            start = ((y - y0) * w + (rx0 - x0)) * 3
            data[start:start + len(fill)] = fill
    return data


def screen_size(config):
    if config.get("image"):
        from PIL import Image

        with Image.open(config["image"]) as picture:
            return picture.size
    return int(config["width"]), int(config["height"])


def main(argv):
    config = load_control()
    geometry_text, fmt, output = parse_args(argv)

    region = None
    if geometry_text is not None:
        try:
            region = parse_geometry(geometry_text)
        except ValueError as error:
            sys.stderr.write(f"grim: {error}\n")
            return 1

    if config.get("log"):
        entry = {"argv": argv, "geometry": list(region) if region else None, "t": time.monotonic(),
                 "pid": os.getpid()}
        with open(config["log"], "a", encoding="utf-8") as log:
            log.write(json.dumps(entry) + "\n")

    if config.get("delay"):
        time.sleep(float(config["delay"]))
    if config.get("fail"):
        sys.stderr.write(f"grim: {config['fail']}\n")
        return 1

    width, height = screen_size(config)
    x, y, w, h = region if region else (0, 0, width, height)
    x0, y0 = max(x, 0), max(y, 0)
    x1, y1 = min(x + w, width), min(y + h, height)
    if x0 >= x1 or y0 >= y1:
        sys.stderr.write("grim: the requested region is outside the screen\n")
        return 1
    w, h = x1 - x0, y1 - y0

    payload = b"P6\n%d %d\n255\n" % (w, h) + bytes(render(config, x0, y0, w, h))
    if fmt != "ppm":
        import io

        from PIL import Image

        buffer = io.BytesIO()
        Image.frombytes("RGB", (w, h), payload[-w * h * 3:]).save(buffer, format=fmt.upper())
        payload = buffer.getvalue()

    if output in (None, "-"):
        sys.stdout.buffer.write(payload)
        sys.stdout.buffer.flush()
    else:
        with open(output, "wb") as file:
            file.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
