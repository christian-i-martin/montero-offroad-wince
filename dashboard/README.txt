MONTERO DASHBOARD 0.5.0

What it is
  A one-page off-road dashboard that the head unit's NAV button opens instead of
  the navigation program. The map of where the car is fills the whole screen;
  the readings sit on see-through panels over it: a bar at the top (clock, GPS,
  headlights, sunset or sunrise, and the car info button), speed (current,
  average and top) and
  elevation with a graph of the last 20 km on the left, outside temperature,
  pressure and battery with small graphs on the right, the heading at the
  bottom left and the address (road, barangay, city and province) at the
  bottom right.
  Day and night colours follow the sun: dark from sunset to sunrise, light
  from sunrise to sunset, worked out from the GPS position and time. Until GPS
  has given both (just after starting), the headlights decide.

Install
  1. On the computer, keep a copy of the SD card's current Navigation folder
     (Total Commander), so you can go back.
  2. Delete the Navigation folder from the SD card and copy this Navigation
     folder to the card root. Correct: \SD\Navigation\Navigation.exe
  3. For the map and the address, copy the Map folder (made on the computer,
     see "Map" below) to the card root as well, next to Navigation:
     \SD\Map\*.pack, \SD\Map\luzon.addr and, for the light day map,
     \SD\Map\Day\*.pack. It sits outside the Navigation folder, so a new
     dashboard does not mean copying the map again.
     A Mac adds a hidden "._" file next to each copied file. The dashboard
     skips them, but they waste space; remove them with
     dot_clean -m /Volumes/<card name>  before ejecting the card.
  4. Put the card back, restart the head unit normally, press NAV.

Using it
  It updates once a second. Hold the clock (top left) for 2 seconds for the
  menu: Time zone, Open file manager (Total Commander, if it was included),
  Close dashboard, Back. Pressing NAV again brings the dashboard back.
  The button at the top right (a car with an "i") opens the car info page;
  Back at its top left returns to the dashboard.
  The + and - buttons at the bottom of the map zoom in and out. The map starts
  at the closest zoom the map files hold (15 when the zoom 15 file is on the
  card, otherwise 14) and goes back to it at every start.

Map
  A north-up map across the whole screen, with the car in the middle below the
  top bar: an arrow along the direction of travel while moving, a dot when
  stopped, a grey dot at the last position when GPS has no fix. No routes or
  directions. The map is drawn from picture files (*.pack) in the SD card's
  Map folder: the dark map there is used at night, and the light map in
  Map\Day by day. Without a Day folder the dark map is used by day too.
  Without any map files the screen says "No map files". They are made on the
  computer from OpenStreetMap data with tools/map in the project (the light
  map with MAP_STYLE=light). The dark map is drawn at half brightness at night.
  Map data (c) OpenMapTiles (c) OpenStreetMap contributors.

Car info
  A page of fixed facts about the car: plate number, fuel type, fuel tank,
  engine, water wading depth, ground and height clearance, width, approach,
  departure and breakover angles, turning circle, tyre size and pressure,
  drive and gross weight. They are built into the program (2016 Montero Sport
  2.4 diesel 4x2); nothing is read from the car. Check the tyre pressure
  against the sticker on the driver's door pillar.

Address
  Bottom right: the nearest named road (within 50 m), the barangay, then the
  city or municipality and the province, looked up offline in luzon.addr in
  the same Map folder (made with tools/address from OpenStreetMap data). It is
  looked up again after about 15 m or 30 seconds. Many rural roads and tracks
  have no name in OpenStreetMap; the panel then says "No named road nearby".
  Without a barangay outline it shows the nearest named place ("Near ...").
  Names can be added or corrected at openstreetmap.org; they appear after the
  next build. "No address file" means there is no luzon.addr in \SD\Map\;
  "Address file cannot be read" means it is there but damaged or incomplete
  (copy it again). The [address] line at the top of the log says which.

Clock
  The head unit's own clock resets to 8 June 2012, so the dashboard takes the
  time and date from GPS. The clock appears once GPS has a fix (it then keeps
  running if the signal drops). Times are shown with AM and PM.
  GPS gives UTC; the dashboard shows UTC+8
  unless you choose another time zone with the - and + buttons in the menu
  (30-minute steps). A choice is saved in dashboard-settings.txt next to
  Navigation.exe; delete that file to go back to UTC+8.

Where the values come from
  Speed, heading while moving, elevation, sunset and sunrise, and the day and
  night colours: the GPS.
  When GPS has no fix, elevation comes from the car's sensor (corrected by the
  last GPS difference) and heading from the car's 8-point compass.
  Pressure, outside temperature, battery and headlights: the car, through the
  head unit's own Car Info channel (read requests only, like the factory Car
  Info screen). Average and top speed count from when the dashboard started
  and are not saved.

Safety
  It only reads: the GPS port, the map files and the address file are opened
  read-only, and the only messages it sends are the factory Car Info screen's
  read requests. It changes nothing on the head unit. The position and the
  address are used on screen only and are never written to the log.

Log
  dashboard-log.txt next to Navigation.exe: start, data sources, a summary
  every 10 minutes, errors. No position, no address. It starts over above
  256 KB.
  Bring it back if something looks wrong.

Not tested on the head unit yet. If it misbehaves, restart the head unit and
put the old Navigation folder back.

Fonts: Barlow by The Barlow Project Authors, SIL Open Font License (fonts/OFL.txt).
PNG decoding: stb_image by Sean Barrett, public domain (or MIT).
