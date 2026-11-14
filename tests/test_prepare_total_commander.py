"""Synthetic offline PE fixtures test packaging, not Windows CE execution."""
from contextlib import redirect_stdout, redirect_stderr
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import URLError

SCRIPT = Path(__file__).resolve().parents[1] / "tools/prepare_total_commander.py"
spec = importlib.util.spec_from_file_location("direct_tc", SCRIPT)
tc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tc)


def pe(machine=0x1C2, marker=b"fixture"):
    data = bytearray(256)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, machine)
    struct.pack_into("<H", data, 88, 0x10B)
    struct.pack_into("<H", data, 156, 9)
    return bytes(data) + marker


class DirectPackageTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.source = self.root / "source"
        self.original = self.source / tc.ENTRYPOINT
        self.original.parent.mkdir(parents=True)
        self.before = pe(marker=b"autokit")
        self.original.write_bytes(self.before)
        self.commander = pe(0x1C0, b"commander")
        self.payload = self.root / "cecmd.exe"
        self.payload.write_bytes(self.commander)
        self.output = self.root / "output"
        for key, value in (("EXPECTED_SIZE", len(self.commander)),
                           ("EXPECTED_BLOB", tc.blob_sha(self.commander)),
                           ("AUTOKIT_BLOB", tc.blob_sha(self.before))):
            patcher = patch.object(tc, key, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def build(self):
        return tc.build(self.source, self.output, self.payload)

    def test_complete_navigation_contains_only_total_commander(self):
        self.build()
        self.assertEqual((self.output / tc.ENTRYPOINT).read_bytes(), self.commander)
        self.assertEqual([p.name for p in (self.output / "Navigation").iterdir()], ["Navigation.exe"])
        self.assertFalse(list(self.output.rglob("*.mscr")))
        self.assertFalse(list(self.output.rglob("MortScript.exe")))
        self.assertFalse(list(self.output.rglob("OriginalApp.exe")))

    def test_source_and_legacy_companions_are_untouched(self):
        companion = self.original.with_suffix(".mscr")
        companion.write_text("old menu", encoding="ascii")
        self.build()
        self.assertEqual(self.original.read_bytes(), self.before)
        self.assertEqual(companion.read_text(), "old menu")

    def test_optional_entrypoint_is_identical(self):
        self.build()
        self.assertEqual((self.output / "LaunchTotalCommander/LaunchTotalCommander.exe").read_bytes(), self.commander)

    def test_manifest_and_install_instructions(self):
        self.build()
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertFalse(manifest["hardware_tested"])
        self.assertEqual(manifest["mode"], "direct-whole-folder-replacement")
        for name, digest in manifest["sha256"].items():
            self.assertEqual(hashlib.sha256((self.output / name).read_bytes()).hexdigest(), digest)
        self.assertIn("COMPLETE Navigation folder", (self.output / "INSTALL.txt").read_text())

    def test_existing_output_is_not_overwritten(self):
        self.output.mkdir()
        with self.assertRaisesRegex(ValueError, "Output already exists"):
            self.build()
        self.assertEqual(list(self.output.iterdir()), [])

    def test_same_size_corruption_rejected(self):
        self.payload.write_bytes(self.commander[:-1] + b"X")
        with self.assertRaisesRegex(ValueError, "size/hash"):
            self.build()
        self.assertFalse(self.output.exists())
        self.assertEqual(self.original.read_bytes(), self.before)

    def test_oversized_payload_rejected(self):
        self.payload.write_bytes(self.commander + b"X")
        with self.assertRaisesRegex(ValueError, "size/hash"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_wrong_cpu_rejected(self):
        self.original.write_bytes(pe(0x14C))
        with self.assertRaisesRegex(ValueError, "CPU family mismatch"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_missing_entrypoint_does_not_download(self):
        self.original.unlink()
        with patch.object(tc, "urlopen", side_effect=AssertionError("network")):
            with self.assertRaisesRegex(ValueError, "Entry point not found"):
                self.build()

    def test_symlinked_entrypoint_rejected(self):
        self.original.unlink()
        self.original.symlink_to(self.payload)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.build()

    def test_verify_is_offline_and_read_only(self):
        self.build()
        before = {p: p.read_bytes() for p in self.output.rglob("*") if p.is_file()}
        with patch.object(tc, "urlopen", side_effect=AssertionError("network")):
            self.assertEqual(tc.verify_sd(self.output), self.output / tc.ENTRYPOINT)
        self.assertEqual(before, {p: p.read_bytes() for p in self.output.rglob("*") if p.is_file()})

    def test_verify_identifies_autokit(self):
        with self.assertRaisesRegex(ValueError, "still AutoKit"):
            tc.verify_sd(self.source)

    def test_verify_identifies_mortscript(self):
        with patch.object(tc, "MORTSCRIPT_BLOB", tc.blob_sha(self.before)), patch.object(tc, "AUTOKIT_BLOB", "other"):
            with self.assertRaisesRegex(ValueError, "old MortScript launcher"):
                tc.verify_sd(self.source)

    def test_network_download_uses_pinned_url(self):
        with patch.object(tc, "urlopen", return_value=io.BytesIO(self.commander)) as request:
            self.assertEqual(tc.get_commander(), self.commander)
        self.assertEqual(request.call_args.args[0].full_url, tc.URL)
        self.assertEqual(request.call_args.kwargs["timeout"], 30)

    def test_invalid_pe_headers_rejected(self):
        invalid = bytearray(pe())
        struct.pack_into("<I", invalid, 60, 999999)
        desktop = bytearray(pe())
        struct.pack_into("<H", desktop, 156, 2)
        pe64 = bytearray(pe())
        struct.pack_into("<H", pe64, 88, 0x20B)
        for data in (b"", b"MZ", bytes(invalid), bytes(desktop), bytes(pe64)):
            with self.subTest(data=data[:8]), self.assertRaises(ValueError):
                tc.ce_machine(data)

    def test_failed_stage_is_cleaned_up(self):
        with patch.object(Path, "rename", side_effect=OSError("simulated failure")):
            with self.assertRaises(OSError):
                self.build()
        self.assertFalse(list(self.root.glob(".tc-direct-*")))
        self.assertFalse(self.output.exists())
        self.assertEqual(self.original.read_bytes(), self.before)

    def test_cli_build_then_verify(self):
        with redirect_stdout(io.StringIO()) as output:
            result = tc.main(["--source-root", str(self.source), "--output", str(self.output), "--cecmd", str(self.payload)])
        self.assertEqual(result, 0)
        self.assertIn("REPLACE the SD card's entire Navigation folder", output.getvalue())
        with patch.object(tc, "urlopen", side_effect=AssertionError("network")):
            with redirect_stdout(io.StringIO()) as output:
                self.assertEqual(tc.main(["--verify-sd", str(self.output)]), 0)
        self.assertIn("VERIFIED:", output.getvalue())

    def test_cli_offline_reference_directory(self):
        reference = self.root / "reference"
        (reference / "TC").mkdir(parents=True)
        (reference / "TC/cecmd.exe").write_bytes(self.commander)
        with patch.object(tc, "urlopen", side_effect=AssertionError("network")), redirect_stdout(io.StringIO()):
            self.assertEqual(tc.main(["--source-root", str(self.source), "--output", str(self.output),
                                      "--upstream-dir", str(reference)]), 0)

    def test_cli_verify_failure_returns_nonzero(self):
        with redirect_stderr(io.StringIO()) as output:
            self.assertEqual(tc.main(["--verify-sd", str(self.source)]), 1)
        self.assertIn("ERROR:", output.getvalue())

    def test_cli_network_failure_leaves_source(self):
        with patch.object(tc, "urlopen", side_effect=URLError("offline")), redirect_stderr(io.StringIO()):
            self.assertEqual(tc.main(["--source-root", str(self.source), "--output", str(self.output)]), 1)
        self.assertEqual(self.original.read_bytes(), self.before)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
