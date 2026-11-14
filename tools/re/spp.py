#!/usr/bin/env python3
"""List or extract entries of a Skypine .spp resource pack (read-only on the pack).

Layout, as observed in the head unit's CarInfoRes*.spp and CommonRes.spp:
  b"spp\\0", u32 entry count, u32 unknown, u32 unknown
  per entry: u32 name length in bytes, UTF-16LE name, u32 size, u32 offset
  entry data follows the directory; offsets are relative to the end of the directory.

usage: spp.py PACK.spp            list entries (size, offset, name)
       spp.py PACK.spp OUT_DIR    extract every entry into a NEW directory
"""
from __future__ import annotations

import argparse
from pathlib import Path, PureWindowsPath
import struct
import sys
from typing import NamedTuple

MAGIC = b"spp\0"


class Entry(NamedTuple):
    name: str
    size: int
    offset: int


def parse(data: bytes) -> tuple[list[Entry], int]:
    """Return the entries and the absolute offset where entry data starts."""
    if data[:4] != MAGIC:
        raise ValueError("Not an .spp pack (bad magic)")
    count = struct.unpack_from("<I", data, 4)[0]
    pos = 16
    entries = []
    for _ in range(count):
        (length,) = struct.unpack_from("<I", data, pos)
        pos += 4
        name = data[pos:pos + length].decode("utf-16le")
        pos += length
        size, offset = struct.unpack_from("<II", data, pos)
        pos += 8
        entries.append(Entry(name, size, offset))
    for entry in entries:
        if pos + entry.offset + entry.size > len(data):
            raise ValueError(f"Entry runs past end of file: {entry.name}")
    return entries, pos


def safe_relative(name: str) -> Path:
    """Map a stored Windows-style name to a relative path, refusing escapes."""
    parts = PureWindowsPath(name).parts
    if not parts or PureWindowsPath(name).anchor or any(p in ("..", ".") for p in parts):
        raise ValueError(f"Refusing unsafe entry name: {name!r}")
    return Path(*parts)


def extract(data: bytes, out_dir: Path) -> list[Path]:
    entries, base = parse(data)
    if out_dir.exists():
        raise ValueError("Output directory already exists; choose a NEW one")
    written = []
    for entry in entries:
        target = out_dir / safe_relative(entry.name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data[base + entry.offset:base + entry.offset + entry.size])
        written.append(target)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pack", type=Path)
    parser.add_argument("out_dir", type=Path, nargs="?")
    args = parser.parse_args(argv)
    try:
        data = args.pack.read_bytes()
        if args.out_dir is None:
            entries, _ = parse(data)
            for entry in entries:
                print(f"{entry.size:9d} {entry.offset:9x} {entry.name}")
            print(f"# {len(entries)} entries", file=sys.stderr)
        else:
            written = extract(data, args.out_dir)
            print(f"Extracted {len(written)} entries to {args.out_dir}")
    except (OSError, ValueError, UnicodeDecodeError, struct.error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
