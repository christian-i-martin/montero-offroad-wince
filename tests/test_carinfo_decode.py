"""Compile the probe's CarInfo decoder for the build machine and check documented frame layouts."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "probe/src/carinfo.c", ROOT / "tests/c/carinfo_driver.c"]
COMPILER = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")


@unittest.skipIf(COMPILER is None, "no C compiler on this machine")
class CarInfoDecodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "carinfo_driver"
        subprocess.run([COMPILER, "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "probe/src"),
                        "-o", str(cls.binary), *map(str, SOURCES)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_frames(self, *frames):
        out = subprocess.run([str(self.binary), *frames], check=True, capture_output=True, text=True).stdout
        lines = out.splitlines()
        returns = [int(line[4:]) for line in lines if line.startswith("ret=")]
        fields = dict(line.split("=", 1) for line in lines if not line.startswith("ret="))
        return returns, fields

    def test_pressure_and_elevation_frame(self):
        # 0x21 reply: 962 hPa (0x03C2), 428 m (0x01AC), big-endian
        returns, f = self.run_frames("21 04 03 c2 01 ac")
        self.assertEqual(returns, [2])
        self.assertEqual((f["pressure_hpa"], f["elevation_m"]), ("962", "428"))

    def test_elevation_is_signed(self):
        _, f = self.run_frames("24 02 ff f6")
        self.assertEqual(f["elevation_m"], "-10")

    def test_compass_is_one_based_eight_points(self):
        _, f = self.run_frames("11 01 08")
        self.assertEqual(f["compass"], "8")
        self.assertIn("Compass NW", f["summary"])

    def test_outside_temperature_is_signed_byte(self):
        _, f = self.run_frames("31 02 fb 00")
        self.assertEqual(f["outside_c"], "-5")

    def test_battery_comes_from_strada_frame_in_tenths(self):
        # 0x40: service distance little-endian, month, battery 142 = 14.2 V
        _, f = self.run_frames("40 04 34 12 06 8e")
        self.assertEqual((f["service_dist"], f["service_month"], f["battery_dv"]), ("4660", "6", "142"))
        self.assertIn("14.2 V", f["summary"])

    def test_full_rv_meter_frame_with_trip_block(self):
        frame = ("00 12" " 03" " 03 c2" " 01 ac" " 1e" " 00"
                 " 00 30 00 00 00 69 01 a4 ff ff 01")
        returns, f = self.run_frames(frame)
        self.assertGreater(returns[0], 0)
        self.assertEqual(f["compass"], "3")
        self.assertEqual(f["outside_c"], "30")
        self.assertEqual((f["avg_speed_kmh"], f["avg_fuel_dkmpl"], f["range_km"]), ("48", "105", "420"))
        self.assertNotIn("service_dist", f)  # 0xFFFF means no data
        self.assertEqual(f["service_due"], "1")

    def test_rv_meter_frame_without_trip_block(self):
        _, f = self.run_frames("00 07 01 03 c2 01 ac 1e 00")
        self.assertEqual((f["compass"], f["pressure_hpa"]), ("1", "962"))
        self.assertNotIn("avg_speed_kmh", f)

    def test_no_data_markers(self):
        _, f = self.run_frames("41 0b 00 ff 00 00 ff ff ff ff ff ff 00")
        for key in ("avg_speed_kmh", "avg_fuel_dkmpl", "range_km", "service_dist"):
            self.assertNotIn(key, f)

    def test_outside_temperature_reply_seen_on_the_car(self):
        # probe run 3: "31 02 1e 00 ae", trailing byte is 0xFF minus the sum of the others
        _, f = self.run_frames("31 02 1e 00 ae")
        self.assertEqual(f["outside_c"], "30")

    def test_lights_status_frame(self):
        # probe run 3: headlights on -> "33 01 04", off -> "33 01 00"
        _, f = self.run_frames("33 01 04")
        self.assertEqual(f["lights"], "4")
        self.assertIn("Lights 04", f["summary"])
        _, f = self.run_frames("33 01 04", "33 01 00")
        self.assertEqual(f["lights"], "0")

    def test_short_and_unknown_frames_are_rejected(self):
        returns, f = self.run_frames("21 04 03", "7f 00", "62 01 00")
        self.assertEqual(returns, [-1, -1, 0])
        self.assertEqual(f["frames"], "1")


if __name__ == "__main__":
    unittest.main()
