MONTERO PROBE 0.5 (data discovery, not the dashboard)

Install
  Copy this Probe folder to the SD card root, replacing the old one:
  \SD\Probe\Probe.exe. Leave the Navigation folder (Total Commander) as it is.
  Delete the old probe-log.txt first so the new log starts clean.
  Park on level ground, engine running, parking brake on, gear in P.

Start
  Press NAV, browse to the Probe folder in Total Commander, start Probe.exe.
  Press Bus, then Next step, and follow the instruction at the top of the
  screen. Press Next step again when each one is done:
    1 baseline, touch nothing for 10 s
    2 parking (small) lights on 5 s, then off
    3 headlights on 5 s, then off
    4 hazard lights on 5 s, then off
    5 foot firmly on the brake: shift to R, wait 5 s, back to P
    6 foot on the brake: release the parking brake 5 s, apply it again
    7 driver's seatbelt: unbuckle 5 s, then buckle again
    8 driver's door: open 5 s, then close
    9 get out and open, one at a time and 5 s each, then close: front
      passenger door, rear left door, rear right door, then the tailgate.
      Get back in and buckle up.
   10 only if the car has a key (skip it with a start button): turn the
      key back to ACC so the engine stops and the head unit stays on, wait
      10 s, key to ON (dashboard lights on, engine off), wait 10 s, then
      start the engine. If the head unit restarts while the engine starts,
      that is fine: the log is saved line by line. Start the probe again
      and press Bus (no need for Next step), then do step 11.
   11 done: if you can, leave the probe running for 10 minutes with the
      engine on before pressing Exit (slow values such as a fuel level
      may only be sent every few minutes)
  If you leave the probe with HOME, press NAV and start Probe.exe again in
  Total Commander: it brings the running probe back, it does not start a
  second one.

Optional: USB network test (needs an Android phone and its USB cable)
  With the probe still running, plug the phone into the head unit's USB
  port, turn on USB tethering on the phone (Settings, Hotspot and
  tethering), wait 15 s, then press Survey. Unplug the phone afterwards.

Exit
  Always exit with the Exit button.

Afterwards
  Copy probe-log.txt and any aircontrol_NN.bmp files from \SD\Probe back to
  the computer. The .bmp files are screenshots of the climate popup.
  The log also holds Main.exe's status table (lines starting [sysinfo]),
  read without changing it.
  If the probe or the head unit misbehaves, restart the head unit normally;
  the probe changes nothing on the unit.
