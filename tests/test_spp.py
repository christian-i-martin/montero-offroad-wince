"""Synthetic .spp packs test the resource-pack reader, not real head-unit files."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/re/spp.py"
spec = importlib.util.spec_from_file_location("spp", SCRIPT)
spp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(spp)


def pack(entries):
    """Build a pack from (name, bytes) pairs with the observed layout."""
    directory, blob = b"", b""
    for name, data in entries:
        encoded = name.encode("utf-16le")
        directory += struct.pack("<I", len(encoded)) + encoded + struct.pack("<II", len(data), len(blob))
        blob += data
    return b"spp\0" + struct.pack("<III", len(entries), 0, len(entries)) + directory + blob


class SppTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def test_lists_entries_in_order(self):
        entries, _ = spp.parse(pack([("a.png", b"AAA"), ("carinfo.ini", b"BB")]))
        self.assertEqual([(e.name, e.size) for e in entries], [("a.png", 3), ("carinfo.ini", 2)])

    def test_extracts_backslash_names_as_folders(self):
        out = self.root / "out"
        spp.extract(pack([("wallpaper\\01.skp", b"xyz"), ("b.bin", b"1")]), out)
        self.assertEqual((out / "wallpaper" / "01.skp").read_bytes(), b"xyz")
        self.assertEqual((out / "b.bin").read_bytes(), b"1")

    def test_rejects_bad_magic(self):
        with self.assertRaises(ValueError):
            spp.parse(b"xxx\0" + bytes(12))

    def test_rejects_entry_past_end(self):
        data = pack([("a", b"1234")])[:-2]
        with self.assertRaises(ValueError):
            spp.parse(data)

    def test_refuses_path_escape(self):
        for name in ("..\\evil.txt", "C:\\evil.txt", "\\evil.txt"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                spp.extract(pack([(name, b"x")]), self.root / f"out{abs(hash(name))}")
        self.assertFalse((self.root / "evil.txt").exists())

    def test_refuses_existing_output(self):
        out = self.root / "exists"
        out.mkdir()
        with self.assertRaises(ValueError):
            spp.extract(pack([("a", b"1")]), out)


if __name__ == "__main__":
    unittest.main()
