# Vehicle data on the head unit: research notes

Status (last updated 2026-11-14): static analysis of the files in `Bin/`, `Infor/` and `dumps/`, plus
three probe runs on the car ("Probe run 1" to "Probe run 3" below). Each finding is marked:

- **Decoded**: read directly from the program code. Byte layouts, ids and scaling are exact.
- **Seen**: observed on the car by the probe.
- **Inferred**: deduced from names, screen layouts or patterns. Likely but unproven.
- **Unknown**: not answered yet.

Firmware: Skypine build `cne-6982e-a`, built 2016-01-11 (from debug paths in the binaries). The
active configuration `Bin/SP_MAIN.ini` has `CODE=0HS3`, the same as `SP_MAIN_6982E.ini`, so this unit
is the 6982E variant. The factory Car Info screen is `Infor/CarInfo.exe` version 2.0.28.

## Short answer

| Dashboard value | On this unit? | Source | Status |
|---|---|---|---|
| Elevation | Yes | CarInfo `0x21` (also `0x00`) | Seen: 200 m (barometric; GPS said 279 m) |
| Barometric pressure | Yes | CarInfo `0x21` (also `0x00`) | Seen: 988 hPa |
| Compass direction | Yes, coarse | CarInfo `0x11` (also `0x00`) | Seen: 4 = SE; 8 points only |
| Outside temperature | Yes | CarInfo `0x31`, request `31 00` | Seen: 30 °C |
| Battery voltage | Yes | CarInfo `0x40`, pushed every 2 to 5 s | Seen: 13.6 to 14.1 V with the engine running |
| Lights status | Yes | CarInfo `0x33`, pushed on change | Seen: headlights on = `0x04` |
| Average speed, average fuel economy, range | No answer | CarInfo `0x41` / `0x00` trip block | `41 00` and `00 00` are never answered on this car |
| Service reminder | Yes | CarInfo `0x40` | Seen: 1486 (probably km) or 7 months; not wanted for the dashboard |
| Tyre pressure and temperature | Probably not | CarInfo `0x60` to `0x6D` | The factory screen's `61 01 00` got no answer in run 2 |
| A/C fan speed | Probably | `Main.exe`'s memory (popup object) | Decoded: level 0 to 8; not wanted for the dashboard |
| Vehicle speed | Yes, from GPS | GPS; the CAN box might also carry it | GPS seen; CAN box unknown |
| GPS heading, speed, altitude | Yes | SiRF receiver on `COM1:`, 38400 baud, NMEA | Seen |
| Steering (tyre) angle, yaw rate | **Not found** | Nothing on the unit reads them | Rough GPS estimate while moving; real values need extra hardware |
| Pitch and roll | **Not found** | | Pitch: rough slope from elevation change while moving; roll needs a motion sensor |
| Engine RPM, coolant temperature | **Not found** | | Needs an OBD-II adapter |

"Not found" means: no program, library or screen resource on the unit mentions it, and the
probe's driver list has no motion-sensor driver. The only "angle" and "tilt" names (`KEY_ANGLE`,
`KEY_TILT`, `ScreenAngle`) are for tilting the screen, and `SteeringWheel` is the steering-wheel
buttons setup. `CanInfo.exe` (the CAN-box screen) and `AC\AC.exe` are not installed on this unit,
and no code in `Main.exe` builds the CAN-box message id `0xBE54` as a constant, so raw CAN-box data
is probably not forwarded to any app. Probe run 3 confirmed it: turning the steering wheel produced
no bus message and no CarInfo frame. See "Without extra hardware" for what can still be estimated.

The factory compass has only 8 directions (45-degree steps). For a smooth heading while moving, use
GPS course.

## How data flows on the stock unit

```text
 car CAN bus ──> CAN box ──┐
 compass / pressure sensor ┤   (wiring not confirmed: inside or attached to the MCU)
                           v
                 MCU (the unit's small microcontroller that talks to the car)
                           │  serial port COM2:, 115200 baud
                           v
                 Main.exe  (\NandFlash\Application\Bin)
                           │  owns COM2, decodes MCU events, shows volume and climate popups
                           │  message bus between programs (window messages + shared memory)
          ┌────────────────┼──────────────────┐
          v                v                  v
   Infor\CarInfo.exe   (other apps)       our program
   app id 0x14,        all resident       joins under its own id, sends requests with
   always running                         arg1 = 0x14, reads replies from the shared ring
```

- The MCU serial port is fixed in code: `SkypineConfig::getMcuUartName()` returns `COM2:` and
  `getMcuUartBandRate()` returns 115200 (**Decoded**, `SkCom.dll`). The disc drive is on `COM5:` and
  Bluetooth on `COM3:`. `Main.exe` keeps COM2 open, and Windows CE serial ports normally allow only
  one program at a time, so a dashboard has to go through `Main.exe`, not the port.
- `Main.exe` turns MCU traffic into numbered events. Event `0x12` is `EVENT_CMD_CANBOX`, `0xC6` is
  `EVENT_CMD_RX_CAN_CMD` (**Decoded**, name table at `Main.exe` RVA `0xad00`).
- Screens are separate programs under `\NandFlash\Application\`. On this unit `Infor\` holds
  `CarInfo.exe`, `Antitheft.exe`, `EQ.exe`, `Standby.exe` and `Wifi.exe` only.
- Most screen programs stay running in the background with hidden windows (**Seen**: CarInfo,
  MainMenu, Bluetooth, Disc, iPod, Telematics). In particular the factory Car Info program is always
  running, so nothing else can take its bus identity.

### App ids and bus window names (Decoded, `SkCom.dll` `getDestName` at RVA `0x11b8`)

The bus window of each app has this name as both class and title. `Main.exe`'s own bus window is
`NavigRecive` (sic). The Navi name is read from `SP_MAIN.ini` `[NAVIGATION] TITLE` at run time.

| Id | App | Window | Id | App | Window |
|---|---|---|---|---|---|
| 0x00 | AppMain | `NavigRecive` | 0x10 | AppHDMI | `HDMI` |
| 0x01 | AppMainMenu | `MainMenu` | 0x11 | AppPandora | `Pandora` |
| 0x02 | AppSetup | `Setup` | 0x12 | AppHDRadio | `HDRadio` |
| 0x03 | AppAntitheft | `Antitheft` | 0x13 | AppRadar | `Radar` |
| 0x04 | AppStandby | `Standby` | 0x14 | AppCarInfo | `CarInfo` |
| 0x05 | AppRadio | `Radio` | 0x15 | AppWifi | `Wifi` |
| 0x06 | AppUSB | `USB` | 0x16 | AppTelematics | `Telematics` |
| 0x07 | AppiPod | `iPod` | 0x17 | AppDAB | `DAB` |
| 0x08 | AppDisc | `Disc` | 0x18 | AppAC | `SWC` |
| 0x09 | AppAuxIn | `AuxIn` | 0x19 | AppSWC | `SWC` |
| 0x0A | AppSirius | `Sirius` | 0x1A | AppThirdPart | `TerminalA` |
| 0x0B | AppDVBT | `DVBT` | 0x1B | AppCarDVR | `CarDVR` |
| 0x0C | AppCamera | `Camera` | 0x1C | AppCanInfo | `CanInfo` |
| 0x0D | AppBluetooth | `Bluetooth` | 0x1D | AppALink | `ALINK` |
| 0x0E | AppEQ | `EQ` | 0x1E | AppCarPlay | `CarPlay` |
| 0x0F | AppNavi | `GPS - Navigation` (from SP_MAIN.ini) | 0x1F | AppUnknown | empty |

## The message bus (Decoded)

`MsgSession` in `SkCom.dll` (source file `MsgSession_win32.cpp`):

- **Windows.** Each app creates a hidden top-level window whose class name and title are both its
  app name (`RegisterClassW` + `CreateWindowExW`). Senders find it with
  `FindWindowW(name, name)`. Names come from the exported function `getDestName(id)`.
- **Shared memory.** Message bodies go into a 256 KB (`0x40000`) named shared memory
  (`SkIPCMsgShareMem`, via SkGUI's `CSPShareMemory`), used as a ring buffer under a JUCE
  `InterProcessLock` (`SkIPCMsgMutex`). Every message between every pair of apps passes through
  it, so a program that maps it can read all bus traffic.
- **Ring layout.** Header at offset 0: `u32` size (`0x40000`), `u32` start (`0x10`), `u32` write
  offset, `u32` end. Records from the start offset: `"msg\0"`, `u16` length, the serialized
  message, `u16` checksum (16-bit sum of the message bytes); all little-endian. A writer that cannot
  fit a record restarts at the start offset. The writer moves the write offset under the lock and
  copies the record afterwards, so the newest record may still be incomplete
  (`SkCom.dll` writer RVA `0x1cf30`, setup `0x1d3b4`; reader in `probe/src/busring.c`).
- **Notification.** The sender posts window message `0x800A` (post) or `0x800B` (send) with
  `wParam` = offset of the record in shared memory and `lParam` = length. The serialized message
  starts 6 bytes after the offset. Plain `WM_COPYDATA` (`0x4A`) is also accepted.
- **Message format.** `RemoteMessage` serializes as: magic `RMSG` (`0x47534D52`), message id, arg1,
  arg2, sender id (each a 32-bit int), then the attached object (a `Bundle`, a key/value container).
  In memory: id at `+0x08`, arg1 `+0x0C`, arg2 `+0x10`, object `+0x14`, sender `+0x18`. A `Bundle`
  serializes as a flags int and a JUCE `var` map; the map's type tags are not decoded, so the probe
  lets `SkCom.dll` rebuild messages (`Parcel::obtain`, `fillData`, `createFromParcel`).
  Primitive encodings: int = 4 bytes, byte array = int length + bytes, string = int (length + 1) +
  UTF-8 with the terminating zero.
- **Receiving.** The window procedure (`SkCom.dll` RVA `0x1e3e0`) rebuilds the message and calls the
  app's `IMsgProc` handler: vtable slot 0 destructor, slot 1 `procOwnerMsg(message)`, slot 2
  `procUnknownMsg(msg, wParam, lParam)`.
- **Joining.** The factory apps do, in `MainServiceManager`'s constructor:
  `MsgSession::config(own id, getDestName)`, then `MsgSession::init(handler)` (creates the window and
  maps the shared memory), then post message `0x6D1` to `AppMain` with arg1 = own id; `Main.exe`
  answers `0x6D2` with arg1 = that id (**Seen**).
- **System info (Decoded and Seen).** `Main.exe` stores system state with
  `SP_SetSysInfo(index, value)` (SkShareMem, a shared table of at least 0x45 ints readable with
  `SP_GetSysInfo`) through one helper (RVA `0x1aa10`, 93 call sites), which also broadcasts
  `PostMessageW(HWND_BROADCAST, 0x52A, index, value)` to every top-level window. Seen: index 9 =
  lights (1 on, 0 off), index 8 = reverse / rear camera, index 0x41 also changes with page
  switches. Decoded: index 0 = Bluetooth connection state (see "Bluetooth").
- **Heartbeat (Seen).** `Main.exe` sends `0x6D3` to every bus window about every 6 seconds; each
  registered app answers `0x6D4` with arg1 = its own id. `Main.exe` records `0x6D1` and `0x6D4` in
  its app-state table (RVA `0x542cc`), so `0x6D4` means "still alive", not "stopped". The probe
  never registers and never answers.
- **Bundle format (Seen).** After the 20-byte message prefix: `"BNDL"`, `u32` entry count, then per
  entry `u32` key length (including the zero), the key, `u32` type and the value. Type 1 = int,
  type 4 = string (`u32` length including the zero, then the text), type 5 = byte array (`u32`
  length, then the bytes). Then `u32` 0. Parsed by `busring_bundle_entries` in
  `probe/src/busring.c`.
- **Sending.** `MessageHelper::postMessage(destination id, message id, arg1, arg2, object)`.
  CarInfo requests use destination `AppMain` (0), message `0x4A`, arg1 `0x14`, arg2 0, and a
  Bundle with the frame bytes under the key `"data"`.
- **Routing in `Main.exe`.** `Main.exe`'s `RemoteSessionManager` (RVA `0x61fdc`) first takes an
  app type from each incoming message (logging `MSG_REMOTE_MSG_REV AppType error` when it is out of
  range) and hands the message to that app's handler. In the CarInfo handler (RVA `0x7a358`) an
  incoming `0x4A` with arg1 `0x14` sends the `"data"` bytes to the MCU as command group `0x3A`;
  arg1 `0x1C` goes to the CAN-box path. **The sender id matters (Seen):** in probe run 2, 209
  requests sent with sender `0x1A` and arg1 `0x14` got no answer, while the identical requests
  from the factory program (sender `0x14`) were answered within about 150 ms. Probe 0.3 therefore
  switches its sender to `0x14` (`MsgSession::config`) for the duration of each post. The MCU's
  replies are always posted to app `0x14` (`post(0x14, 0x4A, 0, 0, bundle)` at RVA `0x5954c`) and
  mirrored as message `0x4B8` with the same `"data"` bundle, so other programs read them from the
  shared ring.

Calling convention notes for code that calls these MSVC-built functions: arguments in r0 to r3 then
the stack; `this` in r0; a function returning an object by value takes a hidden result pointer in
r0 (static functions) or r1 (member functions, after `this`). `juce::String` is one pointer and
is built with SkGUI's `??0String@juce@@QAA@PBD@Z`.

## CarInfo protocol (Decoded from `SkCom.dll` and `Infor/CarInfo.exe`)

Frames are `[command][second byte][payload...]`. The second byte is usually the payload length.
Frames coming from the MCU carry one extra byte after the payload (**Seen**). For `0x11` and
`0x21` it equals `0xFF` minus the 8-bit sum of the frame's other bytes, a checksum; for `0x40` it
often does not match, and `0x33` frames have none. The decoder ignores it.
Requests and their replies use the same command byte. `CarInfoApi::procCarInfoMsg` reads values
from byte 2 onward and hands each one to the screen as `callback(id, value)`; `CarInfo.exe`'s
handler (RVA `0x17abc`) turns ids into text. 16-bit values are big-endian (high byte first),
except in command `0x40`. "RV meter" is Mitsubishi's name for the off-road display (compass,
altimeter, barometer, thermometer).

### What the factory screen sends

When its page is shown, `CarInfo.exe` starts a 5-second timer (RVA `0x13cc8`); when it fires
(RVA `0x10214`) it sends `81 01 01` (StartCommunication), then one second later `61 01 00`
(tyres), `11 00` (compass), `21 00` (pressure and elevation) and `A0 02 40` (service and battery).
`00 00` (RV meter) is sent when that page is opened, `81 01 00` (EndCommunication) when Main.exe
tells it to close (RVA `0x786c`). `31 00` and `41 00` exist in `SkCom.dll` but this `CarInfo.exe`
never sends them. Other programs should not send `81 01 00`: it would stop the factory screen's
updates.

### Requests (Decoded, exact bytes)

| Method | Bytes | Notes |
|---|---|---|
| GetRVMeter | `00 00` | full off-road frame |
| ReqCompass | `11 00` | |
| ReStudyCompass | `12 00` | starts compass calibration: do not send |
| ExitCompass | `13 00` | |
| GetCompassOffset | `14 00` | |
| SetCompassOffset | `15 01 nn` | changes settings: do not send |
| GetAirPressureAndElevation | `21 00` | |
| SetAirPressure | `22 02 hi lo` | changes the pressure reference: do not send |
| ReSetAirPressure | `23 00` | resets the pressure reference: do not send |
| ChangeElevation | `24 00` (argument 0) or `25 00` | |
| GetOutTemperature | `31 00` | not used by this CarInfo.exe |
| GetCanData | `41 00` | not used by this CarInfo.exe |
| ClearMaintenanceHint | `42 00` | do not send |
| ResetMiddleOilExpend | `43 00` | resets average fuel economy: do not send |
| ResetMiddleCarSpeed | `44 00` | resets average speed: do not send |
| GetCurrSysPressureMode / Set | `58 00` / `58 mm` | second byte is the mode, not a length |
| GetAllWheelData | `61 01 00` | tyre data |
| Tyre learning and rotation | `62` to `6C` with `01 xx` | do not send |
| EnterTestMode | `6D 01 00` | do not send |
| GetMaintainInfo | `A0 02 40` | reply is command `0x40` (service and battery) |
| StartCommunication / EndCommunication | `81 01 01` / `81 01 00` | sent by the factory screen on start and exit |

### Replies and what they mean (Decoded)

`p0`, `p1`, ... are payload bytes (frame byte 2 onward). `p1-2` is a 16-bit value with `p1` as the
high byte. "id" is the callback id; the name in capitals is `CarInfo.exe`'s own log name.

| Command | Field | Id | Meaning and scaling |
|---|---|---|---|
| `00` | `p0` | 0x00 | compass, 1..8 = N, NE, E, SE, S, SW, W, NW (0 shows N) |
| `00` | `p1-2` | 0x06 | AIR_PRESSURE, whole hPa |
| `00` | `p3-4` | 0x07 | ELEVATION, signed, metres |
| `00` | `p5` | 0x08 | OUT_TEMPERATURE, signed byte, whole degrees |
| `00` | `p6` | 0x09 | ignored by the screen |
| `00` | `p7` onward | 0x0A..0x10 | trip block, below (present only if the frame is long enough) |
| `10`, `11` | `p0` | 0x00 | compass |
| `12` | `p0` | 0x01 | calibration state |
| `14` | `p0` | 0x03 | compass offset (magnetic variance zone) |
| `20`, `21` | `p0-1`, `p2-3` | 0x06, 0x07 | pressure, elevation |
| `23` | `p0-1` | 0x06 | pressure |
| `24`, `25` | `p0-1` | 0x07 | elevation |
| `30`, `31` | `p0` | 0x08 | outside temperature (`p1` → 0x09, ignored); seen `31 02 1e 00 ae` = 30 °C |
| `33` | `p0` | none | lights status bits, pushed by the MCU; not used by any factory program. Seen `0x04` with headlights on, `0x02`/`0x03` in run 2 (cause unknown) |
| `40` | `p0 + p1×256` | 0x0F | MAINTENANCE_ODOGRAPH, service distance (`0xFFFF` = none) |
| `40` | `p2` | 0x4D | MAINTENANCE_MONTH (`0xFF` = none) |
| `40` | `p3` | 0x4B | CAR_BATTERY, tenths of a volt (142 = 14.2 V) |
| `40` | (derived) | 0x10 | service due when the distance is 2 or less |
| `41` | `p0` onward | 0x0A..0x10 | trip block, below (never answered on this car) |
| `42`, `43`, `44` | none | 0x11..0x13 | reset acknowledgements |
| `58` | `p0` | 0x4E | pressure unit or mode |
| `60`, `61`, `6D` | | 0x41..0x46 | five tyre values (wheel order inferred: FL, FR, RR, RL, spare), 0x46 sensor battery |

**Trip block** (`s0` is its first byte): `s1` → 0x0B MIDDLE_CAR_SPEED, km/h (`0xFF` = none);
`s4-5` → 0x0D MIDDLE_OIL_EXPEND, tenths of km/L (`0xFFFF` = none); `s6-7` → 0x0E CONTINUE_STEER,
range in km (`0xFFFF` = none); `s8-9` → 0x0F service distance; bit 0 of `s10` → 0x10 service due.
`s0` (0x0A) and `s2-3` (0x0C) are not shown by the factory screen.

These layouts are implemented in `probe/src/carinfo.c` and covered by `tests/test_carinfo_decode.py`.

### Tyre pressure (TPMS) (Decoded, partly Seen)

- Tyre frames would arrive on the same path as compass and pressure: `Main.exe` names the MCU event
  that carries all CarInfo frames (`0x3A`) `EVENT_CMD_TX_TPMS_DATA`. The radio receiver, if there is
  one, sits behind the MCU; no Windows CE program talks to it directly.
- The only tyre commands (`SkCom.dll` `CarInfoApi`) are: read all wheels (`GetAllWheelData`,
  `61 01 00`), pairing ("wheel study": `EnterWheelStudy`, `SkipCurWheelEnter{FL,FR,RL,RR,Spare}Study`,
  `DeleteCurWheelToNextStudy`, `ExitWheelStudy`), `EnterWheelPressureStudy` /
  `ExitWheelPressureStudy`, and wheel rotation. There is no frequency or sensor-type setting
  anywhere: no `315`, `433` or `MHz` in `SkCom.dll`, `CarInfo.exe`, `Main.exe` or `SkShareMem.dll`.
  `SkShareMem.dll` holds settings named `TPMSEnable`, `TPMSSettings` and `TPMSPower`, and the
  factory screen has a TPMS page (`BtnGotoTPMS`, sensor battery icons) that it presumably shows only
  when the feature is enabled.
- Seen: in run 2 the factory screen sent `61 01 00` along with `11 00`, `21 00` and `A0 02 40`;
  the other three were answered, the tyre request was not. So this unit most likely has no tyre
  receiver connected, or the feature is off.
- The receiving frequency (315 MHz or 433.92 MHz, depending on the market) is fixed by the receiver
  hardware, and the sensor data format by its firmware, so it cannot be changed from the head unit.
  Pairing only chooses which sensor ids it accepts.

## Climate

Not wanted for the dashboard; kept for reference.

`Main.exe` contains a climate popup (`aircontrolbar.bin`, window `AirControlBar` seen on the car)
with `StAcOnOff`, `PbFanSpeed`, `StAuto`, `StWindMode`, `StTemp`, `StTempRight`, `StAir`
(recirculation) and `StDemister`, so it decodes climate frames from the CAN box itself. There is no
`AC` folder on this unit and `AppAC`'s bus window name is `SWC`. In probe runs 2 and 3 the fan was
changed while every bus message was logged, and no climate message appeared (**Seen**). The user
confirms the popup does appear on screen when the fan or A/C changes; the probe never saw the
`AirControlBar` window become visible while it was in front.

**How climate reaches the popup (Decoded).** `Main.exe` calls its popup directly (function at RVA
`0x43a1c`, message `0x53B` with a kind and a value); nothing is posted to any other program. The
popup object is created on the first climate change and its pointer is kept at VA `0xbe4b8`
(RVA `0xae4b8`). The handler (RVA `0x42aa4`) switches on the kind:

| Kind | Value | Effect |
|---|---|---|
| 0, 1 | temperature (shown in 0.5 degree steps) | stored in the object at `+0x100` and `+0x108` |
| 7 | switch bits (bit 6 A/C on, bits 5 and 3 other switches) | icons only |
| 8 | low 4 bits = **fan level 0 to 8**; bits 5 to 7 = demister / air-flow flags | fan bar (`PbFanSpeed`, `CSPProgressBar` at object `+0xe4`); bit 7 also copied to VA `0xbe51c` |

`CSPProgressBar::setCurrentValue` (SkGUI) keeps the minimum at `+0x74`, the maximum at `+0x78` and
the current value at `+0x7C`. So the fan level can be read with `ReadProcessMemory` from
`Main.exe`: `[0xbe4b8]` → object, `[object + 0xe4]` → bar, `[bar + 0x7C]` → fan level. Probe 0.4
does exactly that, read-only, after checking that `Main.exe` is the 791,040-byte build and that
the values are in range. The value is unknown until the first climate change after the head unit
starts.

## Now playing (Decoded, partly Seen)

Each media app reports what it plays with `sendToInfoBar` (`SkCom.dll`), which posts bus message
`0x4A0` (arg1 3) to `Main.exe` with a Bundle. Like every bus message it passes through the shared
ring, so the dashboard can read it without sending anything.

| Sender | Bundle class | Keys |
|---|---|---|
| USB/SD music, disc, iPod, Pandora, Bluetooth music | `InfoMedia` (`enBundle` RVA `0x7900`) | `SourceID`, `RefreshFlag`, `PlayState`, `TotalNum`, `CurNum`, `DiscType`, `FileType` (ints); `Title`, `Artist`, `Album`, `InforText` (strings) |
| Radio, HD Radio | `InfoTuner` (`0x6790`) | `SourceID`, `Action` (ints); `InforText`, `BandText` (strings, probably frequency and band) |
| Bluetooth phone | `InfoBT` (`0x7490`) | `SourceID`, `Action` (ints); `InforText` (string) |
| Satellite radio | `InfoSatRadio` | `SourceID`, `ChannelName`, `Artist`, ... |

Bundle strings are UTF-8. `USBApi::sendToInfoBar` sets the info's type field to 10 or 7 depending
on its flag argument (probably video or music). There is no playback position, so a progress bar
would have to be timed by the dashboard. Seen: run 2 logged `0x4A0` from the Bluetooth app (`0x0D`)
and `0x496` (`rearSrc`, `normalSrc`, `frontSrc`), which looks like the current audio source. Not
known yet: the `PlayState` codes, how often `0x4A0` is sent, and whether Bluetooth music carries
title and artist (that depends on the phone sending them). No probe run had music playing.

`SourceID` uses `Main.exe`'s source list (**Decoded**, name function at RVA `0xa194`): 0 tuner,
1 disc, 7 SD, 9 iPod, 10 USB, 13 Bluetooth audio, 20 Bluetooth phone, 21 Pandora, 29 CarPlay,
30 none. Seen: `0x4A0` from the Bluetooth app with `SourceID` 20, and `0x496` arg1 7 with
`SourceID` 13 or 30.

### Media keys (Decoded)

`Main.exe`'s `KeyManager` takes keys from the MCU (event `0x1F`, two bytes: key and value) and
from its own panel and InfoBar code, and hands each key to the handler of the program concerned.
About 28 handlers share one method (RVA `0x4a340`) that posts bus message `0x4AB` with arg1 = key
and arg2 = key state to that program; one handler (RVA `0x4b6ec`, probably Bluetooth's) keeps the
phone keys (`0x41` tel, `0x76` BT, `0x5E` send, `0x5F` end) for itself. `SkCom.dll`'s shared receive code (RVA `0xa5cc`)
passes `0x4AB` to the program's command callback without looking at the sender. Key numbers (name
function at RVA `0xa370`): 1 mute, 2 play, 3 next, 4 previous, 6 source, `0x2C`/`0x2D` rewind and
fast forward, `0x2F` shuffle, `0x30` repeat, `0x74`/`0x75` steering-wheel previous and next; the
InfoBar's buttons are `0x10001` previous, `0x10002` next, `0x10003` play/pause, `0x1000B` mute,
`0x1000C` repeat. No message was found that asks `Main.exe` itself to act on a key from another
program, and `SystemHelper` has no key call (`showCurrSource` opens the playing program's screen).
Not known yet: the arg2 values (press, release, long press) and whether a program acts on `0x4AB`
from a sender other than `Main.exe`. Not tried on the car.

## Bluetooth (Decoded, partly Seen)

**Parts (Seen, probe 0.2 survey).** The stack is CSR's BCHS (BlueCore Host Software), loaded as a
driver: `CSR1:` (`Drivers\CSR\BCHSDRV`, `btp_driver.dll`, chip settings
`\NandFlash\Application\Driver\ps-csr_soc_platform.psr`), power control `BTT0:` (`btpower.dll`) and
the serial-port profile ports `COM4:`/`COM9:` (`btp_sppdrv.dll`); the chip itself is on `COM3:`.
Paired devices and their link keys (the secret shared with each phone) are in
`ScDbFile = "\backup\bt_sc.db"`. Microsoft's `btd.dll`, `BTSVC.Dll` and `bthusb.dll` (USB dongles)
are installed too. The program is `\NandFlash\Application\Bluetooth\`: `Bluetooth.exe` (283,648
bytes), `SkBluetoothCore.dll` (694,272), `SP_BTCore.dll` (84,992), `SP_CSR8311.dll` (244,224),
`bt8311reg.dll`, `Bluetooth.ini`, `BTConfig.ini`; its screens are in `BluetoothRes.spp`,
`BluetoothExRes.spp` and `BluetoothRes.bin` (next to the other `*Res.spp`). One process owns the
bus window `Bluetooth` (app `0x0D`), the screen `BluetoothWindow` (JUCE, 800 x 480), `SP_BTWidnow`
(sic, 1 x 1), `Legacy Phonebook Access` and two windows with GUID class names. None of these files
has been copied off the unit yet.

**Talking to other programs (Decoded).**

- `SkCom.dll`'s `BluetoothApi` has no call to list paired devices, connect or disconnect. It has
  `sendToInfoBar` (now playing, above), `startA2DPSrc` (posts `0x475` arg1 15 to `Main.exe`) and a
  receive side that hands `0x4A`, `0x499`, `0x79F`, `0x7A0`, `0x7A1`, `0x7B6` and `0xBE3A` (test
  mode) to `Bluetooth.exe`'s own callbacks. `Main.exe` sends it only `0x499` (arg1 11 or 12, an
  on/off pair) and `0x79F` (arg1 3); at start-up `0x79F` arg1 10 arg2 1 and arg1 1 arg2 3 were
  **Seen**. No program on the unit asks `Bluetooth.exe` to connect a device.
- `Bluetooth.exe` reports to `Main.exe` with `0x475` (arg1 = what, arg2 = value), handled by
  `BluetoothApplication::handleBTState` (`Main.exe` RVA `0x70428`, cases 1 to 20; `0x490` is
  test-mode data passed to the MCU). Case 1 is the connection state: `Main.exe` stores arg2 as
  system info index 0 (so window message `0x52A` wParam 0 reaches every top-level window) and, for
  arg2 = 1, sets setup string `0x40F` back to "No Device". When setup value 8 is 0 (probably
  Bluetooth switched off) it stores 0 instead. Case 2 covers calls ("Calling...", "Dailing" (sic),
  "Connected..."), phone audio and mute, by arg2. **Seen** at every start, with no phone connected:
  arg1 1 arg2 1, arg1 6 arg2 0, arg1 14 arg2 1. The values with a phone connected are not known.
- Setup string `0x40F` is `BTDeviceName` (`SkShareMem.dll` key table at RVA `0x22110` + `0x464`,
  default "No Device"). `Main.exe` only ever writes "No Device", so the connected device's name
  is written by `Bluetooth.exe`; not seen yet. It can be read with `SP_GetSetupValueString`
  (static, returns a `juce::String`: hidden result pointer in r0, index in r1).
- `SystemHelper::showBluetooth()` (`SkCom.dll`) opens the factory Bluetooth screen:
  `PostMessageW(FindWindowW(NULL, "NavigRecive"), 0x46D, 5, 0)`. wParam 6, 7, 8 and 0x10 open
  maintenance, TPMS, telematics info and time alignment. `Main.exe` passes `0x46D` on as internal
  message `0x2000025`. Not tried on the car.

**For the dashboard.** A small panel showing whether a device is connected and its name can be
read-only (system info index 0, setup string `0x40F`), once a log shows the values with a phone
connected. Choosing among paired devices from the dashboard is not possible with what is known: the
paired list lives in the stack's database and inside `Bluetooth.exe`, and no known message asks it
to connect a device. Whether `Bluetooth.exe` accepts such a request can only be answered from a copy
of its folder. Sending commands to `CSR1:` or `COM3:` directly would compete with `Bluetooth.exe`,
which owns the stack, and is out of bounds. The fallback is a button that opens the factory
Bluetooth screen with `0x46D`.

## GPS and the other serial ports (Seen)

The GPS is a SiRF receiver on `COM1:` (`Drivers\BuiltIn\GPS`, `GPS.Dll`) at 38400 baud. It sends
standard NMEA plus SiRF `$PSRF...` sentences. No other program held the port while Total Commander
was in the navigation slot, so the dashboard can open it. There is no GPS Intermediate Driver.
GPS gives speed, course (heading while moving) and altitude. `Main.exe` also reads GPS
(`CGpsDataRead`, for the clock).

| Port | Hardware (registry) | Use |
|---|---|---|
| `COM1:` | SiRF GPS driver | GPS, 38400 baud |
| `COM2:` | SoC UART0 | MCU (Main.exe) |
| `COM3:` | SoC UART2 | Bluetooth module |
| `COM4:`, `COM9:` | Bluetooth serial-port profile (virtual) | need a paired device |
| `COM5:` | SoC USP1 | disc drive |
| `COM6:` | SoC UART1 | unknown; a read-only open fails with error 12 |
| `COM7:` | SoC USP0 | only zero bytes at 57600 and 115200, nothing at lower speeds: an idle line, no device |
| `COM8:` | USB function serial (`usbserialfn.dll`) | only when the unit is plugged into a PC as a device |

The SoC is a SiRF Atlas family chip ("C9SL" in the driver names) with two USB OTG controllers in
host mode, so USB devices can be attached; which USB device drivers exist is in probe 0.2's survey.
The Bluetooth stack (CSR) provides serial-port profile ports, which could carry data from a
Bluetooth OBD-II adapter or sensor if pairing works.

## What the firmware cannot provide

Steering angle, yaw rate, pitch, roll, engine RPM and coolant temperature are not read by anything
on the unit. Options, roughly from least to most effort:

1. **OBD-II adapter** (plugs into the car's diagnostic port). Standard requests give RPM (PID `0C`),
   coolant (`05`) and speed (`0D`). Steering angle and yaw rate are not standard requests: they need
   Mitsubishi-specific diagnostic requests or passive listening to the stability-control messages
   on the CAN bus. The car itself has no tilt sensor: the stability-control sideways and
   forwards G sensors give at best a rough pitch and roll while stopped. A Bluetooth adapter cannot
   be used by the head unit directly: its Bluetooth is run by the factory phone program, the
   serial-port profile ports have no paired device, and Windows CE 6 has no Bluetooth Low Energy
   support, so a phone or a board has to sit in between. A free first check: an adapter whose phone
   app does full-system diagnostics (for example MUCAR BT200) can show whether the stability-control
   unit's live data lists steering angle and yaw rate on this car.
   The stability-control unit (ASTC) is the source for steering angle and yaw rate. Mitsubishi
   manuals for related models list its inputs as a steering wheel sensor, a combined G and yaw rate
   sensor (sideways and forwards acceleration plus yaw rate) and the four wheel speed sensors, all
   shared over CAN; the dealer tool (MUT-III) shows them as live data. Nothing on the head unit
   mentions any of them (no `ASTC`, yaw, wheel-speed or 4WD strings); the only CAN-sending event,
   `EVENT_CMD_CAMRY_CAN_TX`, is for another car model and must not be used. No published CAN id
   map for this platform (Pajero Sport QE / Triton MQ) was found, so the ids would have to be found
   by logging with a listen-only CAN board (it cannot transmit) while turning the wheel.
2. **Motion sensor** (accelerometer and gyroscope, often called an IMU) for pitch, roll and yaw rate.
3. A small microcontroller board that does 1 and 2 and sends one simple text line per update over a
   single serial link is the most robust way to feed the dashboard.

Every option needs a link into the head unit. The unit has USB host ports and Bluetooth
serial-port profile ports (`COM4:`, `COM9:`). No USB serial driver is installed (probe run 2), so a
USB serial adapter would need a Windows CE driver added. Two built-in routes avoid that:

- **USB network (most promising).** `USB8023.DLL` is loaded for USB classes 2 (CDC), 224 (`0xE0`,
  wireless controller) and 239 (`0xEF`, miscellaneous), which are the classes RNDIS and similar
  USB network devices use, and the unit has a TCP/IP stack and Winsock. A small board that shows up
  as a USB network adapter (for example an ESP32-S3 with a CAN transceiver and a motion sensor, or a
  Raspberry Pi Zero in USB gadget mode) could send readings over UDP, with no driver or registry
  change on the head unit. An Android phone with USB tethering is a free first test: probe 0.4's
  Survey lists network adapters.
- **Bluetooth serial-port profile** (`COM4:`, `COM9:`): their `bluetoothAddress` is
  `000000000000` in the registry, so using them probably means pairing through the vendor's
  Bluetooth program or editing the registry, which this project avoids.

### Without extra hardware (Decoded)

- **The raw MCU data cannot be reached.** `Main.exe`'s MCU receive function (RVA `0x46458`, data
  pointer in `r1`, length in `r2`) would log every received byte as `"recv: "` plus hex, under the
  name `HostcommDev`, through `SP_Logcat` (whose log lives in the shared memory `Logcat_Mem`), but
  only when `SkypineConfig::isSystemDebug()` is true. In `SkCom.dll` (RVA `0x27c40`) that function
  is `mov r0, #0; mov pc, lr`, so it is always false and the log cannot be switched on without
  changing a binary in internal storage. The function then cuts the bytes into 32-byte pieces, each
  copied into a newly allocated object and queued, so there is no fixed buffer another program
  could read. Whether the MCU passes any steering-angle message from the car stays unknown; run 3
  showed nothing reaches the bus when the wheel turns.
- **GPS estimates, while moving only.** From the NMEA course and speed (one update per second):
  - yaw rate = change of course per second (allowing for the wrap at 360 degrees);
  - tyre angle = atan(wheelbase x yaw rate / speed), wheelbase 2.80 m. This is the average
    front-wheel angle of a simple two-wheel ("bicycle") model and ignores tyre slip;
  - slope along the track = elevation change / distance travelled, over at least a few tens of
    metres, from the barometric elevation (1 m steps) or the GPS altitude.

  Below about 5 km/h the GPS course is mostly noise, so these must be hidden at crawling speed and
  when stopped, which is exactly when off-road driving needs them. They also lag 1 to 2 seconds.
- **Roll** cannot be derived from anything on the unit.
- **A phone as the sensor.** An Android phone has an accelerometer and a gyroscope. Mounted in the
  car and connected by USB tethering (the USB network route below), it could send pitch, roll and
  yaw rate over UDP. Probe 0.4's USB network test checks whether the link comes up. Tyre angle
  would still be an estimate (gyroscope yaw rate and speed).

## Integration notes for the dashboard

- `SP_MAIN.ini` `[NAVIGATION]`: `PATH=\SD\Navigation\Navigation.exe`, `TITLE=GPS - Navigation`,
  `AUTORUN=1`. The dashboard runs in the `AppNavi` slot. `AUTORUN=1` suggests `Main.exe` may start
  navigation at boot (**Inferred**).
- `Main.exe` finds the navigation window with `FindWindowW(NULL, title)`, trying the Navi bus name
  (`GPS - Navigation`) and then `iGO 8`, `GPS - Navigation`, `NavMate`, `Navi`, `CMCNavi`
  (RVA `0xf87c`), and calls `ShowWindow` on it when switching screens (**Decoded**). So the
  dashboard's main window should be titled `GPS - Navigation`, and the dashboard must not join the
  bus as Navi (that would create a second, hidden window with the same title). Join under another
  id, as the probe does with `TerminalA` (0x1A).
- To get CarInfo data, do what probe 0.3 does: post requests with sender and arg1 `0x14` and read
  the replies from the shared ring, leaving the resident Car Info program untouched. Battery
  arrives on its own every few seconds once communication has been started.
- The barometric elevation (200 m) and GPS altitude (279 m) differed in probe run 2. The factory
  screen lets the user set the pressure reference; the dashboard could instead show GPS altitude,
  or correct the barometric value with GPS whenever the fix is good.
- The screen is 800 x 480 (**Seen**), Windows CE 6.0.

## Dashboard

`dashboard/` (version 0.3.0, **built, not run on the car yet**; 0.1.0 ran, see "First run" below) is
the one-page dashboard from the design canvas (https://claude.ai/artifact/9WDQyDaPa6RprDPPk3ST8x,
private). It is a complete SD `Navigation` folder: `Navigation.exe`, `fonts/` (Barlow), `README.txt`,
and `TotalCommander/cecmd.exe` when Total Commander was built. The map's files go in a separate
`Map` folder at the SD card root (see "Map" and "Address" below). Columns are 1:2:2 wide (150, 301,
301 px): outside, pressure, battery; heading (one row), elevation, speed; map (two rows), address.
Every graph draws its first value flat from the left edge, so it fills its width from the first
sample on ("Collecting data" only while there is none).

| Tile | Source | Rule |
|---|---|---|
| Pressure, graph of 1 h | CarInfo `0x21` | sampled every 30 s; "weather" trend from sea-level pressure (needs GPS altitude) over at least 1 h, scaled to 3 h: 1 hPa = falling or rising, 3 hPa = falling fast |
| Elevation, graph of 20 km | GPS altitude, 10-second average | car's barometric elevation plus the last GPS-minus-car difference when GPS has no fix; climb rate over about a minute; a graph point every 250 m |
| Outside, graph of 3 h | CarInfo `0x31` | sampled every 90 s; change shown after 30 min |
| Heading (one row: rose left, degrees right) | GPS course above 6 km/h (back below 4 km/h) | otherwise the car's 8-point compass (`0x11`) |
| Battery, graph of 30 min | CarInfo `0x40` | 13.2 V and up charging, 12.2 to 13.2 V not charging, below 12.2 V low, 14.8 V and up too high |
| Map (wide column 3, rows 1 and 2) | GPS position, `\SD\Map\*.pack` | north-up offline map filling the whole tile (no title or label; rounded corners painted over), centred on the last GPS position; arrow along the GPS course while the heading comes from GPS, otherwise a dot (grey without a fix); +/- zoom through the levels the files hold (start at 14); half brightness at night |
| Address (wide column 3, row 3) | GPS position, `\SD\Map\*.addr` | nearest named road within 50 m (else "No named road nearby"), "Brgy." and the barangay (else "Near" and the nearest named place), then city and province; looked up on its own thread after about 15 m or 30 s; the last address stays, marked "Last GPS position", without a fix; never logged |
| Speed panel (wide column 2, row 3) | GPS | current speed large, average (counts only above 3 km/h) and top speed (highest median of three readings) small on the right, both since the dashboard started and not saved; graph of the last 10 min, a point every 5 s (mean of that time), none while GPS has no fix |
| Header | GPS time, bus | clock: GPS UTC time and date (1024-week rollover corrected), kept running on the tick count when the signal drops; the unit's clock only when its year is 2020 or later. Time zone: the menu choice (`dashboard-settings.txt`), else UTC+8 (`DEFAULT_UTC_OFFSET_MIN` in `dash.c`, the user's choice). The model can also derive it from the unit's clock or time-zone setting, but `dash.c` always passes a zone because both are unreliable on this unit. Sunset or sunrise from the last GPS position. "Headlights on" and night colours from `0x52A` index 9 or `0x33` bit `0x04` |

Car data counts as current for 30 s after the last CarInfo reply, GPS for 3 s after the last RMC
sentence. The bus client is the probe's: it joins as `TerminalA` (waits while another `TerminalA`
window exists, for example the probe), reads the ring backlog, sends `81 01 01` once, then `11 00`,
`21 00`, `A0 02 40`, `31 00` every 5 s with sender `0x14`. Holding the clock for 2 s opens a menu:
file manager, close, back. `dashboard-log.txt` next to the exe records start-up, which drawing
functions and fonts loaded, bus and GPS status, and a summary every 10 minutes (no position).

First run (dashboard 0.1.0, `dashboard-log.txt`, **Seen**): `Ellipse`, `RoundRect`, `Polygon`,
`Polyline` and `GetTextFaceW` all exist in this unit's COREDLL; the three Barlow fonts loaded with
`AddFontResourceW`; COM1 opened; the bus joined as `TerminalA` with 85 backlog records and the
first CarInfo reply within a second, and all requests posted; night colours switched with the
headlights through `0x33`. The unit's clock read **2012-06-08** (15:04 while it was night), as on
probe run 3 a day earlier (the probe log's file date), so the clock resets when the unit loses
power and cannot be used for the time or the date; dashboard 0.2 takes both from GPS.

Still open: whether `AUTORUN=1` starts it at boot and whether `Main.exe` then shows or hides it;
how long the GDI repaint takes once a second; whether the unit's time-zone setting is right.

### Map (dashboard 0.2, **built and tested on the Mac only**)

The user wants to see where the car is, with no routing (the factory GenieTek `Navigator.exe` has a
Philippine map from 2018-01-12 that can no longer be updated). The map is made on the Mac from
OpenStreetMap data with `tools/map/` and stored as **Montero tile packs** (`*.pack`, format in
`tools/map/mtpack.py`): one zoom level and one rectangle of 256 x 256 PNG tiles per file
(OpenStreetMap tile numbering, Web Mercator), an index of offset and length per tile, identical tiles
stored once, each file below 2,000,000,000 bytes for FAT32 and 31-bit file offsets. The dashboard
opens every `*.pack` in the `Map` folder beside its own folder (`\SD\Map\`) read-only, and
`dashboard/src/map.c` reads the index entry and tile bytes on demand, decodes PNG with stb_image
(`dashboard/src/pngdec.c`, PNG only), keeps 12 decoded tiles (RGB565, 128 KB each) and composes the
299 x 271 view (the whole tile inside its border, no title) into a 16-bit DIB section that is
copied onto the back buffer. `CreateDIBSection` is
looked up at run time like the other optional GDI functions. Tests on the Mac:
`tests/test_map_reader.py` (decoding, pixel placement, empty, corrupt and missing tiles, night,
cache, bottom-up bitmaps) and SVG previews with the composed map embedded.

Making the map (`tools/map/build_map.sh`): planetiler 0.10.2 turns the Geofabrik Philippines
extract (plus OpenStreetMap water polygons, Natural Earth and lake centerlines, about 2 GB of
downloads) into OpenMapTiles vector tiles for the Luzon box `119.4,12.4,124.5,18.8` (about 5 min;
reading the whole extract dominates, so a smaller box hardly saves time). tileserver-gl 5.7.0
draws 8-bit palette PNG tiles from them with our own dark style (`tools/map/style/`, written by
`make_style.py`): roads by class, tracks dashed ochre, paths dotted, water blue, peaks with heights;
unnamed tracks and paths from zoom 14, buildings only at 15. `build_map.py` fetches the tiles
from that local server only, skips open sea below sea parents from zoom 11 (a one-colour tile is
reused; tiny islets far from a coast could be missed), caches rendered tiles by a fingerprint of
style and data, and writes one pack per zoom level (column bands when one would pass the size
limit). Test area east of Manila (zoom 8 to 15, 234 tiles): about 65 tiles/s, 7.5 KB per tile;
dense Manila about 22 tiles/s and 32 KB per tile. The first Luzon build (2026-11-15, data up to
2026-11-13) took 12 min with the vector tiles already made: zoom 8 to 14 in 7 packs, 244 MB; zoom
15 in one pack of 416 MB (281,790 tiles, 102,729 rendered, 178,833 by the sea shortcut, about
219 rendered tiles/s); 632 MB in all. The licences ask for "© OpenMapTiles
© OpenStreetMap contributors" to be visible; the map shows "© OpenMapTiles © OpenStreetMap".
No contour lines or hill shading yet (the OpenMapTiles schema has no elevation).

The log gets `[map] N map files, zoom A to B` at start and, every 10 minutes, the zoom, tiles
decoded, failures, read errors and the slowest map draw in ms. Open questions for the first run:
whether `CreateDIBSection` with `BI_BITFIELDS` 565 works on this unit, how long a tile takes to
decode from the SD card (the first draw after a zoom change decodes up to four), and whether the
map is readable in sunlight.

### Address (dashboard 0.3, **built and tested on the Mac only**)

`tools/address/build_address.sh` makes `luzon.addr` (about 28.6 MB, 1 min) from the same
Geofabrik extract: osmium-tool 1.18 (Debian package in a local Docker image, no network) exports
boundaries, named roads and places as GeoJSON, and `build_address.py` simplifies them (outlines
10 m, roads 4 m) and writes a **Montero address file** (`*.addr`, format and reference lookup in
`tools/address/mtaddr.py`): a grid of 0.02 degree cells, each listing copies of the 32-byte
records of the shapes that really reach it (outlines, roads within 75 m, places within 3 km), then
coordinates and names. `dashboard/src/addr.c` reads one cell's records, tests the outlines whose box
holds the point (even-odd rule, so holes and multi-part areas work; the smallest box of a kind
wins), finds the nearest named road within 50 m and the nearest named place within 3 km, and reads
only the winning names. In the real file a lookup reads about 18 KB (63 KB at most); `dash.c` runs
it on its own thread after about 15 m or 30 s. `dash.c` opens the first `*.addr` in `\SD\Map\` that
reads as one and skips names starting with a dot (for both `*.addr` and `*.pack`): a Mac copying to
the FAT card adds a `._name` side file (its file details) next to each file, which also matches the
pattern. Before that change, the first car run of 0.3.0 showed "No address file"; the cause is not
yet known from a log (either `luzon.addr` was not on the card, or the `._luzon.addr` side file was
found first and failed to open, which 0.3.0 showed the same way).
The screen now says "No address file" only when no `*.addr` exists and "Address file cannot be
read" when one exists but does not open. `tests/test_addr_reader.py` checks the C reader
against the Python reference on a made-up file and on the built Luzon file (junction ties, where two
roads are equally near, may name either road).

OpenStreetMap tagging in the Philippines (checked in the 2026-11-13 extract): admin_level 4
province, 6 city or municipality (Pateros included), 10 barangay (numbered ones are named
"Barangay 470" and so on; the dashboard adds "Brgy." only to other names); 3 region, 5 Metro Manila
districts, 7 legislative districts, 8 and 9 Manila's districts and zones, 11 and 12 sitio and purok.
Metro Manila has no province, so its level-3 outline is used as "Metro Manila". OpenStreetMap
leaves the independent cities Baguio, Angeles, Olongapo, Lucena and Naga out of their provinces, so
a fixed table adds them back under the province people write (Baguio, Benguet). Barangays come
from relations and level-10 closed ways (52 Laoag barangays are wrongly tagged level 6 and are
skipped). Names the font cannot draw are rewritten (Roman numeral characters to I, II, ...; "№" to
"No."; the non-breaking hyphen to "-").

Coverage in Luzon against PSA's list (PSGC, 30 June 2026): all 37 provinces in the box; 416 of
about 724 cities and municipalities have outlines (about 57%; complete in Metro Manila, Central
Luzon's south and Calabarzon, almost none in Abra, Kalinga, Quirino, Nueva Vizcaya, the Ilocos
provinces, Isabela, Ifugao and Apayao); 3,566 of about 19,700 barangays (about 18%; Quezon City
complete, Manila 147 of 897). About 13,900 barangays exist as named points, so the tile falls back
to "Near" the nearest place. Of named village, hamlet and neighbourhood points, 100% get a province,
63% a city and 22% a barangay outline. Along all drivable roads (sampled every 400 m), 33% get a road
name within 50 m: motorways and trunk roads over 98%, primary 97%, secondary 83%, tertiary 35%,
residential 32%, unclassified 10%, tracks 3%; a named place within 3 km 97% (89% on tracks).
Missing names and outlines can be added in OpenStreetMap; they appear after the next build.

## The probe

`probe/` is a small Windows CE program (C, built with CeGCC in Docker: `tools/build_probe.sh`). It
runs from `\SD\Probe\Probe.exe`, started from Total Commander, and appends everything to
`probe-log.txt` next to itself. See `probe/README.txt` for the on-car steps. Version 0.4 buttons
(starting `Probe.exe` while it already runs brings the running copy to the front):

1. **Survey** (read-only): OS version, screen size, every top-level window, `HKLM\Drivers\Active`
   with each driver's settings, the USB driver registry (`Drivers\USB\ClientDrivers`,
   `Drivers\USB\LoadClients`), driver-like files in `\Windows`, and `\NandFlash\Application`.
2. **Bus**: loads `SkCom.dll`, joins the bus as `TerminalA` (0x1A) through the vendor's own
   exported functions, then reads the shared ring: first the backlog (everything still in the
   current lap), then new records every 100 ms. Each message type (id and sender) is logged when
   first seen and whenever its content changes, with its bundle decoded by the probe itself.
   Every CarInfo frame from `Main.exe` is logged and decoded at the top of the screen. It sends
   `81 01 01` once, then `11 00`, `21 00`, `A0 02 40`, `00 00`, `31 00`, `41 00`, then repeats the
   first four every 5 seconds, all with sender and arg1 `0x14`. Its own records are recognised
   by their ring offsets and not logged twice. It never sends `81 01 00`.
3. **GPS**: reads `COM1:` at 38400 and shows speed, course, altitude, fix and satellites.
   Position is never logged.
4. **Ports**: opens each COM port from the driver list read-only, except COM2, COM3 and COM5.
   For NMEA ports it logs only which sentence types appear; for anything else a hex sample.
5. **Next step**: a guided test that writes a numbered marker into the log, so unknown messages
   can be matched to actions. Version 0.4 steps: parking lights, headlights, hazards, reverse (foot
   on the brake), parking brake, and changing the fan from the home screen. Survey also lists
   network adapters (`HKLM\Comm\*\Parms\TcpIp`) for the USB tethering test.
6. **Exit**: logs a summary and quits.

Always on: logs when `Main.exe`'s `AirControlBar` climate popup is shown or hidden and saves up to
12 screenshots of it as `aircontrol_NN.bmp`.

The ring reader and bundle parser (`probe/src/busring.c`) and NMEA parser (`probe/src/nmea.c`)
are tested on the build machine by `tests/test_probe_selftest.py`, partly with messages copied
from the car. The probe checks two export offsets of `SkCom.dll` before using its internal
shared-memory pointer and refuses to start on another build.

## Probe run 1 (probe 0.1)

- Survey: Windows CE 6.0, screen 800 x 480, window list and drivers as described above. No
  motion-sensor or CAN driver. `Loader\SystemPath` is `\Release\`.
- CarInfo session: refused safely. Probe 0.1 checked for a window named `AppCarInfo`, but the bus
  window is named `CarInfo` and the factory Car Info program is always running, so
  `MsgSession::init` saw the existing window and returned false. This is why probe 0.2 joins as
  `TerminalA` and reads the ring instead.
- GPS scan: NMEA found on `COM1:` at 38400. `COM7:` streams binary data at 57600 and 115200 (not
  identified yet). `COM4:`/`COM9:` (Bluetooth) and `COM6:` could not be opened.

## Probe run 2 (probe 0.2)

- Bus: joined as `TerminalA` without problems and read 1007 records over about four minutes, with
  no resyncs. The backlog already held the factory screen's own exchange: `81 01 01`, then
  `61 01 00`, `11 00`, `21 00`, `A0 02 40`, answered by `11 01 04 e9` (compass SE),
  `21 04 03 dc 00 c8 33` (988 hPa, 200 m) and `40 04 ce 05 07 8c ..` (service 1486 in 7 months,
  battery 14.0 V).
- The probe's own 209 requests (sender `0x1A`) were never answered; see "Routing in `Main.exe`".
  Battery frames (`0x40`) kept arriving every 2 to 5 seconds anyway: 13.6 to 14.1 V with the
  engine running.
- `00 00` (RV meter) got no answer from anyone, so outside temperature is still unconfirmed;
  probe 0.3 also tries `31 00`.
- Unknown CarInfo frames `33 01 xx` (xx = 0, 2 or 3, no checksum byte) arrived in bursts around
  the time of the steering-wheel and fan test. Nothing on the unit decodes command `0x33`. The
  guided test in probe 0.3 will tie it to an action.
- No climate messages on the bus while the fan was changed. The rear-camera program (app `0x0C`)
  registered at one point (`0x6D1` from sender `0x0C`), and window message `0x52A` was broadcast
  with wParam 8, lParam 1 then 0, which looks like a reverse-camera on/off event.
- Other traffic: heartbeats `0x6D3`/`0x6D4`, Bluetooth (`0x475`, `0x4A0` from `0x0D`), page
  changes (`0x6BC` to `0x6C2`, `0x6DA` with `Format`, `int1`..`int3`), source info (`0x496` with
  `rearSrc`, `normalSrc`, `frontSrc`), version strings (`0x6E5` from `0x08`).
- GPS: fix with 8 to 9 satellites, altitude 279.1 m, one update per second (`GPGGA`, `GPGNS`,
  `GPGSA`, `GPRMC`, `GPGSV`, plus SiRF `PSRF...`), 0 bad checksums.
- `COM7:` returned only zero bytes, so there is no device on it. `COM8:` (USB function serial)
  was silent.
- USB host class drivers installed: HID (keyboards, mice), mass storage, printer, USB network
  (`USB8023`), Bluetooth dongles (`bthusb`), Apple iPod, and a Senselock dongle. **No USB serial
  driver** (FTDI, CP210x, PL2303 or CDC-ACM), so a USB serial adapter will not get a COM port
  without adding a driver.

## Probe run 3 (probe 0.3)

- Requests posted with sender `0x14` are answered: 243 sent, 0 failed, each answered within about
  100 to 200 ms. This is the working method for the dashboard.
- `31 00` answered `31 02 1e 00 ae`: outside temperature 30 °C. `00 00` and `41 00` were never
  answered, so the RV-meter frame and the trip computer are not available on this car.
- Guided test: steering wheel left, right and centre, fan up and down, temperature up and down,
  turn signal and door produced **no bus message and no CarInfo frame**. Headlights on produced
  `33 01 04` plus window message `0x52A` wParam 9 lParam 1 to every bus window; off produced
  `33 01 00` and lParam 0. In run 2, `0x52A` wParam 8 came with the rear-camera program starting,
  so wParam 8 is probably reverse and wParam 9 the lights (night mode).
- The climate popup (`AirControlBar`) never appeared while the probe was in front, so no
  screenshots. Probe 0.4 tests changing the fan from the factory home screen.
- 1512 bus records in about five minutes, no resyncs, no failed requests.

## Next steps

1. Run probe 0.4 (`probe/README.txt`): lights, hazards, reverse, parking brake and the USB
   tethering test. The fan steps (7 and 8) can be skipped. Bring back `probe-log.txt`.
2. From the log: the meaning of the `0x33` bits and of `0x52A`, and whether a USB network adapter
   appears.
3. For pitch, roll and yaw rate: if USB networking works, a phone over USB tethering is the
   no-purchase route. For real steering angle, RPM and coolant: a small USB-network sensor board
   (CAN for steering angle and yaw rate, OBD-II requests for RPM and coolant).
4. Run dashboard 0.1.0 (`dashboard/README.txt`) and bring back `dashboard-log.txt`.
5. Bluetooth device switching: copy `\NandFlash\Application\Bluetooth\` and the three
   `Bluetooth*Res` files to the SD card with Total Commander (copy, never move; do not copy
   `\backup\bt_sc.db`, it holds link keys), to find out whether `Bluetooth.exe` accepts a connect
   request and where it keeps the paired list. For the status panel, log `0x475`, `0x52A` wParam 0
   and setup string `0x40F` while connecting, disconnecting and switching phones in the factory
   screen.
6. Media: run the probe's bus logging while Bluetooth music plays, and press next, previous and
   pause on the steering wheel and on the InfoBar. The log should show `0x4AB` (key and state, to
   the Bluetooth app) and `0x4A0` with the song fields.

## Reproducing this analysis

```sh
python3 tools/re/spp.py dumps/CarInfoRes_6982C.spp                 # list a resource pack
python3 tools/re/spp.py dumps/CarInfoRes_6982C.spp /tmp/new-dir    # extract to a NEW folder
python3 tools/re/cestrings.py Bin/Main.exe                         # ASCII and UTF-16 strings
python3 -m venv .venv && .venv/bin/pip install -r tools/re/requirements.txt
.venv/bin/python tools/re/cedis.py Bin/SkCom.dll --exports CarInfoApi
.venv/bin/python tools/re/cedis.py Bin/SkCom.dll procCarInfoMsg 120
.venv/bin/python tools/re/cedis.py Infor/CarInfo.exe 17abc 60      # CarInfo id handler
```

Useful addresses (RVAs). `SkCom.dll`: `procCarInfoMsg` `0x4e54` (jump tables at `0x4e88` for
commands `0x00`-`0x43` and `0x5288` for `0x58`-`0x90`), `Car_Status_Process` `0x4028`,
`copyDataToHost` `0x10a48`, `CarInfoApi::remoteCallBack` `0xd0e8`, `MainServiceManager`
constructor `0xa0f8`, `MsgSession::init` `0x1ee88`, bus window procedure `0x1e3e0`,
`MsgSession::postMessage` `0x1d6a0`, ring writer `0x1cf30`, ring setup `0x1d3b4`, shared-memory
base pointer `0x69a34`, bus window names `getDestName` `0x11b8`, app-name table `0x2424`,
`Bundle::writeToParcel` `0x1ac68`. `Infor/CarInfo.exe` (image base `0x10000`): id handler
`0x17abc` (jump table `0x17af8`), start-up requests `0x10214`, page-show timer `0x13cc8`, compass
text `0xfa50`, compass needle `0xf600`. `Main.exe` (image base `0x10000`): MCU event name table
`0xad00`, post-to-app helper `0x497e4`, `0x6D1`/`0x6D4` handler `0x542cc`, incoming `0x4A`
routing `0x7a358`, CarInfo reply post `0x5954c`, navigation window lookup `0xf87c`. The
disassembler prints branch targets as full addresses (image base plus RVA).
