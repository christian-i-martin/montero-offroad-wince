"""Compile and run the C self-test for the probe's bus ring reader and NMEA parser."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")


@unittest.skipIf(COMPILER is None, "no C compiler on this machine")
class ProbeSelfTest(unittest.TestCase):
    def test_ring_and_nmea(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "probe_selftest"
            subprocess.run([COMPILER, "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "probe/src"),
                            "-o", str(binary), str(ROOT / "tests/c/probe_selftest.c"),
                            str(ROOT / "probe/src/busring.c"), str(ROOT / "probe/src/nmea.c")], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
