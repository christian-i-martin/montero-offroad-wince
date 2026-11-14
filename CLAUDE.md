# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Modding the factory head unit of a Mitsubishi Pajero Sport / Montero Sport (3rd generation). The
unit is a Skypine build (variant 6982E, firmware `cne-6982e-a` from 2016) running Windows CE 6 on
ARM. The head unit's Navigation button launches `\SD\Navigation\Navigation.exe` (configured in
`Bin/SP_MAIN.ini`, `[NAVIGATION] PATH=`), which is the only entry point we control. The end goal is
to replace that executable with an off-road dashboard built from the data the unit already has
(elevation, pressure, compass, battery, outside temperature, lights, GPS). A/C and fan speed are not
wanted; steering angle, yaw, pitch and roll are set aside for now (they need extra hardware).
The current milestone ships Total Commander/CE as
`Navigation.exe` so the internal filesystem can be browsed and dumped.

## Repository layout

The repository root mirrors the SD card root, plus reference material:

- `Navigation/`, `Navigator/`, `GenieTek/`, `autokit/`: SD card folders. The committed
  `Navigation/` is the original AutoKit (CarPlay) snapshot, kept as a reference; the working copy may
  already hold Total Commander instead. Do not treat it as the build output.
- `Bin/`: copies of the head unit's `\NandFlash\Application\Bin` (`Main.exe`, `SkCom.dll`, ...).
- `Infor/`: copy of `\NandFlash\Application\Infor` (`CarInfo.exe` is the factory Car Info screen;
  this unit has no `CanInfo.exe`).
- `dumps/`: other files copied off the unit (`CarInfoRes*.spp`, `CommonRes.spp`; the DLLs are
  byte-identical to the `Bin/` copies).
- `tools/prepare_total_commander.py`: builds `dist/total-commander-direct/` (gitignored).
- `probe/`: the on-car data-discovery program (C for Windows CE). `src/carinfo.c` (CarInfo frame
  decoder), `src/busring.c` (shared-memory bus reader) and `src/nmea.c` (GPS) are portable C,
  also compiled on the Mac by `tests/test_carinfo_decode.py` and `tests/test_probe_selftest.py`.
  `src/probe.c` is the Windows CE program. The dashboard reuses these three files.
- `dashboard/`: the dashboard (the future `Navigation.exe`). `src/model.c` (what is shown: GPS-first
  elevation and heading with car fallbacks, speeds, histories, trends), `src/render.c` (the
  800 x 480 layout, drawn through a small `Canvas` interface) and `src/sun.c` (sunset) are portable
  C; `src/dash.c` is the Windows CE program (GDI drawing, GPS thread, bus client copied from the
  probe). `fonts/` holds the Barlow TTFs (SIL OFL) shipped next to the exe. The agreed design is
  the design canvas linked in the research doc's "Dashboard" section. `src/map.c` (offline map:
  reads `*.pack` files, composes the view) and `src/pngdec.c` (PNG decoding through the vendored
  `third_party/stb_image.h`) are portable C too.
- `tools/map/`: builds the offline map on the Mac (Luzon by default) from OpenStreetMap data:
  `mtpack.py` (the `.pack` format, shared with `map.c`), `build_map.sh` (planetiler and
  tileserver-gl in Docker, then `build_map.py`, the packer), `make_style.py` and
  `style/montero-dark.json` (our own dark map style). Output `dist/map/Map/`, copied to the SD card
  root as `\SD\Map\`.
- `tools/re/`: reverse-engineering helpers for the head unit files.
- `docs/vehicle-data-research.md`: what vehicle data exists on the unit and how it flows. Read this
  before any dashboard or data-access work, and update it when new findings land.

## Commands

```sh
python3 tools/prepare_total_commander.py                          # build dist/total-commander-direct
python3 tools/prepare_total_commander.py --cecmd /path/cecmd.exe  # offline build
python3 tools/prepare_total_commander.py --verify-sd /Volumes/SD  # read-only check of a mounted card
python3 -m unittest discover -s tests -v                          # all tests
python3 -m unittest tests.test_spp -v                             # one module
python3 -m unittest tests.test_prepare_total_commander.DirectPackageTests.test_complete_navigation_contains_only_total_commander
```

Probe build (CeGCC 9.3 for `arm-mingw32ce` from ENLYZE's Docker image, pinned by digest in the
script; the image is amd64-only and runs emulated on this Apple Silicon Mac):

```sh
./tools/build_probe.sh                      # -> dist/probe/Probe/Probe.exe + README.txt
python3 -m unittest tests.test_carinfo_decode tests.test_probe_selftest -v   # host C compiler
```

The user copies `dist/probe/Probe/` to the SD card root and starts it from Total Commander; the
probe appends to `\SD\Probe\probe-log.txt`, which the user brings back. Logs pasted into the
conversation are the main source of on-car facts; record new facts in the research doc.

The probe imports only `COREDLL.dll` and reaches the vendor DLLs at run time with `LoadLibraryW` and
`GetProcAddressW` using the MSVC-mangled export names. Keep it ARM mode (`-marm -march=armv4t`, the
toolchain default): vendor code calls back into it with `mov lr, pc; ldr pc, [...]`.

Dashboard build and checks (same Docker image):

```sh
./tools/build_dashboard.sh                  # -> dist/dashboard/Navigation/ (adds Total Commander if built)
python3 -m unittest tests.test_dashboard -v # model, sun, drawing calls, SVG previews (host C compiler)
DASHBOARD_PREVIEW_DIR=/some/new/dir python3 -m unittest tests.test_dashboard   # keep the 5 previews
```

The previews go through the same `render.c` with text widths read from the Barlow files, so they
show the real layout; `qlmanage -t -s 1600 -o DIR file.svg` turns one into a PNG (it crops to a
square, so pad the SVG height first). A missing COREDLL import stops a CE exe from loading at all,
so the dashboard looks up functions the probe never imported on the car (`Ellipse`, `RoundRect`,
`Polygon`, `Polyline`, `GetTextFaceW`, `AddFontResourceW`, `CreateDIBSection`) with
`GetProcAddressW` and has fallbacks; compare new imports with the probe's before adding direct calls
(`arm-mingw32ce-objdump -p` in the build image lists them). GCC can add imports on its own:
`malloc` plus `memset` becomes `calloc`, which is why `map.c` clears its cache with a loop and has
its own logarithm.

Offline map (Docker for planetiler and tileserver-gl, both native arm64; about 2 GB of downloads
into `dist/map-build/` the first time; other running containers can starve planetiler of memory):

```sh
tools/map/build_map.sh                                   # Luzon, zoom 8 to 14 -> dist/map/Map
tools/map/build_map.sh --min-zoom 15 --max-zoom 15       # add zoom 15 (separate pack file)
tools/map/build_map.sh --bounds W,S,E,N --output DIR     # another area; must lie inside MAP_VECTOR_BOUNDS
python3 tools/map/build_map.py --count --max-zoom 15     # tile counts only, no Docker
python3 -m unittest tests.test_map_tools tests.test_map_reader -v   # packer, style, C map reader
```

`dashboard_preview FONT_DIR OUT_DIR MAP_DIR LAT LON` (built as in `tests/test_map_reader.py`)
renders the previews with a real map composed by `map.c`.

The Total Commander build refuses to overwrite an existing `--output` directory. On macOS,
`test_verify_is_offline_and_read_only` fails because `verify_sd` resolves the `/var` to
`/private/var` temp-dir symlink; it is a test-side path comparison issue, not a tool bug.

The system `python3` here is 3.9 although the README asks for 3.10+; scripts use
`from __future__ import annotations` so they still run. Keep new code 3.9-compatible.

Reverse-engineering helpers (the disassembler needs capstone and pefile in a virtual environment):

```sh
python3 tools/re/spp.py dumps/CarInfoRes_6982C.spp [NEW_OUT_DIR]   # list or extract a resource pack
python3 tools/re/cestrings.py Bin/Main.exe                         # ASCII + UTF-16 strings with offsets
python3 -m venv .venv && .venv/bin/pip install -r tools/re/requirements.txt
.venv/bin/python tools/re/cedis.py Bin/SkCom.dll --exports CarInfoApi
.venv/bin/python tools/re/cedis.py Bin/SkCom.dll procCarInfoMsg 120  # by export name or RVA (hex)
.venv/bin/python tools/re/cedis.py Bin/Main.exe --xref-str EVENT_CMD_CANBOX
```

## Head unit architecture (details in docs/vehicle-data-research.md)

- `Main.exe` owns the MCU (the unit's microcontroller that talks to the car) on `COM2:` at 115200
  baud and relays data to the other programs over a message bus in `SkCom.dll` (`MsgSession`):
  each app has a hidden window whose class and title are its bus name (from `getDestName`, e.g.
  `NavigRecive` for Main.exe, `CarInfo`, `TerminalA`; not the `App...` names); message bodies go
  into a ring buffer in the `SkIPCMsgShareMem` shared memory and the window gets `0x800A`
  (wParam = offset, lParam = length). Every bus message passes through that ring.
- Screen programs stay resident; the factory `CarInfo` program is always running, so its identity
  (0x14) cannot be taken. A program joins with `MsgSession::config(id, getDestName)` +
  `MsgSession::init(handler)` under a free id; the probe uses `TerminalA` (0x1A) through
  `SkCom.dll`'s exports, with a C struct standing in for the C++ `IMsgProc` handler.
- Compass, pressure, elevation, outside temperature (`31 00`), battery voltage and lights status
  (`0x33`) come through the CarInfo channel (message 0x4A, Bundle key `"data"`, frames
  `[command][length][payload]`); the trip computer (`41 00`, `00 00`) is never answered on this car. `Main.exe` routes each incoming message by its sender's app id:
  requests with sender 0x1A were ignored on the car, so the probe switches its sender to 0x14
  (`MsgSession::config`) just for each post. Replies always go to the `CarInfo` window (and are
  mirrored as message 0x4B8), so other programs read them from the ring. The scaling of every field
  is decoded from `Infor/CarInfo.exe`.
- Bundles serialize as `"BNDL"`, entry count, then key / type (1 int, 4 string, 5 bytes) / value;
  `probe/src/busring.c` parses them. `Main.exe` pings every bus window with 0x6D3 about every 6 s
  and registered apps answer 0x6D4.
- GPS is a SiRF receiver on `COM1:` at 38400 baud (NMEA, 1 Hz). Climate data is decoded inside
  `Main.exe` and never appears on the bus; the probe reads the fan level read-only from
  `Main.exe`'s memory (`[0xbe4b8]` → popup, `+0xe4` → fan bar, `+0x7C` → value), only after checking
  the `Main.exe` file size, and only trusts in-range values; nothing on the unit reads steering angle, yaw, pitch,
  roll, RPM or coolant. No USB serial driver is installed (HID, mass storage, Bluetooth dongles are).
- `Main.exe` shows and hides the navigation app with `FindWindowW(NULL, "GPS - Navigation")` +
  `ShowWindow`, so the dashboard's main window gets that title and it must not join the bus as Navi.

## Working with the binaries

- The Skypine binaries are 32-bit ARM code (machine 0x1c2, `file` reports "ARM Thumb" but the code
  is ARM mode), built with MSVC 2008. C++ export names are MSVC-mangled (`?method@Class@@...`); the
  macOS `c++filt` cannot demangle them, read them as-is. UI classes come from JUCE plus Skia
  (`SkGUI.dll`).
- `COREDLL.dll` is imported by ordinal (for example 168 `CreateFileW`, 286 `FindWindowW`, 865
  `PostMessageW`).
- Device names, window titles and paths are usually UTF-16, so plain `strings` misses them; use
  `tools/re/cestrings.py`.
- `.spp` files are resource packs (see `tools/re/spp.py`); screen layouts inside them are UTF-16
  `.ini` files listing every control (`ComponentName=`).
- Never execute the dumped binaries. Write extracted or scratch output to a new directory outside the
  repo unless it is meant to be kept.

## Safety rules for anything that runs on the head unit

From the README and the research so far:

- Never modify internal storage (`\NandFlash`), the registry, firmware, `Navigator/` or `GenieTek/`.
  Only the SD card's `Navigation` folder is replaced, as a whole folder.
- Never write to vehicle ports (`COM2:` MCU, `COM3:` Bluetooth, `COM5:` disc) or I2C devices.
- When talking to `Main.exe`, send only read requests. CarInfo commands such as `12` (compass
  calibration), `15` (compass offset), `22`/`23` (pressure reference), `42`-`44` (resets), `62`-`6C`
  (tyre learning) and `6D` (test mode) change state on the unit. Never send `81 01 00`
  (EndCommunication): it would stop the resident Car Info screen's updates. Do not send `0x6D1`
  (register) or `0x6D4` (heartbeat answer); `Main.exe` records them in its app table and the probe
  is deliberately not a registered app.
- Never take an identity whose bus window already exists (check `FindWindowW(name, name)` first).
- Do not log GPS position (the user shares logs in conversation); speed, course and altitude are fine.
- Nothing has been tested on the hardware unless the docs say so; keep claims about runtime
  compatibility explicit.
