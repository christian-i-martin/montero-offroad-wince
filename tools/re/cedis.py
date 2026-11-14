#!/usr/bin/env python3
"""Annotated ARM disassembler for the head unit's Windows CE PE files.

The Skypine binaries are 32-bit ARM (machine 0x1c2), built with MSVC 2008. Exported C++
names are kept, so this tool labels calls with export and import names and shows the
strings that literal-pool loads point at. All addresses printed are RVAs (offsets from
the image base) except branch targets, which capstone prints as full addresses.

Needs: pip install -r tools/re/requirements.txt  (capstone, pefile)

usage:
  cedis.py FILE (RVA_HEX | EXPORT_SUBSTRING) [COUNT]   disassemble COUNT instructions
  cedis.py FILE --exports [SUBSTRING]                  list exports (RVA, mangled name)
  cedis.py FILE --xref-str TEXT                        code that loads a pointer to TEXT
  cedis.py FILE --xref-addr RVA_HEX                    code that loads or calls RVA
"""
from __future__ import annotations

import re
import struct
import sys

import capstone
import pefile

CODE = 0x20000000  # IMAGE_SCN_CNT_CODE


class Image:
    def __init__(self, path: str):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = bytes(self.pe.get_memory_mapped_image())
        self.imports = {}  # IAT slot VA -> "DLL!name" (COREDLL imports are by ordinal)
        for entry in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = entry.dll.decode()
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else f"ord{imp.ordinal}"
                self.imports[imp.address] = f"{dll}!{name}"
        self.exports = {}  # RVA -> mangled name
        directory = getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", None)
        for exp in directory.symbols if directory else []:
            if exp.name:
                self.exports[exp.address & ~1] = exp.name.decode()
        self.thunks = self._find_thunks()

    def code_sections(self):
        for section in self.pe.sections:
            if section.Characteristics & CODE:
                yield section.VirtualAddress, section.VirtualAddress + section.Misc_VirtualSize

    def u32(self, rva: int):
        if 0 <= rva <= len(self.mem) - 4:
            return struct.unpack_from("<I", self.mem, rva)[0]
        return None

    def _find_thunks(self):
        """Import stubs: ldr r12,[pc,#x] then ldr pc,[r12] or ldr r12,[r12]; bx r12."""
        thunks = {}
        for start, end in self.code_sections():
            for rva in range(start, end - 8, 4):
                first, second = self.u32(rva), self.u32(rva + 4)
                if (first & 0xFFFFF000) == 0xE59FC000 and second in (0xE59CF000, 0xE59CC000):
                    slot = self.u32(rva + 8 + (first & 0xFFF))
                    if slot in self.imports:
                        thunks[rva] = self.imports[slot]
        return thunks

    def name_of(self, va: int):
        rva = va - self.base
        if va in self.imports:
            return "IAT:" + self.imports[va]
        return self.thunks.get(rva) or self.exports.get(rva)

    def string_at(self, va: int):
        rva = va - self.base
        if not 0 <= rva < len(self.mem) - 2:
            return None
        chunk = self.mem[rva:rva + 200]
        match = re.match(rb"((?:[\x20-\x7e]\x00){3,})\x00\x00", chunk)
        if match:
            return 'L"' + match.group(1).decode("utf-16le") + '"'
        match = re.match(rb"([\x20-\x7e\t\r\n]{3,})\x00", chunk)
        if match:
            return '"' + match.group(1).decode("ascii") + '"'
        return None


def disasm(img: Image, rva: int, count: int) -> str:
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    lines = []
    for ins in md.disasm(img.mem[rva:rva + count * 4], img.base + rva):
        here = ins.address - img.base
        if here in img.exports and here != rva:
            lines.append(f"\n<{img.exports[here]}>:")
        note = ""
        if ins.mnemonic.startswith("ldr") and "[pc" in ins.op_str:
            match = re.search(r"#(-?0x[0-9a-f]+|-?\d+)\]", ins.op_str)
            literal = img.u32(ins.address + 8 + (int(match.group(1), 0) if match else 0) - img.base)
            if literal is not None:
                label = img.name_of(literal) or img.string_at(literal)
                note = f"=0x{literal:x}" + (f" {label}" if label else "")
        elif ins.mnemonic.startswith("b") and ins.op_str.startswith("#0x"):
            note = img.name_of(int(ins.op_str[1:], 16)) or ""
        lines.append(f"{here:08x}: {ins.mnemonic:8s} {ins.op_str:32s} {'; ' + note if note else ''}")
    return "\n".join(lines)


def literal_loads(img: Image, value: int):
    """RVAs of `ldr rX, [pc, #imm]` instructions whose literal equals value."""
    hits = []
    for match in re.finditer(re.escape(struct.pack("<I", value)), img.mem):
        slot = match.start()
        if slot % 4:
            continue
        for back in range(4, 4096, 4):
            word = img.u32(slot - back)
            if word is None:
                break
            if (word & 0x0F7F0000) == 0x051F0000:  # ldr/ldrb with pc base
                imm = word & 0xFFF
                if slot - back + 8 + (imm if word & (1 << 23) else -imm) == slot:
                    hits.append(slot - back)
    return hits


def branch_sites(img: Image, target_rva: int):
    for start, end in img.code_sections():
        for rva in range(start, end, 4):
            word = img.u32(rva)
            if word is not None and (word & 0x0F000000) == 0x0B000000:  # bl
                offset = word & 0xFFFFFF
                offset -= 0x1000000 if offset & 0x800000 else 0
                if rva + 8 + offset * 4 == target_rva:
                    yield rva


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    img, command = Image(argv[1]), argv[2]
    if command == "--exports":
        needle = argv[3] if len(argv) > 3 else ""
        for rva, name in sorted(img.exports.items()):
            if needle in name:
                print(f"{rva:08x} {name}")
    elif command == "--xref-str":
        text = argv[3]
        for encoded in (text.encode("ascii") + b"\0", text.encode("utf-16le") + b"\0\0"):
            for match in re.finditer(re.escape(encoded), img.mem):
                for site in literal_loads(img, img.base + match.start()):
                    print(f"string at {match.start():08x} loaded at {site:08x}")
    elif command == "--xref-addr":
        target = int(argv[3], 16)
        for site in literal_loads(img, img.base + target):
            print(f"literal load at {site:08x}")
        for site in branch_sites(img, target):
            print(f"bl from {site:08x}")
    else:
        try:
            rva = int(command, 16)
        except ValueError:
            matches = sorted((r, n) for r, n in img.exports.items() if command in n)
            if not matches:
                print("ERROR: no export matches", file=sys.stderr)
                return 1
            rva = matches[0][0]
            print(f"<{matches[0][1]}> @ {rva:08x}")
        print(disasm(img, rva, int(argv[3]) if len(argv) > 3 else 80))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
