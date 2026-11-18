# Vehicle data on the head unit: research notes

Status (last updated 2026-11-17): static analysis of the files in `Bin/`, `Infor/` and `dumps/`, plus
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
| Parking brake | Probably | Device status bit `0x0100`, kept in `Main.exe`'s memory only, never broadcast | Inferred; probe 0.5 reads it |
| Doors, seatbelt, fuel level | No | Nothing in `Main.exe` handles them; the CAN box door frame is dropped | Decoded, see "What `Main.exe` receives from the MCU" |
| Average speed, average fuel economy, range | No answer | CarInfo `0x41` / `0x00` trip block | `41 00` and `00 00` are never answered on this car |
| Service reminder | Yes | CarInfo `0x40` | Seen: 1486 (probably km) or 7 months; not wanted for the dashboard |
| Tyre pressure and temperature | Probably not | CarInfo `0x60` to `0x6D` | The factory screen's `61 01 00` got no answer in run 2 |
| A/C fan speed | Probably | `Main.exe`'s memory (popup object) | Decoded: level 0 to 8; not wanted for the dashboard |
| Vehicle speed | Yes, from GPS | GPS only: a CAN box speed value would go to the missing `Radar` program and is not kept | GPS seen; CAN box: Decoded |
| GPS heading, speed, altitude | Yes | SiRF receiver on `COM1:`, 38400 baud, NMEA | Seen |
| Steering (tyre) angle, yaw rate | **Not found** | Nothing on the unit reads them | Rough GPS estimate while moving; real values need extra hardware |
| Pitch and roll | **Not found** | | Pitch: rough slope from elevation change while moving; roll needs a motion sensor |
| Engine RPM, coolant temperature | **Not found** | | Needs an OBD-II adapter |

"Not found" means: no program, library or screen resource on the unit mentions it, and the
probe's driver list has no motion-sensor driver. The only "angle" and "tilt" names (`KEY_ANGLE`,
`KEY_TILT`, `ScreenAngle`) are for tilting the screen, and `SteeringWheel` is the steering-wheel
buttons setup. `CanInfo.exe` (the CAN-box screen) and `AC\AC.exe` are not installed on this unit,
and no code in `Main.exe` builds the CAN-box message id `0xBE54` as a constant, so raw CAN-box data
is not forwarded to any app: `Main.exe` reads the CAN box frames itself and keeps only climate and
parking-sensor values (**Decoded**, see "What `Main.exe` receives from the MCU"). Probe run 3 confirmed it: turning the steering wheel produced
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
- `Main.exe` turns MCU traffic into numbered commands, with one name table for what it sends
  (`EVENT_*`, RVA `0xad00`, for example `0x12` `EVENT_CMD_CANBOX`) and another for what it receives
  (`IND_*` and `*_RESP`, RVA `0xb048`), so one number can have two names (**Decoded**, see "What
  `Main.exe` receives from the MCU").
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
  `SP_SetSysInfo(index, value)` (`SkShareMem.dll`, readable with `SP_GetSysInfo`) through one
  helper (RVA `0x1aa10`, 93 call sites, arguments `this`, index, value), which also broadcasts
  `PostMessageW(HWND_BROADCAST, 0x52A, index, value)` to every top-level window. Seen: index 9 =
  lights (1 on, 0 off), index 8 = reverse / rear camera, index 0x41 also changes with page
  switches. Decoded: index 0 = Bluetooth connection state (see "Bluetooth").

  The table is the 72-byte shared memory block `SkSysInfoShareMem`, laid out by `SkShareMem.dll`
  (147,456 bytes) at RVA `0x3ff0`: 62 indexes, all single bytes except index 0x2C (a 32-bit int at
  offsets 0x2C to 0x2F). Up to 0x2B the offset equals the index; from 0x2D on the offsets shift
  (0x2D at 0x30, 0x41 at 0x45, 0x43 at 0x40 and so on; the full list is `probe/src/sysinfo.c`).
  Indexes 0x36 to 0x3A and 0x40 do not exist. `SP_GetSysInfo` opens the block with
  `CreateFileMappingW` and a size of 0, which can only open an existing block; only when that
  fails does it create the block and fill in defaults. Who sets which index (all 93 calls):

  | Index | Set by | Meaning |
  |---|---|---|
  | 0 | Bluetooth state handler (RVA `0x70428`) and others | Bluetooth connection (Decoded) |
  | 1, 2, 0x1D, 0x2A, 0x2B, 0x2D | Bluetooth, Bluetooth serial port, iPod | their states (Inferred) |
  | 5 | device status bit `0x0800` (MCU) | a car signal, unknown; nothing in the copied binaries reads it |
  | 7 | `DeviceStatusManager` ("CallState") | phone call (Decoded) |
  | 8 | device status bit `0x0400` (MCU) | reverse (Seen) |
  | 9 | device status bit `0x0200` (MCU) | headlights (Seen) |
  | 0xA | MCU command `0x28` `00 xx` | screen or standby state (Inferred) |
  | 0x41 | `ViewManager` (RVA `0x55900`) | current page (Seen) |
  | 0x42 | auto test | test mode |
  | 0x44 | `DeviceStatusManager` | set to 0; not in the table, so only the broadcast happens |
  | 3, 4, 0xB to 0x17, 0x2C, 0x30 to 0x35, 0x3B, 0x3E | start-up, settings, radio, main menu, navigation, telematics | program and settings states |

  Only indexes 5, 8, 9 and 0xA come from MCU data, and none from doors, seatbelt, fuel or speed.
  Probe 0.5 maps the block read-only and logs every value, because values that never change were
  never broadcast while the probe listened.
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

- Tyre frames would arrive on the same path as compass and pressure: Car Info requests go to the MCU
  as command `0x3A`, which `Main.exe` names `EVENT_CMD_TX_TPMS_DATA`; the replies come back as
  incoming command `0x3A`, named `IND_AIR_DATA`. The radio receiver, if there is
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

## What `Main.exe` receives from the MCU (Decoded)

Traced on 2026-11-17 to find car data that never reaches the bus. This is static analysis: what
the MCU actually sends on this car is known only from the probe logs.

`HostSessionManager` (RVA `0x48790`) accepts only MCU frames that start with `A0`; the command is
`((b4 & 0x7f) << 8) | b5` and the payload starts at byte 6. Each frame becomes the internal
message `0x200000D` (arg1 command, arg2 length). Exactly 8 functions test for that message, and
all of them were read. Commands sent to the MCU are named in the `EVENT_*` table (RVA `0xad00`),
commands received in the `IND_*` and `*_RESP` table (RVA `0xb048`).

| Incoming command | Name | Handler (RVA) | What `Main.exe` does with it |
|---|---|---|---|
| `0x3A` | `IND_AIR_DATA` (the Car Info channel) | `0x58ddc`, branch `0x593dc` | Car Info command `0x32` is climate: payload bytes 2 to 6 go to the popup only (kinds 7, 8 (fan), 0, 1, 2). Every other command is posted to CarInfo (`0x4A`) and Telematics (`0x4B8`), so all other Car Info frames reach the ring |
| `0x2C` | `IND_CANBOX_INFO` | `0x59104`, switch on the first payload byte | `21` climate; `22`, `23`, `25` parking-sensor values and `16` a 16-bit value, all for the `Radar` app; `17` to `20`, `24` and everything above `25` are ignored |
| `0x2B` | `IND_C2CAN_INFO` | `0x58f38` | `1A` climate (7 bytes); `19` parking sensors for `Radar`; `17` the CAN adapter's version ("ADP %d V%d.%d", setup string `0x406`); the rest is ignored |
| `0x39` | `IND_SWC_STATE` | `0x58f24` | two bytes to the `SWC` app as `0x538` |
| `0x03` | `GET_DEVICE_STATUS_RESP` | `DeviceStatusManager` `0x73cec`, `0x73f28` | the device status word, below |
| `0x28` | `IND_MCU_CMD` | `0x73cec` | `00 01` and `00 00`: system info `0xA` = 1 or 0 with a hide or show window broadcast (screen or standby, Inferred); `01`, `02`, `03 01`: internal |
| `0x31` | test mode | `0x58e60` | starts `HeroTestMode.exe` |
| `0x01`, `0x02`, `0x04`, `0x06`, `0x09` | last memory, version, settings, source switch | | settings and sources only |
| `0x1D` to `0x25`, `0x30`, `0x32`, `0x33`, `0x4D`, `0x4E`, `0x1F`, `0x50` | radio, RDS, TV, keys, auto test | | not car data |
| `0x2A`, `0x41`, `0x42`, `0x4B` and all others | `IND_CAN_IAP_CMD`, `IND_CAN_CONFIG`, `IND_MISC_FLAG`, `IND_GOLFCAN_INFO`, ... | none | dropped |

Values for the `Radar` app (and for `SWC` when its window is missing) never reach the ring:
`MsgSession::postMessage` (`SkCom.dll` RVA `0x1d6a0`) looks the window up first and writes
nothing when it does not exist, and `Radar.exe` is not installed. Requests that the Car Info
screen sends with arg1 `0x1C` go to the MCU as command `0x41` (`EVENT_CMD_CAMRY_CAN_TX`) sub 5
(RVA `0x7a488`); nothing ever answers app `0x1C`.

In the common aftermarket CAN box protocol (**Inferred** from the numbers only), `21` is climate,
`22` and `23` the rear and front parking sensors, `24` doors and other body state, `16` speed and
`29` the steering angle. The door and steering frames are exactly the ones this firmware drops.

**Device status word.** `DeviceStatusManager` (object pointer at `[0xbe4cc]`, vtable `0x9bce8`,
created by the getter at RVA `0x21700`) splits the 16-bit word (`p0 << 8 | p1`) into bytes at
object +0x4C to +0x64 and keeps the previous copy at +0x65; its change handler (RVA `0x6a94c`)
compares the two. Changes are skipped while object +0x30 is set.

| Bit | Byte | Broadcast as | Meaning |
|---|---|---|---|
| `0x0200` | +0x4D | system info 9 | headlights (Seen) |
| `0x0400` | +0x4E | system info 8 | reverse (Seen with the rear camera) |
| `0x0800` | +0x4F | system info 5 | unknown; nothing in the copied binaries reads index 5 |
| `0x0100` | +0x4C | not broadcast | the TV app holds something back while it is 0 (posts `0x49D`, RVA `0x59800`): probably the parking brake, from the usual "video only when parked" rule (Inferred) |
| `0x8000` | +0x5C | not broadcast | posts `0x2000028` or `0x2000029` to Radio, USB, ALink and the source switch: probably mute or pause (Inferred) |
| `0x0001`, `0x0008`, `0x0010`, `0x0080`, `0x2000`, `0x4000` | +0x54, +0x57, +0x58, +0x5B, +0x51, +0x52 | not broadcast | stored only; unknown |
| `0x0002` | not stored | | a virtual call; unknown |
| `0x0004`, `0x0020`, `0x0040`, `0x1000` | | | never read |

The other bytes of the block (+0x50, +0x53, +0x55, +0x56, +0x59, +0x5A, +0x5D to +0x64) hold media
and camera states set by other code. Probe 0.5 reads the 25 bytes read-only (`[devstatus]` log
lines, same `Main.exe` size check and process handle as the fan reader), so its parking-brake step
shows which bit moves.

**Verdict.** Doors, seatbelt, fuel level, RPM and coolant: no handler, no stored field, no system
info index and no text for them in `Main.exe`; if the MCU sent them, `Main.exe` would drop them.
Gear: reverse only. Ignition: nothing named; `IND_MCU_CMD 00 xx` (system info `0xA`) is the only
power-like signal (Inferred). Vehicle speed: only CAN box `16`, sent to the missing `Radar` app and
not stored, so it cannot be read even if the MCU sends it. Climate arrives in three forms (Car Info
`0x32`, CAN box `21`, C2CAN `1A`), all for the popup only. These values can only come from a link
to the car's own network (see "What the firmware cannot provide").

## Climate

Not wanted for the dashboard; kept for reference.

`Main.exe` contains a climate popup (`aircontrolbar.bin`, window `AirControlBar` seen on the car)
with `StAcOnOff`, `PbFanSpeed`, `StAuto`, `StWindMode`, `StTemp`, `StTempRight`, `StAir`
(recirculation) and `StDemister`, so it decodes climate frames itself: they arrive as Car Info
command `0x32` (on other CAN boxes as CAN box `21` or C2CAN `1A`) and go only to the popup (see
"What `Main.exe` receives from the MCU"). There is no
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
(sic, 1 x 1), `Legacy Phonebook Access` and two windows with GUID class names. The program folder
was copied off the unit on 2026-11-18 (not kept in the repo); the three `Bluetooth*Res` files were
not.

**Profiles (Decoded, from the copied folder).** The stack is built with CSR's Windows CE 6 SDK
(`CSR_SfWE_CE6_SDK` in a debug path, version string `Sfwe 1.1.13` in `Bluetooth.exe`), and
`SP_CSR8311.dll` drives it through Microsoft's standard Bluetooth functions in `BTDRT.dll`
(`BthCreateACLConnection`, `BthAuthenticate`, `BthNsLookupServiceBegin`, `BthSetPIN` and others) plus
`WS2.dll` (Winsock). Each profile is a wrapper class that loads its CSR library by a name read from
`HKLM\Software\CSR\Applications\...`: `CApi_HF` (hands-free calls), `CApi_A2dp` (music),
`CApi_Avrcp` (music controls and browsing), `CApi_PB` (phonebook, `btp_ui_ext_pb.dll`,
`csr_bt_vcard.dll`), `CApi_SPP` (serial-port profile: Apple accessory protocol, Pandora on Android,
and an Android helper app for HDMI mirroring, `HDMISPP.cpp`), `CApi_HID` (the unit acts as a
Bluetooth mouse, `CSR Mouse`, so touches reach the phone during mirroring: `SetHIDMouseMove`,
`SetHIDMouseLeftClickValue`), `CApi_SearchPair` and `CApi_SystemEvent`. There is no network profile:
no PAN (Bluetooth tethering), no BNEP (its transport), no DUN (dial-up) in any of the five files, and
the probe 0.3 driver list has no Bluetooth network adapter. So the unit cannot reach the internet
over Bluetooth with the factory software (see "Internet access"). `BTConfig.ini` holds the stack
settings (`ComPort=COM3:`, `SSPMode=1`, `AutoConnect=0`, `UseDataBase=0`); `Bluetooth.ini` holds the
device name `MITSUBISHI` and audio gains.

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

## Internet access (Seen and Decoded, nothing tried on the car)

- **No Wi-Fi on the Windows CE side.** The probe 0.3 driver list has no Wi-Fi driver and no
  wireless settings service; the two SD host controllers serve only `SDMemory.dll` and
  `triGDrv.dll` ("TriG SDIO driver", by its name and settings probably part of the satellite
  receiver, not checked). The factory Wi-Fi mirroring is an outside video source: `Main.exe` lists
  `eVideoWifi` and `eAudioWifi` among the switched inputs next to `eVideoCamera`, `eVideoAux1`,
  `eVideoDVR` and `eVideoHDMI` (Windows CE's own sound is `eAudioWince`), and `Infor/Wifi.exe`
  only calls `VideoIn::show` (input type 3, 0, 0, 800 x 480; RVA `0x1e38`), like the camera and AUX
  screens (it still holds the name `AuxInWindow`). The Wi-Fi radio is in the mirroring module, and
  the module reaches the unit only as video. An owner's guide from 2017 ("Updating Montero Sport
  2016 HU firmware online", first on atbp.ph, read from a copy on eriels.com.ph; not tried here)
  fits this: the module runs its own Wi-Fi network (the author's was "Mitsubishi Wireless TV 507"),
  serves a setup web page at 192.168.59.254, shows its own setup menu on the screen as video, and is
  controlled from a phone browser or Realtek's AirFun app (`com.realtek.airfun.client`), not from
  the touch screen. Its Network > WiFi Connection menu joins another Wi-Fi network (it then gets an
  address from that network and leaves its own), and System > Online Upgrade downloads and installs
  new firmware, which the author hoped would fix AirPlay and Android mirroring. That updates the
  module (inferred: the guide just says "HU"); this unit's Windows CE software is still the
  2016-01-11 build. The module's internet connection never reaches Windows CE. Nothing on the
  Windows CE side talks to the module either (Decoded): `Wifi.exe` imports no file, port or socket
  functions (14 `COREDLL` ordinals, no `CreateFileW`, no `WS2.dll`); `SkCom.dll`'s `WifiApi` is only
  the generic app shell (`remoteCallBack` at RVA `0xd320` goes straight to the shared receive code
  at `0xa5cc`, and it exports no send calls); `Main.exe` has no Wi-Fi command among its MCU events,
  only `SRC_WIFI` in the source list; and no USB network device was attached in the probe 0.3
  survey.
- **No network over Bluetooth** (see "Bluetooth"): the factory stack has no PAN or DUN profile.
  Dial-up over `COM4:`/`COM9:` would also need pairing and a stored dial-up entry (both registry
  changes), and iPhones and most current Android phones no longer offer dial-up.
- **USB networking is the only route.** The network software runs: `tcpstk.dll` (TCP/IP),
  `tcpip6.dll`, `AFD.Dll` and `ws2serv.dll` (Winsock; vendor code imports `WS2.dll`), `NDIS.Dll`,
  `PPP.Dll`, `autoras.dll`, `dhcpv6l.dll`, and `USB8023.DLL` is loaded for USB classes 2, 224 and
  239 (see "What the firmware cannot provide"). Android USB tethering (RNDIS, class 224) matches;
  iPhone tethering uses Apple's own USB protocol and will not. A board that joins a phone's Wi-Fi
  hotspot and shows up as a USB network adapter would also work. Probe 0.5's Survey lists the
  network adapters with a phone tethered; whether a connection works is still unknown.
- **For a program that fetches data:** load `WS2.dll` at run time (a missing import stops a CE exe
  from loading), and bring a TLS library (for example BearSSL) for HTTPS, because Windows CE 6's own
  secure connections are probably too old for most sites today (not checked on the unit). Plain
  HTTP needs neither.

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
  showed nothing reaches the bus when the wheel turns. Even if it does, `Main.exe` drops the CAN
  box frame that carries it in the common layout (`29`) and keeps nothing like it.
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

`dashboard/` (version 0.9.0, **built, not run on the car yet**; 0.1.0 ran, see "First run" below) is
the dashboard from the map-background design canvas
(https://claude.ai/artifact/FkFvhi7mSFDTMhACtCK7ST, private; versions 0.1 to 0.3 followed the earlier
column layout of https://claude.ai/artifact/9WDQyDaPa6RprDPPk3ST8x). It is a complete SD `Navigation`
folder: `Navigation.exe`, `fonts/` (Barlow), `README.txt`, and `TotalCommander/cecmd.exe` when Total
Commander was built. Since 0.9 the map is the vector map, in a separate `VMap` folder at the SD
card root, turned with the direction of travel up (see "Vector map in the dashboard" below; 0.2 to
0.8 showed the north-up picture map from the `Map` folder, see "Map"); the address file stays in
the `Map` folder (see "Address"). Besides the dashboard page there is a car info page with fixed facts about
the car (see "Car info page" below), opened from a button in the header, and a map page (0.7, see
"Map page" below), opened by tapping the car on the map. On a holiday a notification shows it at
the top of the map (0.8, see "Holiday notification" below).

Layout (800 x 480, `dashboard/src/render.c`): the offline map fills the whole screen, and every
widget sits on its own semi-transparent panel over it. The panels are tinted, not blurred, to spare
the processor (the user's rule): the panel colour is mixed into the map's pixels at 90 % by day and
at night (the user's choice for 0.5; 0.4 used 80 % and 74 %), with no shadows. A 40 px header runs across the top; side columns are 1/6 of the
screen wide (133 px): speed and elevation on the left, outside temperature, pressure and battery on
the right; the heading panel (bottom left) and the address panel (bottom right) are 2/6 wide
(266 px, 88 px tall); the zoom buttons, scale bar and map credit sit at the bottom centre, a
compass at the top right of the map (0.9; a north-up mark before). Icons are drawn on a pixel grid at the size they are
shown (20 px in the header, 14 to 16 px in panels): whole-pixel corners, 2 px strokes, slopes of
45 degrees or 2:1, so they stay recognizable on the low-resolution screen without smoothing.
Graphs are a line through every sample (straight pieces). 0.4 drew smooth waves instead: the
samples were averaged into 12 points and joined by a monotone cubic curve, but the averaging hid
short changes (a 1 hPa dip lasting a few samples moved the line 1 px instead of 2, a change of one
sample not at all), so the user saw flat lines and asked for lines again in 0.5. The lines still
looked flat on the car in 0.5.0 (elevation from 200 to 230 m drawn as a straight line): each graph
was laid over its full window (20 km, 30 min, 1 h, 3 h), so a short history sat in the last few
pixels at the right (the 80 elevation points are 1.4 px apart, so 4 points over the first 750 m
took 4 px) and the rest was the flat line drawn from the first value. Since 0.5.1 the samples there
are always span the whole width, oldest at the left edge and newest at the right; the note over a
time graph says what it covers until its window is full ("6 min", "1.5 h"), and over the elevation
graph how much road its points span ("450 m", "1.5 km") until it shows its full 3 km. A single
sample is a flat line across, and "Collecting data" shows only while there is none. The elevation
graph is a distance profile: a point every 150 m driven (counted from 3 km/h, like the average
speed, so a parked car adds nothing however the GPS height drifts), the last 20 kept, so it shows
the last 3 km of road (the user's choice in 0.5.3; 0.5.0 had a point every 250 m over 20 km). 0.5.2
tried a point per 10 m of climb or descent instead; the user preferred points by distance. While
the graph has only its first point, that point is taken again when the height source changes (the
car's sensor before GPS is ready, then GPS), so the graph does not start with a false step. The
height of a change was never the problem: a 30 m climb moves the elevation line about 38 px.

| Panel | Source | Rule |
|---|---|---|
| Pressure (right column), graph of 1 h | CarInfo `0x21` | sampled every 30 s; "weather" trend from sea-level pressure (needs GPS altitude) over at least 1 h, scaled to 3 h: 1 hPa = falling or rising, 3 hPa = falling fast |
| Elevation (left column), graph of the last 3 km | GPS altitude, 10-second average | car's barometric elevation plus the last GPS-minus-car difference when GPS has no fix ("Car sensor", or "Car" when that does not fit); climb rate over about a minute; a graph point every 150 m driven, the last 20 kept, the note saying how much road they span until it is 3 km |
| Outside (right column), graph of 3 h | CarInfo `0x31` | sampled every 90 s; change shown after 30 min |
| Heading (bottom left, 2/6) | GPS course above 6 km/h (back below 4 km/h) | otherwise the car's 8-point compass (`0x11`); a compass tape (2.4 px per degree, ticks every 5 degrees, the eight points as letters) under a fixed mark, then the point and the degrees ("Car compass": the point only) |
| Battery (right column), graph of 30 min | CarInfo `0x40` | 13.2 V and up charging, 12.2 to 13.2 V not charging, below 12.2 V low, 14.8 V and up too high (low and too high in the warning colour) |
| Map (whole screen, behind the panels) | GPS position and course, `\SD\VMap\*.vmap` | vector map turned with the GPS course up (0.9), the car at (400, 260), the middle below the header; it glides to each new GPS position and course over 1 s; below 4 km/h (heading from the car's compass) it keeps its last turn, north up until the car first moves; arrow along the GPS course while the heading comes from GPS, otherwise a dot (grey without a fix); zoom in half levels from the file's lowest data zoom to 19, starting at 16 (large print) and then at the last zoom chosen, saved as `map_zoom_tenths=` in `dashboard-settings.txt`; dark look at half brightness at night, light look by day, both in the same file. 0.2 to 0.8: north-up picture map from `\SD\Map\*.pack` and `Map\Day` |
| Address (bottom right, 2/6) | GPS position, `\SD\Map\*.addr` | nearest named road within 50 m (else "No named road nearby"), then one line: "Brgy." and the barangay (else "Near" and the nearest named place), a middle dot, city and province (the province left out when the line is too wide); looked up on its own thread after about 15 m or 30 s; the last address stays, marked "Last GPS position", without a fix; never logged |
| Speed (left column, top) | GPS | current speed large (a smaller font for three digits), average (counts only above 3 km/h) and top speed (highest median of three readings) under it, both since the dashboard started and not saved. The model still keeps the 10-minute speed series; 0.4 does not draw it |
| Header (semi-transparent, over the map) | GPS time, bus | clock: GPS UTC time and date (1024-week rollover corrected), kept running on the tick count when the signal drops; the unit's clock only when its year is 2020 or later. Time zone: the menu choice (`dashboard-settings.txt`), else UTC+8 (`DEFAULT_UTC_OFFSET_MIN` in `dash.c`, the user's choice). The model can also derive it from the unit's clock or time-zone setting, but `dash.c` always passes a zone because both are unreliable on this unit. Times are in 12-hour form with AM or PM (the user's choice, 0.5). GPS status ("Waiting for GPS" without a fix, its icon blinking 1 s on and 1 s off at the 1 s repaint, the user's choice in 0.5.3), "Headlights on" while the bus reports them (`0x52A` index 9 or `0x33` bit `0x04`), "No car data"; on the right sunset (with the time left; without it, or left out, when it would run into the items on the left) or sunrise from the last GPS position, then the car info button (an icon only: a car with an "i" badge) |
| Day and night colours | GPS position and time; bus | dark from sunset to sunrise (the next sun event is sunrise), light from sunrise to sunset, checked every 10 s with the sunset time; the headlights decide only until GPS has given time and position (the unit's clock resets to 2012). Logged as `[colours] night, from sunset and sunrise` (or `from the headlights`) when it changes |

Car info page (`render_carinfo` in `render.c`; 0.5 had 16 tiles in a 4 x 4 grid, 0.6 has categories):
opened with the button at the right end of the header, closed with Back at the top left; the same
header bar (Back, "Mitsubishi Montero Sport", "2016 · 2.4 diesel 4x2", the time on the right). The
body keeps the 4 columns of 0.5 (189 px, 8 px apart). The first is the side bar: one panel with a
stronger border (the user wanted it clearly apart) holding a button per category, 76 px tall, with a
20 px icon, the name and a one-line note; the chosen one is filled with the text colour (not the
accent, like the header button's badge), the others with the theme's button shade. The other three
columns hold the chosen category's tiles, 3 x 3, 135 px tall (at most 9 places, the user's limit),
each with a 24 px pixel-grid icon (the measured part or the fluid in the accent colour), a name, a
value and up to two detail lines. A tile with `rows` takes two places: the value and three lines on
the left, a table of named rows on the right. `dash.c` keeps the category while it runs
(`g_carinfo_cat`) and logs `[page] car info, category N` when it changes.

- General: plate NDE 4391 (the user's), diesel (Euro 4), tank 68 L, 2.4 L turbo (4N15, 181 PS,
  430 Nm), 8-speed automatic, 4x2 (rear-wheel drive, no low range), tyres 265/60 R18, pressure 29 /
  32 psi cold front / rear, gross weight 2,710 kg.
- Size and clearance: ground clearance 218 mm, wading 700 mm, height 1,805 mm, approach 30°,
  departure 24.2°, breakover 23.1°, width 1,815 mm, length 4,785 mm, turning circle 11.2 m.
- Fluids: engine oil 7.4 L (sump 7.0 + filter 0.3 + oil cooler 0.1) as a double-width tile with
  grade 5W-30 (10W-30 and 15W-40 also allowed), ACEA A1/B1, A3/B3, A3/B4 or A5/B5, API CD or better,
  change every 10,000 km or 8 months (synthetic) or 5,000 km or 4 months (mineral), genuine filter at
  every change; gearbox ATF-MA1 only, 12.8 L, 60,000 km or 48 months; rear differential GL-5 SAE 80,
  2.1 L, 30,000 km or 24 months; coolant long-life 50 % mix, 7.7 L, 45,000 km or 36 months; brake
  fluid DOT 3 or 4, replaced every 30,000 km or 24 months; power steering Mitsubishi PSF; washer
  3.5 L; A/C refrigerant HFC-134a, 770 to 810 g.

The dimensions are Mitsubishi's published ones for this model, mostly from Australian Pajero Sport
data because the Philippine sheet could not be read; tyre pressure and gross weight come from 4x4
and Australian sources and should be checked against the door-pillar sticker and the registration
papers, and the gearbox and tyre size depend on the trim (not confirmed). Fluid types and amounts
come from Mitsubishi Thailand's English owner's manual for the 2020 facelift (same 4N15 engine and
8-speed gearbox; no 2016 to 2019 manual is online), the change intervals from Mitsubishi Motors
Philippines' service guide for the Montero Sport (24MY and older), and the brake fluid change from
the Australian schedule (the Philippine guide only checks the level). Not settled: whether the
Philippine engine has a diesel particulate filter (DPF); if it does, the oil must be ACEA C2 or C3
instead. The page draws no map, so it costs less than the dashboard; the model keeps updating
behind it.

Map page (0.7, `render_mappage` in `render.c`, the view in `mapview.c`, **built and tested on the Mac
only**): the user asked for a full-screen map without the panels that a finger can drag. Tapping
the car on the dashboard's map opens it: a box of 100 x 100 px around the car (`CAR_HIT`), much
larger than the arrow (26 px) or dot (16 px), so a finger finds it in a moving car; it is clear of
the zoom buttons and the side columns. Only while the map picture is drawn (map files, a position
and the back buffer). The page draws the map across all 800 x 480 pixels with the car,
Back at the top left (the whole corner answers), the north-up mark (0.9: the compass) at the top right, the scale bar
at the bottom left (at the car's latitude: across Luzon the scale differs by 3 % at most), the map
credit at the bottom middle (the licences ask for it to stay visible), and the zoom buttons at the
bottom right; no header, no panels, so no tinting either. Rules, the user's choices of 2026-11-17
(they accepted the suggested defaults): the page has its own zoom, starting at the dashboard's,
so the dashboard is unchanged after Back; it follows the car (the car in the middle of the screen)
until a drag; after a drag the view stays where it was left and the car moves on it (it is not drawn
within 20 px of the screen's edge), and a "Centre on car" button (the GPS crosshair icon) appears
left of the zoom buttons; the view follows the car again after 30 s without a touch
(`MAPVIEW_IDLE_MS`); zooming keeps the middle of the screen (world pixels doubled or halved per
level). A press becomes a drag only after the finger has moved 10 px (`MAPVIEW_DRAG_PX`): touch
screens jitter by a few pixels and a tap must not move the map. The point under the finger stays
under it, measured from where the finger went down (no drift from adding up small steps). A dragged
view stays inside the box the map files cover at that zoom (`map_area`), so it never drifts into
empty grey. Two-finger zoom is not possible: Windows CE 6 has no gesture support, and the screen is
most likely single-touch (not checked on the unit).

Dragging uses `WM_MOUSEMOVE`, which Windows CE sends only while the finger is down; no new COREDLL
import was needed (the 0.7.0 import table is identical to 0.6.0's, checked with `objdump -p`), and
`SetCapture` is not used (the window fills the screen). Windows sends `WM_PAINT` only when no other
message waits, so the frame rate adapts to the unit. The risk is decoding new tiles while dragging:
every 256 px dragged brings in a row or column of 2 to 5 tiles, each read from the card and decoded
from PNG, and how long one takes on the unit is still unmeasured (see the open questions under
"Map" below). So on the map page `map_compose` reads and decodes at most 1 new tile per frame while
a finger drags (`MAP_DRAG_DECODES`) and 3 otherwise (`MAP_PAGE_DECODES`); the tiles left out are
drawn in the pack's background colour and counted in `MapSet.skipped`, and `dash.c` asks for another
frame until none are left. The dashboard page keeps no limit (as in 0.6). The tile cache grew from
20 to 30 tiles (3.75 MB) so a drag back finds the tiles it just left; if that much memory is not
free, 20 (`[map] N tiles kept in memory` in the log). At zoom 16 one tile is about 590 m across
around Luzon (2.3 m per pixel), so the screen shows about 1.8 x 1.1 km; looking further is cheaper
after zooming out. The log gets `[page] map, zoom N` when the page opens and, when it closes, the
frames drawn while dragging, the slowest of them (map, buttons and copy to the screen) and the tiles
decoded; the 10-minute `[map]` line adds how often the page was opened and the slowest frame while
dragging since the start. The view's middle is a position, so it is never logged.

Since 0.9 the page shows the vector map: while it follows the car it is turned like the dashboard
(the GPS course up), and a drag keeps the turn it had when the finger went down (the finger's move
is turned back to north up before it moves the middle, so the point under the finger stays under
it at any turn); "Centre on car" and the 30 s rule bring back the turn with the car. The tile
decode limits above (`MAP_DRAG_DECODES`, `MAP_PAGE_DECODES`, the 30-tile cache) went with the
picture map; see "Vector map in the dashboard".

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
how long the GDI repaint takes once a second (0.4 composes the whole 800 x 480 map and tints the
panels every second; the copy of the 800 x 480 back buffer alone took about 40 ms in the vector
map test); whether the unit's time-zone setting is right.

### Map (dashboard 0.2 to 0.8, **built and tested on the Mac only**; replaced by the vector map in 0.9)

Since dashboard 0.9 this picture map is no longer shown (see "Vector map in the dashboard"); the
build tools, the `Map` folder and `map.c` stay (the address file and the pilot's data come from
the same build, and `map.c`'s tests still run).

The user wants to see where the car is, with no routing (the factory GenieTek `Navigator.exe` has a
Philippine map from 2018-01-12 that can no longer be updated). The map is made on the Mac from
OpenStreetMap data with `tools/map/` and stored as **Montero tile packs** (`*.pack`, format in
`tools/map/mtpack.py`): one zoom level and one rectangle of 256 x 256 PNG tiles per file
(OpenStreetMap tile numbering, Web Mercator), an index of offset and length per tile, identical tiles
stored once, each file below 2,000,000,000 bytes for FAT32 and 31-bit file offsets. The dashboard
opens every `*.pack` in the `Map` folder beside its own folder (`\SD\Map\`) read-only, and
`dashboard/src/map.c` reads the index entry and tile bytes on demand, decodes PNG with stb_image
(`dashboard/src/pngdec.c`, PNG only) and keeps decoded tiles (RGB565, 128 KB each). Since 0.4 the
back buffer itself is an 800 x 480 RGB565 DIB section (bottom-up): the map is composed straight
into its pixels across the whole screen, the panels are tinted into the same pixels
(`map_tint`: the panel colour mixed in with a 5-bit alpha, the three channels at once, rounded
corners worked out per row with whole numbers), and GDI then draws the text and shapes on the same
bitmap. The full screen needs up to 15 tiles at once (5 across, 3 down), so 20 are kept (2.5 MB).
`CreateDIBSection` is looked up at run time like the other optional GDI functions; without it the
back buffer is an ordinary bitmap, with no map picture and solid panels. Packs in `\SD\Map\` are
the dark map, packs in `\SD\Map\Day\` the light one; one `MapSet` holds both, tagged by style, the
tile cache is keyed by style too, and `map_use_style` switches when the colours change (the zoom
range follows the style in use). The 0.4 build imports exactly the same COREDLL functions as 0.3.
Tests on the Mac: `tests/test_map_reader.py` (decoding, pixel placement, empty, corrupt and
missing tiles, night, cache, bottom-up bitmaps, styles, tinting) and SVG previews with the
composed map embedded.

Making the map (`tools/map/build_map.sh`): planetiler 0.10.2 turns the Geofabrik Philippines
extract (plus OpenStreetMap water polygons, Natural Earth and lake centerlines, about 2 GB of
downloads) into OpenMapTiles vector tiles for the Luzon box `119.4,12.4,124.5,18.8` (about 5 min;
reading the whole extract dominates, so a smaller box hardly saves time). tileserver-gl 5.7.0
draws 8-bit palette PNG tiles from them with our own styles (`tools/map/style/`, written by
`make_style.py`; the light one has the same layers with the LIGHT palette and is built with
`MAP_STYLE=light`, into `dist/map/Map/Day`, through its own tile server configuration
`tileserver/config-light.json` so the dark map's tile cache stays valid): roads by class, tracks
dashed ochre, paths dotted, water blue, peaks with heights;
unnamed tracks and paths from zoom 14, buildings only at 15. `build_map.py` fetches the tiles
from that local server only, skips open sea below sea parents from zoom 11 (a one-colour tile is
reused; tiny islets far from a coast could be missed), caches rendered tiles by a fingerprint of
style and data, and writes one pack per zoom level (column bands when one would pass the size
limit). Test area east of Manila (zoom 8 to 15, 234 tiles): about 65 tiles/s, 7.5 KB per tile;
dense Manila about 22 tiles/s and 32 KB per tile. The first Luzon build (2026-11-15, data up to
2026-11-13) took 12 min with the vector tiles already made: zoom 8 to 14 in 7 packs, 244 MB; zoom
15 in one pack of 416 MB (281,790 tiles, 102,729 rendered, 178,833 by the sea shortcut, about
219 rendered tiles/s); 632 MB in all. The first light map (2026-11-15, same data): zoom 8 to 14 in
7 packs, 243.1 MB, 4 min (40,655 tiles rendered at about 166 per second, 54,034 by the sea
shortcut); its zoom 15 in one pack of 414.0 MB, 8 min (102,957 rendered at about 206 per second,
178,833 by the sea shortcut); 657 MB in all. The licences ask for "© OpenMapTiles
© OpenStreetMap contributors" to be visible; the map shows "© OpenMapTiles © OpenStreetMap".
No contour lines or hill shading yet (the OpenMapTiles schema has no elevation).

Zoom 16 is drawn in **large print** (the user's choice, 2026-11-17) and is the level the map
starts at: the map twice as close as at zoom 15, with text, lines, dots and label halos twice
their zoom 15 size, so it reads like zoom 15 enlarged but stays sharp. Enlarging zoom 15 on the
unit (each pixel drawn as 2 x 2) was compared in previews first and turned down as blocky.
`make_style.py` gives line widths and dot sizes a zoom 16 stop (`grow`) and each label layer a
`-large` copy from zoom 16, the normal one stopping there. Labels need the copy because the
renderer places them using their size one zoom level further in (and at style zoom 18 for the
spacing along roads), so a zoom 16 size in the same layer would move or drop zoom 15 labels. Check:
300 busy tiles at zoom 13 to 15 came out byte-identical from the old and the new style. Build
(2026-11-17, same data): 1,125,019 tiles per style, 383,705 rendered (about 250 per second) and
741,314 by the sea shortcut; dark 1,054.2 MB in 25 min, light 1,061.1 MB in 26 min, one pack each.
A style change starts a fresh tile cache and the sea shortcut reads zoom 15 from that cache, so zoom
8 to 15 were rendered again first (13 and 14 min, packed into a throwaway folder; the zoom 8 to 15
files in `dist/map` are still those of 2026-11-15). The `Map` folder now holds 1.71 GB (dark), its
`Day` folder 1.72 GB (light), 3.46 GB with the address file. The dashboard needs no change for it:
it starts at the highest level the files hold, so 16 once that pack is on the card (checked in the
previews through `map.c` with the real files, not yet on the car).

The log gets `[map] N dark and M light (Day folder) map files, zoom A to B` at start, `[map] light
map` or `dark map` when the style changes, and, every 10 minutes, the zoom, tiles decoded,
failures, read errors and the slowest map draw in ms. Open questions for the first run: whether
`CreateDIBSection` with `BI_BITFIELDS` 565 works on this unit (now also as the back buffer that GDI
draws text on; the vector map test's identical back buffer worked on the car, 2026-11-18, and its
copy to the screen took about 40 ms, see "Vector map pilot"), how long a tile takes to decode from the SD card (the first draw after a zoom or
style change decodes up to 15), how long composing and tinting the full screen takes, whether
the map and the see-through panels are readable in sunlight, and (0.7) whether dragging the map
page feels smooth: the `[page] dashboard (map page: ...)` line gives the slowest frame while
dragging, which tells whether `MAP_DRAG_DECODES` and the 10 px drag allowance need changing.

### Vector map pilot (first car run 2026-11-18, **Seen**)

The user asked (2026-11-17) for a map that works like a map app: roads, names and markers drawn
by the unit itself, rotation, and zooming between levels, instead of ready-made pictures. They
chose full vector drawing, including roads, and a pilot over one small area, Dasmarinas City,
Cavite, to find out how hard it is and how fast it runs. The picture map (`dist/map/`, `\SD\Map\`)
and the dashboard are untouched: the pilot has its own data folder and its own test program.

Data (`tools/vmap/build_vmap.py`): the OpenMapTiles vector tiles that `build_map.sh` already made
(`dist/map-build/tiles/openmaptiles.mbtiles`, zoom 0 to 14) are decoded (`mvt.py`), each feature
is matched against the rules compiled from `make_style.py`'s two styles (filters, colours of both
palettes, line widths, dot sizes and text sizes for every 1/8 zoom from 8 to 19; widths keep
growing by 1.4 per level past zoom 16 so roads stay road-like close up; the large print label
copies are folded in, text growing from its zoom 15 size at 15 to the large size at 16), cut
exactly at its tile's edge (areas, lines, and area outlines without the cut edges; points kept
only in their own tile) and split into an 8 x 8 grid per tile, because the source merges hundreds
of buildings or streets into one feature and the unit could then not skip what is off screen.
Output: `dist/vmap/VMap/dasmarinas.vmap`, bounds 120.89,14.23,121.02,14.39, data zooms 10 to 14
(86 tiles, 56 of them at zoom 14), 58 drawing rules, 6.26 MB (11.1 MB uncompressed), about 15 s.
For comparison the picture map holds 3.46 GB for Luzon; the vector tiles for all of Luzon are
253 MB, so a Luzon `.vmap` would be roughly 300 to 400 MB (estimate). Format: `mtvmap.py`
(header, rules, per data zoom a tile index, zlib-compressed tiles of features, parts, 16-bit
points in tile units, draw lists per rule and UTF-8 names).

Drawing (`dashboard/src/vraster.c`, `vfont.c`, `vmap.c`, portable C, no floating-point maths in
the inner loops): areas and lines are filled with smooth (anti-aliased) edges by the cell method
of FreeType and AGG, every tile of a rule in one pass so tiles join without seams; cells go into a
full-screen grid with a bitmap of touched cells (3 MB for 800 x 480) instead of a sorted list,
which halved the time. Lines become outlines: a box per segment, a wedge or rounded wedge on the
outer side of each bend, half discs for round ends (left out under 2 px wide), dashes cut first.
Labels: letters are read from `Barlow-Medium.ttf` as outlines (composite letters such as n with a
tilde included) and filled the same way, with a halo; place names stay upright, road and river
names follow their line letter by letter and are never upside down; the most important rules
place first, nothing overlaps (8 px grid), and the same name is not repeated nearby. The view is a
Mercator centre, any zoom, any bearing (the direction pointing up), dark or light, dimmed or not.
The new COREDLL imports are none: `VMapTest.exe` imports 62 functions, all among the dashboard's
(`tools/build_vmaptest.sh` writes both lists); `DeleteDC`, `DrawTextW` and `wcscat` were taken
out for that.

Previews (`tests/test_vmap.py`, `VMAP_PREVIEW_DIR`): the pictures match the picture map's styles;
road names bend round roundabouts, labels stay upright at any bearing, the day and night maps
both work. Speed on the Mac (Apple Silicon, one core; best of 5): zoom 16 about 6 ms, zoom 17
turned 35 degrees 3 ms, zoom 14 19 ms (7,491 features from 20 tiles), zoom 12 9 ms. Processor
instructions per frame (from `/usr/bin/time -l`): zoom 16 73 million, zoom 17 44 million, zoom 14
200 million, zoom 12 102 million. The head unit's processor is not known exactly (a SiRF Atlas
family chip, ARM9 or ARM11 class, probably 400 to 600 MHz, no floating-point unit, no divide
instruction); at one to two cycles per instruction that is very roughly 0.15 to 0.3 s per frame
at zoom 16 and 17 and 0.5 to 1 s at zoom 14. **Estimate only**: `VMapTest.exe` measures it.

`VMapTest.exe` (`vmaptest/`, version 0.1.0): copy `dist/vmaptest/VMapTest` and `dist/vmap/VMap` to
the SD card root, start it from Total Commander. It logs free memory, then runs a series of 12
views over the middle of the area (zoom 12 to 18, 16.5, turned 30 degrees, dark and dimmed, no
labels), each drawn 3 times, logging per view the time to draw (tiles read, areas, lines, labels),
the copy to the screen, features, cells, labels, and the three costliest rules. Then a finger drags
the map, and buttons zoom by half a level, turn by 15 degrees, spin once round (24 frames, average
and slowest), switch day and night, labels, rerun the series, and exit. The log has no positions;
the test reads nothing from the car, the bus or the GPS.

First car run (`vmaptest-log.txt`, 2026-11-18, two starts, the series run four times; **Seen**):
it starts from Total Commander, finds `\SD\VMap\dasmarinas.vmap`, loads the font, and the 16-bit
`CreateDIBSection` back buffer (`BI_BITFIELDS` 565, bottom-up) works, as the dashboard 0.2 and
later assume. **Memory: 413,484 KB of RAM, 348,580 KB free** at start (about 1 MB used after
opening the map), far more than the dashboard's caches assumed. Drawing times, light map with
labels, after the tiles are in memory (the four series agree within about 10 %):

| Zoom | Draw | Features | Cells |
|---|---|---|---|
| 12 | 265 ms | 8,665 | 450,000 |
| 13 | 570 ms | 10,509 | 1,050,000 |
| 14 | 525 ms (the same turned 30 degrees) | 7,422 | 853,000 |
| 15 | 395 ms | 2,295 | 627,000 |
| 16 | 195 ms (turned 30 degrees, dark: the same; no labels 170 ms) | 516 | 299,000 |
| 16.5 | 142 ms | 313 | 189,000 |
| 17 | 103 ms | 111 | 124,000 |
| 17.5 | 80 to 100 ms (any bearing) | 84 | 87,000 |
| 18 | 56 ms | 42 | 58,000 |

Copying the 800 x 480 picture to the screen (`BitBlt` from the DIB section) takes **about 40 ms
every frame** (768 KB, about 19 MB/s), on top of the drawing: likely a format conversion or the
bottom-up row order; not yet checked. Reading and inflating tiles: the first draw at zoom 14 (15
new tiles) took 870 to 890 ms instead of 525, about 23 ms per zoom 14 tile. Turning costs nothing
extra, labels about 25 ms. The costliest rules: minor street casings and minor streets (#23, #31:
160 and 90 to 145 ms each at zoom 13 and 14), buildings (#12: 40 ms at 16, 93 to 117 ms at 15),
dotted paths (#20, 33 ms at 14). Dragging at zoom 16: 214 to 296 ms a frame (3 to 4 frames a
second with the copy); at 17.5: 81 to 97 ms (about 7 frames a second). Spin at 16: 216 to 277 ms
on average, slowest 395 ms. The unit runs about 30 times slower than one Mac core (zoom 16: 5.8
against 195 ms), about 375 million Mac instructions' worth a second: faster than the estimate
above, so the processor is probably newer than ARM9 (not identified; `KernelIoControl` with
`IOCTL_PROCESSOR_INFORMATION` would name it). The colours looked right on the screen (the user,
2026-11-18).

What it means: the unit can draw a full vector map, turned, from zoom 16 in 0.1 to 0.2 s plus
the copy, so following the GPS heading up with one redraw a second works now; zoom 13 to 15 takes
0.4 to 0.6 s. Smooth dragging and turning do not work by redrawing (3 to 4 frames a second at
zoom 16). The user's rule (2026-11-18): no simpler drawing, every detail stays at every zoom; use
the memory for speed instead.

Speed from memory (`VMapTest.exe` 0.2, **built and tested on the Mac only**):
- Map pictures (`dashboard/src/vtiles.c`): the map is drawn north up, without labels, into
  256 x 256 RGB565 pictures (128 KB) by the same `vmap_draw` (shapes only), up to 480 kept
  (61 MB, allocated as needed, least recently used dropped but never one the last frame used). A
  frame copies them onto the screen: rows straight across when not turned or scaled, otherwise
  16 x 16 pixel blocks mapped back into the pictures with whole-number steps, the nearest picture
  pixel ("Fast") or the four nearest mixed ("Smooth"). A picture not drawn yet is taken from the
  zoom shown before, scaled, or left in the land colour; pictures outside the file's area are
  land. Missing pictures are drawn one at a time while nothing else happens (a 1 ms timer, whose
  messages come only when the queue is empty): the screen first, nearest the middle first, then
  two rings beyond it, then the screen at zoom +0.5 and -0.5; while a finger drags, one per frame.
  Mac checks: north-up frames from pictures equal the full draw pixel for pixel when the view's
  middle sits on a whole pixel; turned 30 degrees, 2.6 % of pixels differ (softer edges). Mac
  timings: a frame from pictures 0.03 ms north up, 0.35 ms turned fast, 1.4 ms smooth, against
  6.6 ms for the full draw at zoom 16; a picture takes 1.25 ms at zoom 16 and 3.6 ms at zoom 14
  (12 for a screen: about twice a full draw, done once). At the measured factor of about 30 that
  would be very roughly 1 to 40 ms per frame on the unit, plus labels and the copy.
- Letter pictures (`vmap.c`): each letter is filled once per size (1/4 px), angle (128 steps of
  2.8 degrees), halo and half-pixel position, with its halo grown from it, and kept (4,096 slots);
  labels then only mix kept pictures in. Mac: labels 30 to 45 % faster with the cache full.
- The whole Dasmarinas file stays decoded (128 data tiles kept).
- The copy: 0.2 times four ways on the unit and keeps the fastest: `BitBlt` from the bottom-up DIB
  (0.1's way), from a top-down DIB, `SetDIBitsToDevice`, and writing straight into the screen
  memory (`ExtEscape GETRAWFRAMEBUFFER`, used only for an 800 x 480 RGB565 screen in the program's
  reach and only while its window is in front). It also logs bits per pixel, the processor
  (`GetSystemInfo`, `KernelIoControl` `IOCTL_PROCESSOR_INFORMATION`) and uses
  `QueryPerformanceCounter` for timings under a millisecond when the unit has it. Every function
  the dashboard does not import is looked up with `GetProcAddressW`; the import table is still the
  same 62 dashboard functions.
- A turned view is drawn again in full 0.3 s after it stops moving ("sharp"); north up needs no
  redraw (the pictures are exact).
- New buttons: Drive (15 s at 50 km/h, heading up, turning 6 degrees a second, frames as fast as
  they come), Smooth/Fast, Pictures/Direct. The series now times per zoom (12, 14 to 18) the full
  draw, the pictures for the screen from none, frames from pictures north up and turned, labels
  with an empty and a full letter cache, then spins (smooth and fast), a drag over drawn and
  undrawn ground, and 10 s of driving at zoom 17.

Second car run (`VMapTest.exe` 0.2, 2026-11-18, **Seen**): the processor is a **Cortex-A9**
(`KernelIoControl`: core "Cortex-A9", name "TT6X0A", vendor "SiRF"; clock not given; one core;
`GetSystemInfo` type 2577, level 4, revision 6), so ARMv7-A; the dashboard and probe are built
for ARMv4T. The **screen has 32 bits per pixel** (`GetDeviceCaps`); the driver offers no screen
memory (`GETRAWFRAMEBUFFER`). Copying the 16-bit frame: bottom-up `BitBlt` 41 ms, top-down 40 ms,
`SetDIBitsToDevice` 43 ms: the driver converts 16 to 32 bits while copying, which explains
0.1's 40 ms. Memory: 350 MB free at start, 326 MB after the series (72 pictures kept). Map
pictures, from none: zoom 12 64 ms each, 14 85 ms, 15 57 ms, 16 29 ms, 17 12 ms, 18 7 ms (12 to
16 for a screen). Frames from pictures: north up 2 to 6 ms, turned 11 ms (Fast) or 30 to 37 ms
(Smooth); labels 13 to 40 ms (17 to 24 at zoom 16, with the letter cache full); copy 39 to 45 ms.
Spin at 16: 77 ms a frame Fast (12.9 a second), 106 ms Smooth (9.4). Drag at 16: 63 to 71 ms
Fast without or with labels (14 to 15.7 a second), 118 ms Smooth; the same drag drawn in full
every frame (0.1's way, "Direct"): 291 to 306 ms (3.3 a second). Drive at 16: 80 ms Fast (12.4
a second), 103 ms Smooth; Direct 269 ms (3.7). The sharp redraw after turning: 215 to 320 ms.
Light map, dark map and no labels behave the same. After the switch to the dark look, 14
pictures were missing for a moment (drawn within a second).

What 0.3 changes (**built and tested on the Mac only**), still without leaving out any detail:
the copy test adds a 32-bit top-down DIB section (`BI_RGB`, 0x00RRGGBB) and `SetDIBitsToDevice`
from it, and draws frames in 32 bits when that copies fastest (`VSurface.px32`: the filler, the
letter mixing, the land fill and the compositor all write 32 bits; map pictures stay 16-bit,
widened while composed, so a frame from 16 and 32-bit agree exactly); the screen-memory way is
gone. Labels: while the map moves, the last placement is kept and moved and turned with it
(`VM_DRAW_LABELS_KEPT`; placed afresh after 20 degrees or 120 pixels, and once the map rests);
letters are looked up in the font once and kept (a table of 12,288 code points), letter pictures
sit in groups of four slots so letters used together no longer evict each other, and road
directions are worked out once per road. Mac: labels placed afresh 0.29 to 0.61 ms (0.42 to 1.15
before), kept 0.05 to 0.19 ms. The exe is built twice: `VMapTest.exe` for ARMv7-A (`-march=armv7-a
-mtune=cortex-a9 -mno-unaligned-access -mword-relocations`, ARM mode, no floating-point or NEON
instructions; the bit scan of the filler becomes count-leading-zeros; `-mword-relocations` because
the Windows CE object format cannot hold MOVW/MOVT address relocations) and
`VMapTest-ARMv4.exe` as before, in case the first does not start. Both import the same 62
dashboard functions.

Third car run (`VMapTest.exe` 0.3, 2026-11-18, both builds, ARMv4 first; **Seen**): both start.
32-bit frames copy in **8.9 to 9.3 ms** (`BitBlt` or `SetDIBitsToDevice` from a 32-bit top-down
DIB) instead of 40 ms. The ARMv7 build draws 5 to 10 % faster (full draw at zoom 16 175 against
189 ms, pictures 27 against 29 ms each). Frames from pictures, ARMv7: north up 8 to 16 ms (widening
to 32 bits), turned 19 ms Fast and 41 to 50 ms Smooth; labels kept while moving 1 to 4 ms, placed
afresh 6 to 30 ms. Drag north up at zoom 16: 28 to 33 ms a frame, **30 to 35 frames a second**;
drag turned 75 degrees: 41 to 45 ms Fast (22 to 24 a second), 73 ms Smooth (13.5); spin 44 ms
Fast (22), 70 ms Smooth (14); drive heading up 37 ms Fast (26 to 27 a second), 66 ms Smooth (15).
About 7 ms of each frame was the test's button bar. The user wants Smooth turning (2026-11-18).

What 0.4 changes (**built and tested on the Mac only**), every detail still drawn:
- Map pictures hold one more row and column (257 x 257, copied from the right and lower
  neighbours), so mixing four pixels never needs a second picture; composing goes row by row in
  runs that stay inside one picture (no slow per-pixel path except where a picture is missing).
- Smooth turning with NEON (`dashboard/src/vtiles_neon.c`): eight pixels at a time, the four
  picture pixels of each loaded into vector lanes, the per-colour mixing and the widening to 32 bits
  vectorised; the same integer formula as the plain code, so frames are identical (tested on the
  Mac, whose processor has NEON too). Mac: a smooth turned frame 1.6 to 1.8 ms plain, 0.6 ms with
  NEON. Only the ARMv7 exe has it (`-mfpu=neon -mfloat-abi=softfp`; CeGCC 9.3 crashes on the NEON
  "by scalar" intrinsics such as `vmulq_n_u16`, so only vector-by-vector ones are used). Because a
  NEON instruction the system does not allow would stop the program, it starts itself again with
  `--neon-check`; that helper runs `vt_neon_selftest` and ends with 77 when every pixel agrees; only
  then does the program use NEON (`WaitForSingleObject`, `GetExitCodeProcess` looked up at run time).
- The copy test adds `SetDIBitsToDevice` from ordinary memory, and the series compares drawing a
  smooth turned frame into the DIB section and into ordinary memory, keeping the quicker pair.
- The other look (day or night) is drawn ahead while idle; the button bar is drawn once and copied.
- **Place markers** (the category highlight wish): `tools/vmap/build_vmap.py` `MARKERS` picks
  OpenMapTiles poi points by class and subclass into 13 marker rules (layer kind 6, the pictogram
  number in the layer record's last field): fuel, hospital, mall (shop=mall and department stores),
  police from zoom 15; parking, clinic, supermarket (and marketplace), pharmacy, bank and ATM, car
  repair and parts, hotel (hotel, motel, guest house, hostel) from 16; convenience store and food
  (fast food, restaurant) from 17. Names beside them from zoom 16 for fuel, hospital, mall and
  police (as the style named fuel stations), from 17 for the rest, in the badge colour (lighter on
  the dark map). The style's fuel dot and fuel name are replaced. Counts in the Dasmarinas area
  (zoom 14 data): fuel 179, parking 790, mall 33 plus 16 department stores, hospital 30 and clinic
  70, police 56, pharmacy 115, bank 95 and ATM 82, supermarket 87 and marketplace 27, convenience
  550, fast food 361 and restaurant 304, car repair 79 and parts 27, hotels and the like 47.
  Drawing (`vmap.c`): a 26 px badge in the place's colour inside a 2 px ring in the land colour, the
  16 x 16 pictogram (`dashboard/src/vicons.c`, drawn on the pixel grid, the user's icon rule) in
  white, centred on a whole pixel so it stays sharp; upright at any bearing; placed with the labels
  (after place names, before peaks and road names, by the poi rank), never overlapping; the name
  only when it fits; kept with the labels while the map moves; dimmed with the night map.

Fourth car run (`VMapTest.exe` 0.4, 2026-11-18, **Seen**): **no NEON**: the helper started with
`--neon-check` ended with code `0xC000001D` (illegal instruction), so this Cortex-A9 has no NEON
unit or the system does not enable it (the check took about 5 s; the main program carried on).
NEON was then taken out again (0.5). Drawing a smooth turned frame into ordinary memory
(`malloc`) took 51 ms against 35 ms into the 32-bit DIB section, so frames stay in the DIB
section; copying either takes about 9 ms. With the bordered pictures and runs (no NEON): turned
frames 15 to 17 ms Fast and 26 to 36 ms Smooth (41 to 50 in 0.3); spin at zoom 16 60 ms Smooth
(16.5 a second), 40 ms Fast (24.5); drive heading up 52 to 55 ms Smooth (17.9 to 19.2 a second);
drag north up 22 to 28 ms (35 to 44 a second). Place markers showed (7 to 9 at zoom 15 and 16 in
the series' views). Zooming through several half levels quickly leaves up to 12 pictures missing
for a moment (drawn from the zoom before until ready). Labels placed afresh at zoom 14 can take
up to 57 ms (only when the map comes to rest).

0.5 (**built and tested on the Mac only**): turned or scaled frames are composed in chunks of
screen pixels (64 x 16 by default) instead of whole rows, so the picture rows a chunk crosses
stay in the processor's cache from one screen row to the next (a whole turned row crosses
hundreds of picture rows). The series times rows, 64 x 16, 32 x 32, 128 x 8 and 32 x 16 on the
unit and keeps the quickest (on the Mac, whose caches are far larger, rows are slightly quicker).
Widening 16-bit pixels to 32 bits goes through two tables of 256 entries (high and low byte)
instead of eleven steps; the same result as `VR_565_TO_32`. Mac: a smooth turned frame 1.3 ms
(1.6 to 1.8 before).

More place markers (2026-11-18, the user: "add more markers"; they confirmed the first list, that
markers win over road names in busy places, and the zoom levels, and found Smooth turning smooth
enough): 16 more kinds, 29 marker rules and 28 pictograms in all. Toll booth from zoom 16 (in this
data `toll_booth` also marks car park pay booths at the malls, so not from 15), campsite and picnic
site from 15 (useful off-road); church (`place_of_worship`), school (school, kindergarten, college,
university), town or barangay hall (`town_hall`, including community centres), attraction
(attraction, viewpoint, museum, monument), bus station (not bus stops), fire station from 16;
café, bakery, hardware (and DIY), motorcycle shop, post office, dentist, toilets, drinking water
from 17. Names from 17 (toll and campsite from 16). Colours by family: transport blue, shopping
teal (malls purple), food raspberry, health red (pharmacies green), money and post gold, cars
slate, landmarks brown (so hotels moved from brown to violet), outdoors olive, water cyan.
Dasmarinas counts: churches 245, schools 294 and colleges 40, halls 163, attractions about 46,
toll booths 36, bus stations 27, fire stations 10, cafés 155, bakeries 104, hardware 129,
motorcycle shops 77, post offices 19, dentists 43, toilets 182, drinking water 23, campsites and
picnic sites about 16. `VM_MAX_LAYERS` went from 96 to 128 (the file has 85 rules).

Still to do for a full map after that:
Luzon data (zoom 8 to 14, about 70,000 tiles at zoom 14), label stability while moving, and the
category highlight wish (a style rule here). The map in the dashboard and its map page, heading
up with the GPS glide, came in dashboard 0.9 (next section).

### Vector map in the dashboard (0.9, **built and tested on the Mac only**)

The user's request (2026-11-18): replace the dashboard's picture map with the vector map, as the
dashboard's background, following the heading; the map page lets a finger drag it; remember the
zoom; no Luzon build yet (the pilot file covers Dasmarinas only). `dashboard/src/dash.c` opens the
first readable `*.vmap` in `\SD\VMap\` (skipping `._` files), reads `fonts\Barlow-Medium.ttf` for the
names, and draws with `vmap.c` and `vtiles.c`, the pilot's renderer unchanged. `map.c` is no longer
compiled into the dashboard; `luzon.addr` is still read from `\SD\Map\`.

- Heading up: after each model update, the map's view (position at the car and the direction up the
  screen) glides from where it is shown to the newest GPS position and course over 1 s
  (`GLIDE_MS`), the turn the short way round. The course is used only while the heading comes from
  GPS (above 6 km/h, back below 4 km/h); with the car's 8-point compass the map keeps its last
  turn, north up until the first GPS course. Changes smaller than 0.3 px and 0.5 degree are not
  glided; below 2 km/h position changes under 4 px are ignored, so a parked car's wandering GPS
  position does not keep the map moving. The car's arrow is drawn at the heading minus the map's
  turn, so it points up once the glide ends.
- Frames while the map moves come from the map pictures (`vt_compose`, Smooth turning) with the
  kept labels (`VM_DRAW_LABELS_KEPT`), on a timer (`TIMER_ANIM`) every 100 ms at most; when a frame
  takes longer, the timer moves out to 1.5 times its time (`glide_pace`), so a third of the time
  stays for GPS, the bus and the address thread. At rest the map is drawn once in full
  (`vmap_render`, labels placed afresh) and that picture is kept (3 MB) and reused by the 1 s
  repaint until the view changes. Frames wait while the holiday notification moves.
- Missing pictures are drawn on a 1 ms timer (`TIMER_MAPWORK`), one per message, nearest first:
  the screen, a ring around it, half a zoom level in and out, then the other look (day or night)
  for the switch at sunset or sunrise. Up to 400 pictures (52 MB; 348 MB were free on the unit)
  or, without that much memory, 120; 128 decoded file tiles.
- The back buffer is now a 32-bit top-down DIB section (the unit's screen is 32 bits per pixel;
  in the pilot the copy of a 32-bit buffer took 9 ms against 40 ms for 16-bit), the panels are
  tinted into it with `vr_tint` (the old `map_tint` for 32-bit pixels), and the holiday
  notification's saved copy is 32-bit too.
- Zoom: + and - move half a level (`MAP_ZOOM_STEP`) between the file's lowest data zoom (10 for the
  pilot) and 19 (`MAP_TOP_ZOOM`). The dashboard's zoom is saved at each press as
  `map_zoom_tenths=` in `dashboard-settings.txt` (beside the time zone) and used at the next
  start (the user's request); without it the map starts at 16 (`MAP_DEFAULT_ZOOM`). The map
  page's zoom is its own and not saved. The scale bar works at any zoom.
- The compass (44 px, where the north-up mark was) turns with the map: the north half in the
  graph colour, the south half muted, "N" 16 px out from the middle.
- Messages: "No map file" ("Copy the VMap folder to the SD card") without a readable file, "No map
  for this area" when the car is outside the file's box.

Luzon file (2026-11-18, the user: "build entire luzon now", with the builder unchanged):
`python3 tools/vmap/build_vmap.py --bounds 119.4,12.4,124.5,18.8 --name luzon` (the picture map's
Luzon box, data zooms 10 to 14, vector tiles of 2026-11-13) wrote `dist/vmap/VMap/luzon.vmap` in
6 min 55 s on one core of the M1 Pro: 227.2 MB (372.6 MB before compression), 94,180 tiles (320,
1,140, 4,484, 17,782 and 70,454 per zoom). Header and tile index 1.12 MB (`vmap_open` allows 4
MB); the largest tile 932 KB unpacked (Quezon City, 1,258 features; the reader allows 8 MB, the
format 65,535 features), Dasmarinas's largest 604 KB; zoom 14 tiles average 3.9 KB. Checks on the
Mac: Dasmarinas draws pixel for pixel the same from both files; at zoom 16 and 14, Manila, Quezon
City and Baguio take about as long as Dasmarinas (6 to 9 ms and 13 to 16 ms, best of 3), the
countryside and coast a fraction of that, so the car's Dasmarinas figures should hold in the
cities; dashboard previews in Manila and Baguio show the map, markers and the address. Before the
build a sample of 1,200 random tiles gave about 6 minutes and the data size about 10. Both the
dashboard and `VMapTest.exe` open the first `*.vmap` the card's folder listing returns (the copy
order on FAT, not the name), so `\SD\VMap` should hold one file; VMapTest centres its views on the
middle of the file's box, which for Luzon is near the Aurora coast, so its series needs
`dasmarinas.vmap` alone to compare with the earlier runs. Not run on the car yet.

The 0.9.0 build imports the same 105 COREDLL functions as 0.8.0 (`objdump -p`), with no compiler
warnings. Mac tests: `tests/test_dashboard.py` (the drawing calls: map view passed with position,
zoom, turn and car point; scale bar at fractional zooms; the map page's drag at a turn, frozen turn,
car on the dragged view, box limit; "No map file") and
`tests/test_vmap.py` `DashboardMapTests` (the dashboard preview with the real Dasmarinas file,
heading up, by day and night). Log: at start `[map] vector map: N rules, M data zooms, zoom A to
B, showing Z (remembered); P pictures at most`; every 10 minutes `[map] zoom Z | moving: frames,
average and slowest ms; at rest: sharp drawings, average and slowest ms | pictures drawn and kept |
map page opened, slowest frame while dragging`. No position. Not known yet: the frame times on
the car while driving (the pilot's Drive series gave 52 to 55 ms Smooth at zoom 16, without the
panels and the GDI text), how often a glide finds pictures missing at 1 frame per GPS update, and
whether the glide looks smooth at 10 frames a second.

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

### Holiday notification (dashboard 0.8, **built and tested on the Mac only**)

The user's request (2026-11-18): at start-up, when today is a national holiday, show it like a
phone's "island" notification (icon, name, long date, a note when it is a non-working day); once
GPS gives the location, add the regional one; after a few seconds collapse into a small round
button on the left, like the north mark on the right, with the holiday icon in red, shown only on a
holiday; tapping it shows the notification again; showing and hiding animated. All holidays are
listed, working ones too, with a "DAY OFF" badge on regular holidays and special non-working days.
Regional means the Luzon regions only (NCR, CAR, I, II, III, IV-A, MIMAROPA, V). The user's rule
(2026-11-18): the regional part must not depend on the map or the address file (the map will be
replaced); it works from coordinates alone.

Data (`tools/holidays/`, built into `dashboard/holidays.txt`, copied next to the exe):
- National: Proclamation No. 1006 (2026, signed 2025-09-03), Eid'l Fitr No. 1189 (2026-03-20),
  Eid'l Adha No. 1264 (2026-05-27), and No. 1427 for 2027 (signed 2026-09-08; its Lawphil copy lacks
  the "additional special non-working days" heading, the press office's release lists Chinese New
  Year, Black Saturday, All Souls' Day and Christmas Eve as non-working). The 2027 Eid dates are
  proclaimed later (expected around 10 March and 16 May 2027), then go into `national.tsv`.
- Local, 2026: 219 proclamations for Luzon places (Nos. 1109 to 1515), read one by one from the
  proclamation texts on lawphil.net on 2026-11-18, with the reason from the text. Two are special
  election days (Antipolo's 2nd district, 14 March; Cavite's 4th district (Dasmariñas), 29 August).
  Proclamations after No. 1515 (most December dates) were not published yet.
- By law, every year: 11 nationwide special working holidays (for example National Women's Day
  8 March, Quezon Day 19 August, Graciano Lopez-Jaena Day 18 December) and 104 local days (63
  non-working, 41 working)
  (Republic Act titles from the Lawphil indexes; RA 12099, 9210 and 7552 read in full). Laws that
  later laws repealed or replaced are left out (listed at the top of `laws.tsv`). In a year that
  has proclamations (2026), a law's date is left out where a proclamation holds a day for the same
  place within a week, since the proclamation moved or replaced it (Occidental Mindoro 15 November
  moved to Monday 16 November 2026; Dasmariñas 26 November is a working holiday by law and a day off
  by Proclamation 1507).
- Towns: every Luzon city and municipality (771, matching PSA's counts per province) with its
  centre and land area from Wikidata (CC0), `tools/holidays/fetch_towns.py`, run 2026-11-18. Fixes:
  the highly urbanised cities sit under their region on Wikidata and are put back in their province
  (Baguio, Angeles, Olongapo, Lucena, Puerto Princesa), dissolved towns are dropped.
- The list: 475 holidays (62 national, 413 local; 22 regular, 359 special non-working, 94 working)
  for 2026-01-01 to 2027-12-31, 54 KB. Outside those dates nothing is shown and the log says so.

Where the car is (`holiday_locate` in `dashboard/src/holiday.c`, the same rule as `locate()` in
`build_holidays.py`): the town whose centre is nearest the GPS position, each distance divided by
the town's radius (the radius of a circle of its land area), so a large municipality reaches
further than a small one; none when even that town is more than 3 radii away (at sea, outside
Luzon). It is a nearest-centre estimate, not the real boundary: near a town line the neighbour can
be picked. Whole numbers and doubles in a fixed order make the C reader give exactly the Python
reference's answers (checked on every town centre and 3,000 random points). A local holiday applies
when its places hold that town or its province. The same name at several levels (Quezon Day: a
working day nationwide, a day off in Quezon City) is shown once, at the most specific level. At most
3 rows: national first, then the province's, then the town's. The town is looked up again after
about 110 m (at most every 10 s), on the UI thread (771 towns, a few thousand soft-float
operations).

Behaviour (`dash.c`, movement in `island.c`, drawing in `render_island`): once the date is known,
each second the dashboard works out today's rows; a row not shown yet today opens the notification
(at once on the dashboard page with the window visible and the menu closed, otherwise when it is
next in front). It grows out of the round button (a slight bounce, 0.42 s), the text fades in, it
stays 6 s, the text fades out and it shrinks back into the button (0.45 s). A new row while open
makes it taller; when no holiday applies any more the button goes. Tapping the button opens it
again, tapping the open panel closes it, and leaving the dashboard page, opening the menu or the
window being hidden turns an open panel back into the button at once. The button sits at the top
left of the map (x 153, y 48, 44 px), the north mark's place mirrored; the panel is 380 px wide at
the top middle of the map, 72 px a row, nearly solid (248 of 256) so nothing shows through its text.
Each row: the red calendar icon, the name, the long date ("Thursday, November 26, 2026"), and the
note ("Regular holiday", "Special non-working day", "Special working day", "Non-working day in
Lucban", "Working holiday in San Juan") after a red "DAY OFF" badge on days off.

Speed on the unit: a full repaint (map composition, panels, the 800 x 480 copy that alone took about
40 ms in the vector map test) is too slow for a smooth movement. So after each full repaint of the
dashboard the program keeps a copy of the screen under the notification's area (460 x 240 pixels,
221 KB), and while it moves a 15 ms timer asks for frames that put back only the part the previous
frame covered, draw the new frame there and copy only that part to the screen. The 1 s repaint waits
while it moves (half a second at most). Without the back buffer (16-bit until 0.8, 32-bit since
0.9) each frame is a full repaint.
The import table is identical to 0.7.0's (`objdump -p`). Not known yet: how fast these frames are on
the unit; the 10-minute log line `[holidays] notification: N frames drawn alone, slowest X ms` tells.

Log (`dashboard-log.txt`): the list loaded (`[holidays] 475 holidays, 39 provinces, 771 towns, for
...`), and once a day the national holidays (`[holidays] 2026-11-30: Bonifacio Day (regular
holiday)`). The town and the local holidays are never logged: they are as revealing as the position.

Updating: edit `tools/holidays/national.tsv`, `local-2026.tsv` (2027's proclamations go into a new
`local-2027.tsv` of the same form; every `local-*.tsv` is read) or `laws.tsv`, run
`python3 tools/holidays/build_holidays.py`, rebuild the dashboard. `tests/test_holidays.py` checks
that `holidays.txt` is up to date and compares the C reader with the reference.

## The probe

`probe/` is a small Windows CE program (C, built with CeGCC in Docker: `tools/build_probe.sh`). It
runs from `\SD\Probe\Probe.exe`, started from Total Commander, and appends everything to
`probe-log.txt` next to itself. See `probe/README.txt` for the on-car steps. Version 0.5 buttons
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
   can be matched to actions. Version 0.5 steps: parking lights, headlights, hazards, reverse (foot
   on the brake), parking brake, driver's seatbelt, driver's door, the other three doors and the
   tailgate one at a time, the key from ACC to ON to engine start (cars with a key only), and
   finally 10 minutes of running for values that are sent only rarely. The fan steps of 0.4 are
   gone (climate is not wanted). Survey also lists network adapters (`HKLM\Comm\*\Parms\TcpIp`) for
   the USB tethering test.
6. **Exit**: logs a summary and quits.

Always on: logs when `Main.exe`'s `AirControlBar` climate popup is shown or hidden and saves up to
12 screenshots of it as `aircontrol_NN.bmp`; reads the fan level from `Main.exe`'s memory (see
"Climate"); and (0.5) maps the system status table `SkSysInfoShareMem` read-only, logs all 62
values at start and at exit and every change in between, checked every 200 ms (`[sysinfo]`
lines; layout in "The message bus"). It opens the block itself with `CreateFileMappingW` (size 0,
which only opens an existing block) and `MapViewOfFile` (`FILE_MAP_READ`), both looked up at run
time, so no `SkShareMem.dll` code runs in the probe. When `SkShareMem.dll` is not the analysed
147,456-byte build it logs changed bytes by offset only. Probe 0.5 imports exactly the same
functions as 0.4.

The ring reader and bundle parser (`probe/src/busring.c`), NMEA parser (`probe/src/nmea.c`) and
status table reader (`probe/src/sysinfo.c`) are tested on the build machine by
`tests/test_probe_selftest.py`, partly with messages copied from the car. The probe checks two export offsets of `SkCom.dll` before using its internal
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

1. Run probe 0.5 (`probe/README.txt`; it replaces 0.4): lights, hazards, reverse, parking brake,
   seatbelt, doors and tailgate, key positions, 10 minutes of running, and the USB tethering
   test. Bring back `probe-log.txt`.
2. From the log: the meaning of the `0x33` bits and of `0x52A`; which `[sysinfo]` and
   `[devstatus]` values move with each step (which bit is the parking brake, and whether anything
   at all moves for the doors, seatbelt or key); whether a USB network adapter appears.
3. For pitch, roll and yaw rate: if USB networking works, a phone over USB tethering is the
   no-purchase route. For real steering angle, RPM and coolant: a small USB-network sensor board
   (CAN for steering angle and yaw rate, OBD-II requests for RPM and coolant).
4. Run dashboard 0.1.0 (`dashboard/README.txt`) and bring back `dashboard-log.txt`.
5. Bluetooth device switching: `\NandFlash\Application\Bluetooth\` is copied (2026-11-18; no
   network profile, see "Bluetooth"); still to do: find in it whether `Bluetooth.exe` accepts a
   connect request and where it keeps the paired list (`HKLM\Software\CSR\Bluetooth\Device` is a
   candidate), and copy the three `Bluetooth*Res` files (copy, never move; do not copy
   `\backup\bt_sc.db`, it holds link keys). For the status panel, log `0x475`, `0x52A` wParam 0
   and setup string `0x40F` while connecting, disconnecting and switching phones in the factory
   screen.
6. Media: run the probe's bus logging while Bluetooth music plays, and press next, previous and
   pause on the steering wheel and on the InfoBar. The log should show `0x4AB` (key and state, to
   the Bluetooth app) and `0x4A0` with the song fields.
7. Vector map pilot: run `VMapTest.exe` 0.5 (replace `\SD\VMapTest`; `\SD\VMap` stays), let the
   series finish, then drag, turn and press Drive in Smooth; bring back
   `VMapTest\vmaptest-log.txt` (the `[chunks]` line says which chunk size the unit likes).
8. Dashboard 0.9.0 (`dashboard/README.txt`): copy `Navigation`, and `luzon.vmap` alone into
   `\SD\VMap`, drive with the map (Dasmarinas and a city compare with the pilot's figures), change the zoom and restart once (it should come back), try the map
   page's drag; bring back `dashboard-log.txt` (the `[map]` lines give the frame times).

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
