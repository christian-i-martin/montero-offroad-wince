"""Compile and run the dashboard's portable code on the build machine.

The self-test covers the model rules (GPS-first elevation and heading, speeds, trends, battery),
sunrise and sunset, and the drawing calls. The preview test renders five screens to SVG through
the same render.c the head unit uses; set DASHBOARD_PREVIEW_DIR to keep them for a look.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
SOURCES = ["dashboard/src/model.c", "dashboard/src/render.c", "dashboard/src/sun.c",
           "dashboard/src/map.c", "dashboard/src/pngdec.c", "dashboard/src/addr.c",
           "probe/src/nmea.c", "probe/src/carinfo.c"]
SCENES = ["driving.svg", "parked-night.svg", "no-gps.svg", "startup.svg", "menu.svg"]


def build(main, binary):
    subprocess.run([COMPILER, "-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                    "-I", str(ROOT / "dashboard/src"), "-I", str(ROOT / "probe/src"),
                    "-o", str(binary), str(ROOT / main)] + [str(ROOT / s) for s in SOURCES] + ["-lm"],
                   check=True)


@unittest.skipIf(COMPILER is None, "no C compiler on this machine")
class DashboardTests(unittest.TestCase):
    def test_selftest(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "dashboard_selftest"
            build("tests/c/dashboard_selftest.c", binary)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_previews_render(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(os.environ.get("DASHBOARD_PREVIEW_DIR") or Path(temp) / "previews")
            out.mkdir(parents=True, exist_ok=True)
            binary = Path(temp) / "dashboard_preview"
            build("tests/c/dashboard_preview.c", binary)
            result = subprocess.run([str(binary), str(ROOT / "dashboard/fonts"), str(out)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in SCENES:
                root = ET.parse(out / name).getroot()
                self.assertEqual(root.get("width"), "800")
                self.assertEqual(root.get("height"), "480")
                texts = [t.text for t in root.iter("{http://www.w3.org/2000/svg}text")]
                self.assertTrue(texts, name)


if __name__ == "__main__":
    unittest.main()
