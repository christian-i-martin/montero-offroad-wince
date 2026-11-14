MONTERO PROBE 0.4 (data discovery, not the dashboard)

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
    7 stay on the probe screen: A/C fan up 3 steps, then down 3 steps,
      about 3 s apart (watch "Fan n/8" at the top of the screen)
    8 press the head unit's HOME button, change the A/C fan 3 steps,
      wait 10 s, press NAV, then start Probe.exe again in Total Commander
      (it brings the running probe back; it does not start a second one)
    9 done

Optional: USB network test (needs an Android phone and its USB cable)
  With the probe still running, plug the phone into the head unit's USB
  port, turn on USB tethering on the phone (Settings, Hotspot and
  tethering), wait 15 s, then press Survey. Unplug the phone afterwards.

Exit
  Always exit with the Exit button.

Afterwards
  Copy probe-log.txt and any aircontrol_NN.bmp files from \SD\Probe back to
  the computer. The .bmp files are screenshots of the climate popup.
  If the probe or the head unit misbehaves, restart the head unit normally;
  the probe changes nothing on the unit.
