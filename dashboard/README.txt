MONTERO DASHBOARD 0.9.0

What it is
  A one-page off-road dashboard that the head unit's NAV button opens instead of
  the navigation program. The map of where the car is fills the whole screen,
  turned so that the direction of travel is up; the readings sit on
  see-through panels over it: a bar at the top (clock, GPS,
  headlights, sunset or sunrise, and the car info button), speed (current,
  average and top) and elevation with a graph of the last climbs and descents
  on the left, outside temperature, pressure and battery with small graphs on
  the right, the heading at the bottom left and the address (road, barangay,
  city and province) at the bottom right. Tapping the car on the map opens
  the map page: the map alone, across the whole screen, which you can drag
  with a finger. On a holiday a notification at the top of the map says so
  (see "Holidays").
  Day and night colours follow the sun: dark from sunset to sunrise, light
  from sunrise to sunset, worked out from the GPS position and time. Until GPS
  has given both (just after starting), the headlights decide.

Install
  1. On the computer, keep a copy of the SD card's current Navigation folder
     (Total Commander), so you can go back.
  2. Delete the Navigation folder from the SD card and copy this Navigation
     folder to the card root. Correct: \SD\Navigation\Navigation.exe
  3. For the map, copy luzon.vmap (made on the computer, see "Map" below)
     into a VMap folder at the card root, next to Navigation:
     \SD\VMap\luzon.vmap. Keep only one .vmap file in that folder (the
     dashboard opens the first one it finds). For the address, the Map folder with
     \SD\Map\luzon.addr stays as it is. Both sit outside the Navigation
     folder, so a new dashboard does not mean copying them again. The old
     picture map files (\SD\Map\*.pack and \SD\Map\Day\*.pack) are no
     longer used; they can stay or be deleted, the address file must stay.
     A Mac adds a hidden "._" file next to each copied file. The dashboard
     skips them, but they waste space; remove them with
     dot_clean -m /Volumes/<card name>  before ejecting the card.
  4. Put the card back, restart the head unit normally, press NAV.

Using it
  The graphs use their whole width from the start; the small note above the
  outside, pressure and battery graphs says how much time they cover (for
  example "6 min") until they hold their full span (3 h, 1 h, 30 min). The
  elevation graph adds a point every 150 m driven and keeps the last 20, so it
  shows the last 3 km of road; while parked it does not change. Its note says
  how much road it covers ("450 m", "1.5 km") until it reaches 3 km.
  It updates once a second. Hold the clock (top left) for 2 seconds for the
  menu: Time zone, Open file manager (Total Commander, if it was included),
  Close dashboard, Back. Pressing NAV again brings the dashboard back.
  The button at the top right (a car with an "i") opens the car info page;
  Back at its top left returns to the dashboard.
  The + and - buttons at the bottom of the map zoom in and out, half a level
  per press, from the farthest zoom the map file holds (10) to 19. The first
  time, the map starts at zoom 16 (large print: names, roads and dots twice
  their size at zoom 15). After that it starts at the zoom you chose last: the
  choice is saved in dashboard-settings.txt next to Navigation.exe (delete the
  file to go back to 16 and UTC+8).

Map page
  Tap the car (the arrow or dot in the middle of the map) to open the map
  page: the same map across the whole screen, without the panels. It opens at
  the dashboard's zoom with the car in the middle, and follows the car, turned
  like the dashboard (the direction of travel up).
  Drag the map with a finger to look around. The map then stays where you left
  it, turned as it was, while the car moves on it, and a "Centre on car"
  button appears at the bottom right; press it to follow the car again. After
  30 seconds without a touch the page follows the car again by itself. The map
  stops at the edge of the area the map file covers.
  The + and - buttons zoom in and out around the middle of the screen. This
  zoom belongs to the map page: the dashboard keeps its own, and the page's
  zoom is not saved.
  Two-finger zoom does not work: the screen senses one finger only.
  Back at the top left returns to the dashboard.
  While you drag or zoom, new parts of the map may appear plain or blurred for
  a moment and fill in as they are drawn. The log says how fast the page was
  while dragging (see "Log").

Map
  A map across the whole screen, with the car in the middle below the top bar:
  an arrow along the direction of travel while moving, a dot when stopped, a
  grey dot at the last position when GPS has no fix. No routes or directions.
  The map turns with the car, the direction of travel up, and glides to each
  new GPS position and heading over a second. The round mark at the top right
  is a compass: its orange half and its N point north. While the car is stopped
  or slower than about 4 km/h, GPS gives no direction, so the map stays turned
  as it was (north up until the car has first moved). A parked car's GPS
  position wanders by a few metres; below 2 km/h the map ignores moves
  smaller than 4 pixels.
  The head unit draws the map itself from a vector map file (*.vmap: roads,
  areas, names and place markers as shapes) in the SD card's VMap folder,
  sharp at any zoom and turn. While the map moves, it is put together from
  pieces kept in memory, with names and markers moving along; once it stops,
  it is drawn again in full and the names are placed afresh. The same file
  holds the dark look, used at night at half brightness, and the light look
  used by day. Place markers (fuel, malls, parking, hospitals, banks and
  more) appear from zoom 15 to 17 depending on the kind, their names from 17.
  The file covers Luzon (luzon.vmap, 227 MB); outside it the screen says "No
  map for this area". Without a map file it says "No map file". The file is
  made on the computer from OpenStreetMap data with tools/vmap in the
  project.
  Map data (c) OpenMapTiles (c) OpenStreetMap contributors.

Car info
  A page of fixed facts about the car. The buttons on the left choose what the
  tiles on the right show:
    General: plate number, fuel type, fuel tank, engine, gearbox, drive, tyre
      size, tyre pressure, gross weight.
    Size and clearance: ground clearance, water wading depth, height,
      approach, departure and breakover angles, width, length, turning circle.
    Fluids: engine oil (amount, grade, ACEA and API classes, how often to
      change it, filter), gearbox fluid, rear differential oil, coolant,
      brake fluid, power steering fluid, washer fluid, A/C refrigerant, each
      with its type, amount and change interval.
  The facts are built into the program (2016 Montero Sport 2.4 diesel 4x2);
  nothing is read from the car. The fluid types and amounts come from
  Mitsubishi's owner's manual, the change intervals from the Philippine
  dealer's service guide. Check the tyre pressure against the sticker on the
  driver's door pillar, and the fluids against the manual in the glovebox.
  The page remembers the last category until the dashboard is closed.

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

Holidays
  On a holiday, a notification opens at the top of the map once the date is
  known (from GPS): the red calendar icon, the holiday's name, the date in
  full ("Monday, November 30, 2026") and what kind of day it is, with a red
  DAY OFF badge on a regular holiday or a special non-working day. Special
  working holidays are shown too, without the badge. When GPS gives the
  position, the local holidays of the town and province the car is in are
  added (and again when you drive into a town with one). After 6 seconds the
  notification shrinks into a round button at the top left of the map, with
  the red calendar icon; tap the button to see it again, tap the open
  notification to close it. There is no button on days without a holiday.
  The holidays come from holidays.txt next to Navigation.exe: the national
  holidays of 2026 and 2027 and the local ones of the Luzon regions
  (proclamations of 2026 and the yearly holidays set by law). The town is the
  one whose centre is nearest, from coordinates in that file; it does not use
  the map or the address file. Near a town line it can pick the neighbouring
  town. Local holidays proclaimed after early November 2026, and the 2027 Eid
  holidays, are not in this list yet. After 2027 it shows nothing until a
  newer dashboard with a newer list is copied.

Clock
  The head unit's own clock resets to 8 June 2012, so the dashboard takes the
  time and date from GPS. The clock appears once GPS has a fix (it then keeps
  running if the signal drops). Times are shown with AM and PM. While there
  is no fix, the GPS icon in the top bar blinks next to "Waiting for GPS".
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
  It only reads: the GPS port, the map files, the address file and
  holidays.txt are opened read-only, and the only messages it sends are the
  factory Car Info screen's read requests. It changes nothing on the head
  unit. The position, the address and the town and local holidays found are
  used on screen only and are never written to the log.

Log
  dashboard-log.txt next to Navigation.exe: start, data sources, a summary
  every 10 minutes, errors. No position, no address. It starts over above
  256 KB. At start a [map] line says which map file was read and the zoom
  range and starting zoom; every 10 minutes another says how many frames were
  drawn while the map moved and how long they took, and how long the sharp
  drawings at rest took. Each time the map page closes, a [page] line says how
  many frames were drawn while dragging and how long the slowest took; bring
  the log back after driving with the map and trying the map page, so both
  can be tuned. The [holidays] lines say
  which national holiday is today and, every 10 minutes, how fast the holiday
  notification's movement was drawn.
  Bring it back if something looks wrong.

Not tested on the head unit yet. If it misbehaves, restart the head unit and
put the old Navigation folder back.

Fonts: Barlow by The Barlow Project Authors, SIL Open Font License (fonts/OFL.txt).
PNG decoding: stb_image by Sean Barrett, public domain (or MIT).
