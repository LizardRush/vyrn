#!/usr/bin/env python3
"""Build vyrn-<version>-py3-none-any.whl with the standard library only (docs/dev/standalone.md section 4).

    python3 python/tools/make_wheel.py --out-dir DIR [--version V]

Packs python/vyrn/ (not the worker entry or the tools). Deterministic (fixed timestamps, sorted names), offline.
The version defaults to the one in python/pyproject.toml.
"""

import argparse
import base64
import hashlib
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PYTHON_DIR = os.path.dirname(HERE)
STAMP = (2020, 1, 1, 0, 0, 0)
SUFFIXES = (".py", ".in")
SUMMARY = "Vyrn: teach an AI to play games with three small Python scripts"


def pyproject_version():
    with open(os.path.join(PYTHON_DIR, "pyproject.toml"), encoding="utf-8") as file:
        found = re.search(r'^version\s*=\s*"([^"]+)"', file.read(), re.M)
    if not found:
        sys.exit("make_wheel: no version in pyproject.toml")
    return found.group(1)


def package_files():
    out = []
    root = os.path.join(PYTHON_DIR, "vyrn")
    for folder, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__")
        for name in sorted(files):
            if name.endswith(SUFFIXES):
                path = os.path.join(folder, name)
                out.append((os.path.relpath(path, PYTHON_DIR).replace(os.sep, "/"), path))
    return sorted(out)


def digest(data):
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def build(out_dir, version):
    dist = f"vyrn-{version}.dist-info"
    entries = []
    for name, path in package_files():
        with open(path, "rb") as file:
            entries.append((name, file.read()))
    meta = (f"Metadata-Version: 2.1\nName: vyrn\nVersion: {version}\nSummary: {SUMMARY}\n"
            "Requires-Python: >=3.10\nRequires-Dist: numpy\nRequires-Dist: Pillow\n"
            "Provides-Extra: keys\nRequires-Dist: evdev ; (sys_platform == \"linux\") and extra == 'keys'\n")
    wheel = "Wheel-Version: 1.0\nGenerator: vyrn make_wheel\nRoot-Is-Purelib: true\nTag: py3-none-any\n"
    entries += [(f"{dist}/METADATA", meta.encode()), (f"{dist}/WHEEL", wheel.encode()),
                (f"{dist}/top_level.txt", b"vyrn\n")]
    record = "".join(f"{name},{digest(data)},{len(data)}\n" for name, data in entries)
    record += f"{dist}/RECORD,,\n"
    entries.append((f"{dist}/RECORD", record.encode()))
    os.makedirs(out_dir, exist_ok=True)
    target = os.path.join(out_dir, f"vyrn-{version}-py3-none-any.whl")
    part = target + ".part"
    with zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name, data in entries:
            info = zipfile.ZipInfo(name, STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zf.writestr(info, data)
    os.replace(part, target)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--version")
    args = parser.parse_args()
    print(build(args.out_dir, args.version or pyproject_version()))


if __name__ == "__main__":
    main()
