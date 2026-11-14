#!/usr/bin/env python3
"""Print ASCII (A) and UTF-16LE (W) strings with their file offsets.

Windows CE programs keep most API strings (device names, window titles) in UTF-16,
which the standard `strings` tool misses.

usage: cestrings.py FILE [MIN_LENGTH]
"""
from __future__ import annotations

import re
import sys


def strings(data: bytes, min_length: int = 4):
    for match in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % min_length, data):
        yield match.start(), "W", match.group().decode("utf-16le")
    for match in re.finditer(rb"[\x20-\x7e]{%d,}" % min_length, data):
        yield match.start(), "A", match.group().decode("ascii")


def main(argv: list[str]) -> int:
    if not 2 <= len(argv) <= 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], "rb") as stream:
        data = stream.read()
    for offset, kind, text in sorted(strings(data, int(argv[2]) if len(argv) == 3 else 4)):
        print(f"{kind} {offset:08x} {text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
