MONTERO DASHBOARD 0.1.0

What it is
  A one-page off-road dashboard that the head unit's NAV button opens instead of
  the navigation program: pressure, elevation and outside temperature graphs,
  compass, battery, speed, average speed and top speed, with the time to sunset
  at the top. Colours switch to night mode when the headlights are on.

Install
  1. On the computer, keep a copy of the SD card's current Navigation folder
     (Total Commander), so you can go back.
  2. Delete the Navigation folder from the SD card and copy this Navigation
     folder to the card root. Correct: \SD\Navigation\Navigation.exe
  3. Put the card back, restart the head unit normally, press NAV.

Using it
  It updates once a second. Hold the clock (top left) for 2 seconds for the
  menu: Open file manager (Total Commander, if it was included), Close
  dashboard, Back. Pressing NAV again brings the dashboard back.

Where the values come from
  Speed, heading while moving, elevation and the sunset time: the GPS.
  When GPS has no fix, elevation comes from the car's sensor (corrected by the
  last GPS difference) and heading from the car's 8-point compass.
  Pressure, outside temperature, battery and headlights: the car, through the
  head unit's own Car Info channel (read requests only, like the factory Car
  Info screen). Average and top speed count from when the dashboard started
  and are not saved.

Safety
  It only reads: the GPS port is opened read-only, and the only messages it
  sends are the factory Car Info screen's read requests. It changes nothing on
  the head unit. The position is used on screen for the sunset time and is
  never written to the log.

Log
  dashboard-log.txt next to Navigation.exe: start, data sources, a summary
  every 10 minutes, errors. No position. It starts over above 256 KB.
  Bring it back if something looks wrong.

Not tested on the head unit yet. If it misbehaves, restart the head unit and
put the old Navigation folder back.

Fonts: Barlow by The Barlow Project Authors, SIL Open Font License (fonts/OFL.txt).
