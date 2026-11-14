#!/usr/bin/env python3
"""Build a complete SD Navigation replacement that launches Total Commander directly."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
from urllib.error import URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = "typlo-gh/Ford-WINCE-Autokit"
REVISION = "986cf5464c61eae40d101985027fa24b2e6fde79"
URL = f"https://raw.githubusercontent.com/{UPSTREAM}/{REVISION}/TC/cecmd.exe"
EXPECTED_SIZE = 337920
EXPECTED_BLOB = "1925f097d2e2ef09516154b1d9638d38cc97a8c9"
AUTOKIT_BLOB = "0724152188983e82768750dd9d5a48f652d04ac8"
MORTSCRIPT_BLOB = "87277a137a845cbcc5572ed1308aaaa3ecdd1170"
ENTRYPOINT = Path("Navigation/Navigation.exe")
INSTALL_TEXT = """DIRECT TOTAL COMMANDER REPLACEMENT

This is a COMPLETE Navigation folder, not the earlier MortScript overlay.
1. Save the current SD Navigation folder on your computer.
2. Remove the old Navigation folder from the SD card.
3. Copy this package's Navigation folder to the card ROOT.
   Correct: Navigation/Navigation.exe
   Incorrect: Navigation/Navigation/Navigation.exe
4. Eject safely, restart the head unit normally, and open Navigation while parked.
Expected: Total Commander file browser directly. No selection menu or CarPlay.
Do not copy the repository's original Navigation folder back onto the card.

Navigation.exe is the reference cecmd.exe with a different filename, not a wrapper.
No AutoKit, MortScript, scripts, or AutoKit DLLs are included in the new folder.
LaunchTotalCommander/LaunchTotalCommander.exe is an optional direct entry point.
Do not modify Navigator, GenieTek, internal flash, registry, or vehicle ports.
This is not a firmware/map update. Head-unit runtime compatibility is untested.
Total Commander can modify files and save settings: browse only at first.

If CarPlay still opens, run the preparation script with --verify-sd <card-root>.
After successful verification and a full head-unit restart, another launch path
or an internal copy may be in use; investigate rather than replacing other files.
"""
NOTICE = f"""Total Commander/CE by Christian Ghisler / C. Ghisler & Co.
Copyright remains with its author. This is third-party freeware.
Publisher information and terms: https://www.ghisler.com/cecmd.htm
Reference binary: https://github.com/{UPSTREAM}/blob/{REVISION}/TC/cecmd.exe
Git blob SHA-1: {EXPECTED_BLOB}
Length: {EXPECTED_SIZE} bytes
The binary is not patched; Navigation.exe is a renamed copy of cecmd.exe.
Hash verification establishes equality to the reference, not a security audit
or runtime compatibility. No optional plugins, maps, or fonts are included.
"""


def blob_sha(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def is_commander(data: bytes) -> bool:
    return len(data) == EXPECTED_SIZE and blob_sha(data) == EXPECTED_BLOB


def ce_machine(data: bytes) -> int:
    """Validate PE32/CE headers, not compatibility with the physical head unit."""
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Not a PE executable")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset + 94 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError("Invalid PE header")
    if struct.unpack_from("<H", data, offset + 24)[0] != 0x10B:
        raise ValueError("Expected a PE32 executable")
    if struct.unpack_from("<H", data, offset + 92)[0] != 9:
        raise ValueError("Expected the Windows CE GUI subsystem")
    return struct.unpack_from("<H", data, offset + 4)[0]


def family(machine: int) -> int:
    return 0x1C0 if machine in (0x1C0, 0x1C2) else machine


def get_commander(local_file: Path | None = None) -> bytes:
    if local_file is None:
        request = Request(URL, headers={"User-Agent": "montero-direct-launcher/2.0"})
        with urlopen(request, timeout=30) as response:
            data = response.read(EXPECTED_SIZE + 1)
    else:
        with local_file.open("rb") as stream:
            data = stream.read(EXPECTED_SIZE + 1)
    if not is_commander(data):
        raise ValueError("Total Commander size/hash mismatch; no output created")
    ce_machine(data)
    return data


def entrypoint(root: Path) -> Path:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("Expected a repository, SD backup, or mounted SD root directory")
    target = root / ENTRYPOINT
    current = root
    for part in ENTRYPOINT.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError("Refusing a symlink in the Navigation entry-point path")
    if not target.is_file():
        raise ValueError(f"Entry point not found: {target}")
    return target


def verify_sd(root: Path) -> Path:
    """Read-only and offline: check the actual card, not just a generated package."""
    target = entrypoint(root)
    data = target.read_bytes()
    if not is_commander(data):
        name = {AUTOKIT_BLOB: "AutoKit", MORTSCRIPT_BLOB: "the old MortScript launcher"}.get(
            blob_sha(data), "another executable")
        raise ValueError(f"{target} is still {name}, not Total Commander. "
                         f"Bytes={len(data)}, Git blob SHA-1={blob_sha(data)}")
    ce_machine(data)
    return target


def build(source_root: Path, output: Path, local_file: Path | None = None) -> Path:
    """Create a fresh replacement folder; never modify source or install onto the SD."""
    original = entrypoint(source_root).read_bytes()
    original_machine = ce_machine(original)
    if output.exists() or output.is_symlink():
        raise ValueError("Output already exists; choose a NEW --output directory")
    commander = get_commander(local_file)
    if family(original_machine) != family(ce_machine(commander)):
        raise ValueError("CPU family mismatch; no replacement folder created")
    files = {
        ENTRYPOINT.as_posix(): commander,
        "LaunchTotalCommander/LaunchTotalCommander.exe": commander,
        "INSTALL.txt": INSTALL_TEXT.encode("ascii"),
        "THIRD_PARTY.txt": NOTICE.encode("ascii"),
    }
    manifest = {
        "mode": "direct-whole-folder-replacement", "hardware_tested": False,
        "upstream": UPSTREAM, "revision": REVISION, "entrypoint": ENTRYPOINT.as_posix(),
        "source_git_blob_sha1": blob_sha(original),
        "source_pe_machine": f"0x{original_machine:04x}",
        "commander_git_blob_sha1": blob_sha(commander), "commander_size": len(commander),
        "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".tc-direct-", dir=output.parent))
    try:
        for name, data in files.items():
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            if path.read_bytes() != data:
                raise ValueError(f"Output verification failed: {name}")
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if output.exists() or output.is_symlink():
            raise ValueError("Output appeared during preparation; not overwriting it")
        stage.rename(output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path, default=ROOT / "dist/total-commander-direct")
    offline = parser.add_mutually_exclusive_group()
    offline.add_argument("--cecmd", type=Path, help="Local pinned TC/cecmd.exe for offline builds")
    offline.add_argument("--upstream-dir", type=Path, help="Local reference project containing TC/cecmd.exe")
    parser.add_argument("--verify-sd", type=Path, help="Only verify a mounted SD card; no writes/downloads")
    args = parser.parse_args(argv)
    try:
        if args.verify_sd is not None:
            target = verify_sd(args.verify_sd)
            print(f"VERIFIED: {target} is Total Commander ({EXPECTED_SIZE} bytes).")
            return 0
        local = args.upstream_dir / "TC/cecmd.exe" if args.upstream_dir is not None else args.cecmd
        result = build(args.source_root, args.output, local)
    except (OSError, ValueError, URLError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Prepared COMPLETE replacement folder: {result / 'Navigation'}")
    print("REPLACE the SD card's entire Navigation folder with this GENERATED Navigation folder.")
    print("Do not copy the repository's original Navigation folder or use the old SD overlay.")
    print("No menu, MortScript, or CarPlay fallback. Source files were not changed.")
    print("Eject safely; restart the head unit normally; test while parked. Runtime is unverified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
