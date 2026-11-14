"""Offline packaging tests. Synthetic PE files do not test Windows CE execution."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare", ROOT / "tools/prepare_total_commander.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def executable(tag, machine=0x1C0, subsystem=9):
    data = bytearray(300)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 132, machine)
    struct.pack_into("<H", data, 152, 0x10B)
    struct.pack_into("<H", data, 220, subsystem)
    return bytes(data) + tag


class PackagingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.source = self.base / "source"
        self.original = self.source / "Navigation/Navigation.exe"
        self.original.parent.mkdir(parents=True)
        self.original.write_bytes(executable(b"original autokit"))
        self.vendor = self.base / "vendor"
        fixtures = {}
        for name in prepare.DEPENDENCIES:
            data = executable(name.encode("ascii"))
            path = self.vendor / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            fixtures[name] = (len(data), prepare.blob_sha(data))
        dependency_patch = patch.object(prepare, "DEPENDENCIES", fixtures)
        dependency_patch.start()
        self.addCleanup(dependency_patch.stop)
        self.output = self.base / "dist/overlay"

    def build(self, **kwargs):
        return prepare.build(self.source, self.output, upstream_dir=self.vendor, **kwargs)

    def test_overlay_preserves_source_and_original(self):
        before = self.original.read_bytes()
        self.build()
        self.assertEqual(before, self.original.read_bytes())
        self.assertEqual(before, (self.output / "Navigation/OriginalApp.exe").read_bytes())
        self.assertFalse((self.source / "Navigation/Navigation.mscr").exists())
        self.assertFalse((self.output / "Navigator").exists())
        self.assertEqual(10, len([p for p in self.output.rglob("*") if p.is_file()]))

    def test_matching_launcher_names_and_crlf(self):
        self.build()
        for path in ("Navigation/Navigation", "LaunchTotalCommander/Arthur"):
            self.assertTrue((self.output / (path + ".exe")).is_file())
            data = (self.output / (path + ".mscr")).read_bytes()
            self.assertIn(b'SystemPath("ScriptPath")', data)
            self.assertNotIn(b"SDMEM", data.split(b"folder =")[1])
            self.assertNotIn(b"\n", data.replace(b"\r\n", b""))

    def test_manifest_records_original_hash(self):
        self.build()
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(prepare.blob_sha(self.original.read_bytes()), manifest["original_git_blob_sha1"])
        self.assertFalse(manifest["hardware_tested"])
        self.assertEqual(9, len(manifest["sha256"]))

    def test_existing_output_is_not_overwritten(self):
        self.output.mkdir(parents=True)
        marker = self.output / "keep.txt"
        marker.write_text("keep")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.build()
        self.assertEqual("keep", marker.read_text())

    def test_existing_companion_is_not_overwritten(self):
        (self.original.parent / "OriginalApp.exe").write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "untouched backup"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_tampered_dependency_rejected_before_output(self):
        (self.vendor / "TC/cecmd.exe").write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "Integrity"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_architecture_mismatch_rejected(self):
        self.original.write_bytes(executable(b"x86", machine=0x14C))
        with self.assertRaisesRegex(ValueError, "architecture mismatch"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_thumb_and_arm_family_accepted(self):
        self.original.write_bytes(executable(b"thumb", machine=0x1C2))
        self.build()
        self.assertTrue(self.output.exists())

    def test_invalid_entrypoints_rejected(self):
        for value in ("../Navigation.exe", "/Navigation.exe", "C:/Navigation.exe", "Navigation\\Navigation.exe",
                      "Navigation/../Navigation.exe", "Navigation//Navigation.exe", "Navigation/file.txt",
                      "LaunchTotalCommander/Arthur.exe", "Navigation/MortScript.exe", ""):
            with self.subTest(value=value), self.assertRaises(ValueError):
                prepare.entry_path(value)

    def test_non_ce_executable_rejected(self):
        self.original.write_bytes(executable(b"desktop", subsystem=2))
        with self.assertRaisesRegex(ValueError, "Windows CE"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_truncated_pe_rejected(self):
        for value in (b"", b"MZ", b"MZ" + b"\xff" * 100):
            with self.subTest(value=value[:4]), self.assertRaises(ValueError):
                prepare.ce_machine(value)

    def test_git_empty_blob_hash(self):
        self.assertEqual("e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", prepare.blob_sha(b""))


if __name__ == "__main__":
    unittest.main()
