#!/usr/bin/env python3
"""Build an SD-card overlay on a computer; never execute the Windows CE binaries."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import sys
import tempfile
from urllib.error import URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = "typlo-gh/Ford-WINCE-Autokit"
REVISION = "986cf5464c61eae40d101985027fa24b2e6fde79"
# Path: (exact byte length, Git blob SHA-1), pinned to the reference commit.
DEPENDENCIES = {
    "LaunchTotalCommander/Arthur.exe": (8192, "87277a137a845cbcc5572ed1308aaaa3ecdd1170"),
    "LaunchTotalCommander/MortScript.exe": (94720, "9d1993bdb9ded9fb5e60e6397cea2e285a2eb6bd"),
    "TC/cecmd.exe": (337920, "1925f097d2e2ef09516154b1d9638d38cc97a8c9"),
}


def blob_sha(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def ce_machine(data: bytes) -> int:
    """Check PE32 / Windows CE GUI headers, not OS/API compatibility."""
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Not a PE executable")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset + 94 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError("Invalid PE header")
    optional = offset + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x10B:
        raise ValueError("Expected a PE32 executable")
    if struct.unpack_from("<H", data, optional + 68)[0] != 9:
        raise ValueError("Expected the Windows CE GUI subsystem")
    return struct.unpack_from("<H", data, offset + 4)[0]


def family(machine: int) -> int:
    # Legacy CE ARM and Thumb binaries can belong to the same CPU family.
    return 0x1C0 if machine in (0x1C0, 0x1C2) else machine


def entry_path(value: str) -> Path:
    path = PurePosixPath(value)
    if (not value or "\\" in value or path.is_absolute()
            or any(p in ("", ".", "..") or ":" in p for p in value.split("/"))
            or path.suffix.lower() != ".exe"):
        raise ValueError("Entry point must be a relative .exe path using /, without ..")
    if (path.parts[0].lower() == "launchtotalcommander"
            or path.name.lower() in {"originalapp.exe", "mortscript.exe", "cecmd.exe"}):
        raise ValueError("Entry point conflicts with a generated launcher file")
    return Path(*path.parts)


def get_dependencies(upstream_dir: Path | None) -> dict[str, bytes]:
    result = {}
    for name, (size, expected) in DEPENDENCIES.items():
        if upstream_dir is None:
            url = f"https://raw.githubusercontent.com/{UPSTREAM}/{REVISION}/{name}"
            request = Request(url, headers={"User-Agent": "montero-wince-launcher/1.0"})
            with urlopen(request, timeout=30) as response:
                data = response.read(size + 1)
        else:
            with (upstream_dir / name).open("rb") as stream:
                data = stream.read(size + 1)
        if len(data) != size or blob_sha(data) != expected:
            raise ValueError(f"Integrity check failed: {name}; no overlay was installed")
        ce_machine(data)
        result[name] = data
    return result


def build(source_root: Path, output: Path, entrypoint: str = "Navigation/Navigation.exe",
          upstream_dir: Path | None = None) -> Path:
    source_root = source_root.resolve(strict=True)
    entry = entry_path(entrypoint)
    source = source_root / entry
    if not source.resolve(strict=True).is_relative_to(source_root):
        raise ValueError("Entry point resolves outside the source directory")
    # Refuse a second install over an existing menu or unrelated support files.
    companions = [entry.with_suffix(".mscr"), entry.parent / "OriginalApp.exe",
                  entry.parent / "MortScript.exe", entry.parent / "cecmd.exe"]
    for relative in companions:
        if (source_root / relative).exists() or (source_root / relative).is_symlink():
            raise ValueError(f"Source already contains {relative}; use an untouched backup")
    if output.exists() or output.is_symlink():
        raise ValueError("Output already exists; choose a NEW --output directory")
    original = source.read_bytes()
    original_machine = ce_machine(original)
    dependencies = get_dependencies(upstream_dir)
    if any(family(ce_machine(data)) != family(original_machine)
           for data in dependencies.values()):
        raise ValueError("CPU architecture mismatch; do not install these binaries")
    script = (ROOT / "LaunchTotalCommander/Arthur.mscr").read_text(encoding="ascii")
    script_bytes = script.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii")
    runner = dependencies["LaunchTotalCommander/Arthur.exe"]
    runtime = dependencies["LaunchTotalCommander/MortScript.exe"]
    commander = dependencies["TC/cecmd.exe"]
    files = {
        Path("LaunchTotalCommander/Arthur.exe"): runner,
        Path("LaunchTotalCommander/Arthur.mscr"): script_bytes,
        Path("LaunchTotalCommander/MortScript.exe"): runtime,
        Path("LaunchTotalCommander/cecmd.exe"): commander,
        entry: runner,
        entry.with_suffix(".mscr"): script_bytes,
        entry.parent / "MortScript.exe": runtime,
        entry.parent / "cecmd.exe": commander,
        entry.parent / "OriginalApp.exe": original,
    }
    manifest = {
        "upstream": UPSTREAM, "revision": REVISION, "entrypoint": entry.as_posix(),
        "original_pe_machine": f"0x{original_machine:04x}",
        "original_git_blob_sha1": blob_sha(original),
        "hardware_tested": False,
        "sha256": {p.as_posix(): hashlib.sha256(data).hexdigest() for p, data in files.items()},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".tc-stage-", dir=output.parent))
    try:
        for relative, data in files.items():
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if output.exists() or output.is_symlink():
            raise ValueError("Output appeared during preparation; refusing to overwrite it")
        stage.rename(output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT,
                        help="Repository or untouched working SD-card backup")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/total-commander-sd-overlay")
    parser.add_argument("--entrypoint", default="Navigation/Navigation.exe")
    parser.add_argument("--upstream-dir", type=Path,
                        help="Offline reference-project directory; hashes are still checked")
    args = parser.parse_args()
    try:
        output = build(args.source_root, args.output, args.entrypoint, args.upstream_dir)
    except (OSError, ValueError, URLError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Prepared: {output}")
    print("Merge its CONTENTS onto a backed-up TEST SD card. Do not replace whole folders.")
    print("The source files were not changed. This is an overlay, not a complete SD image.")
    print("Windows CE / head-unit runtime behavior still requires a parked-vehicle test.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
