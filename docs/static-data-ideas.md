# Dashboard ideas that use data copied in advance

Status: brainstorm only (2026-11-15). Nothing here is agreed, designed or built. Agreed decisions
stay in `vehicle-data-research.md` ("Dashboard") and on the design canvas.

The head unit has no internet connection, but anything that changes slowly can be prepared on the
Mac and copied to the SD card. Most ideas get their value from combining a fixed list with what the
dashboard already knows: GPS time, the position, the heading, the speed, and the city and province
from the address file. Many Philippine rules apply per city, so "a list plus which city you are in"
gives more than the list alone.

## Ground rules for all of them

- **Expiry date.** Every file carries a "valid until" date. Once it has passed, the screen says so
  ("Holiday list expired") instead of quietly showing old data. Holidays change during the year
  (Eid dates, added special days), so this matters.
- **File format.** Small lists are plain UTF-8 text files that can be edited on any computer. Big
  data (roads, places, terrain) goes through Mac build tools like `tools/map/` and
  `tools/address/`.
- **Clock.** Calendar items work only once GPS time has arrived (the unit's clock resets to 2012).
- **City coverage.** Per-city items depend on the address file knowing the city. In Luzon only
  about 57% of cities and municipalities have outlines in OpenStreetMap, and almost none in Abra,
  Kalinga, Ifugao, Apayao, Quirino, Nueva Vizcaya, Isabela and the Ilocos provinces, which are
  exactly the off-road areas. "Complete town and barangay outlines" in the second round fixes this.
- **Screen space.** The agreed layout is one page, so each idea needs a home: the header, an extra
  line in the address panel, icons on the map, the speed digits, or a page in the hold menu.
- **Privacy.** Anything chosen by position is shown on screen only and never written to the log,
  like the address.
- **Computing on the unit.** The head unit can also work things out from the files and the live
  data, so a file can hold compact raw material (rules, constants, terrain heights, road shapes)
  and the unit derives what is shown, as needed. Heavy one-time work stays on the Mac. Limits are
  under "What the unit can compute" in the third round.
- **Poor fit.** Data that changes too fast for copying in advance: fuel prices (weekly), weather
  and typhoon signals, road closures, traffic. These would need a live link such as the USB
  network test in probe 0.4.

## First round

### Calendar (needs GPS time)

- **Holidays, national and local.** Regular and special non-working days from the yearly
  proclamation, plus city and provincial days (charter days, founding anniversaries). With the city
  from the address file, a local holiday shows only where it applies. Also flags long weekends,
  when NLEX, SCTEX and the mountain roads fill up. Screen: header.
  **Built in dashboard 0.8** (2026-11-18) as the holiday notification, without the long-weekend
  part: see "Holiday notification" in `vehicle-data-research.md`. Local holidays use the town whose
  centre is nearest the GPS position (a town table in the holiday file), not the address file.
- **Town fiestas.** Every town has a fiesta date. "Fiesta today in (town)" warns about processions
  and closed streets. Screen: address panel.
- **Number coding.** The plate's last digit plus each city's rules (Metro Manila; Baguio is
  believed to have its own scheme). Suspended on holidays, so it uses the holiday list. Shows
  "Coding until 8 PM" only when it applies at this place and time. Screen: header.
- **Tides.** NAMRIA publishes yearly tide tables for coastal stations. Next high and low tide at
  the nearest station, for beach driving and river mouths (Zambales, Ilocos, Aurora). Shown only
  near the coast.
- **Personal dates.** LTO registration month (from the plate's last digit), insurance and licence
  expiry. The service reminder was dropped, so skip this if it feels like the same thing.

### The road you're on (same OpenStreetMap extract as the address file)

- **Road details.** Surface (paved, gravel, dirt), road type, "4x4 only" tags, speed limit, weight
  and height limits on bridges. `addr.c` already finds the nearest named road, so this is mostly
  extra fields in the `.addr` file. Where OpenStreetMap has no limit, the default by road type
  from RA 4136; the speed digits could change colour above it. Philippine coverage of these tags
  is patchy; the address coverage report (`check_address.py`) could measure it.
- **Fords and river crossings** (OpenStreetMap `ford=yes`), drawn on the map.
- **Nearest fuel, hospital and police,** as a straight line and a direction ("Fuel 14 km NE").
  Not routing. Off-road, the useful part is seeing the nearest fuel getting far away.
  Sources checked on 2026-11-15: no public list with coordinates exists from the government. The
  Geofabrik OpenStreetMap extract of 2026-11-13 has 10,893 fuel stations (`amenity=fuel`) in the
  Philippines and 6,541 in the Luzon box (about 100 of them drawn twice, as a point and an
  outline); 92% have a name, 54% a brand, 7% opening hours. Official counts are about 9,400
  (DOE, end of 2019) to about 14,300 (police monitoring, 2026), so OpenStreetMap has most
  stations but not all. DOE's own list (fuel subsidy stations, 17 August 2026, about 2,750 stations
  nationwide) gives text addresses only (region, province, city, barangay, street, company,
  station name): useful as a cross-check, not for positions. Overture Maps places (open licence per
  record) is the best second source to merge with OpenStreetMap on the Mac; Google Maps and the
  oil companies' station finders do not allow copying their data.
- **Kilometre posts.** DPWH KM markers (distance from KM 0 at Rizal Park) are how people describe
  places on national roads. OpenStreetMap has some; DPWH data on data.gov.ph may have more.
  "Near KM 245" in the address panel.
- **Toll roads.** On an expressway, which RFID it takes (Easytrip or Autosweep).

### Terrain (a free height model: Copernicus 30 m or SRTM)

- **Ground height under the car.** Steadier than GPS altitude, and a better sea-level pressure for
  the weather trend. The agreed rule is GPS-first elevation, so this would be a cross-check or a
  fallback, not a replacement.
- **Hillshade or contour lines on the map,** so ridges and valleys show where there are no roads.
  One more layer in the existing map build; bigger pack files.
- **Peaks and passes ahead,** from OpenStreetMap peaks and mountain passes plus the heading
  ("Mt. Pulag, 18 km NE").
- Size for the Luzon box (119.4 to 124.5 E, 12.4 to 18.8 N): about 95 MB at 90 m detail, about
  850 MB at 30 m, less once the sea is left out.

### Safety and areas

- **Hazard zones.** Landslide and flood risk (MGB, Project NOAH), volcano danger zones (Taal,
  Mayon, Pinatubo lahar areas). A line in the address panel or shading on the map. Download terms
  need checking.
- **Protected areas.** "Entering Mt. Pulag National Park" from OpenStreetMap boundaries, plus own
  notes such as "register at the ranger station".
- **Emergency card per town.** 911 plus the local disaster office (MDRRMO), police and fire
  numbers, and the nearest hospital. A page in the hold menu.
- **Mobile signal.** OpenCelliD (free, CC BY-SA) lists tower positions, so the dashboard can warn
  "probably no signal" in the mountains. Rough: a tower's position is not its coverage.

### Own lists

- **Saved places** (camps, trailheads, shops) from a GPX or Google My Maps export, as map icons with
  distance and direction. Different from the rejected "back to camp" (nothing is recorded on the
  unit), but close; ask before building.
- **Trail lines** from a GPX file (for example a Wikiloc trail) drawn on the map, no directions.
  Same caveat.
- **Vehicle card** in the menu: tyre pressures, wading depth, approach and departure angles, fuse
  list, from the owner's manual.
- **Town card** on entering a new municipality: province, population, what it is known for, fiesta
  (PSA and Wikipedia data).

### No file needed (calculated like the sunset already is)

- Moon phase and moonrise, for night driving.
- End of twilight ("light until 18:12").

### First-round picks

1. Road details in the `.addr` file (surface, 4x4 only, speed limit, fords): reuses the address
   build and reader.
2. Holidays with local days, plus number coding: small text files, one header line.
3. The height model: hillshade and a second elevation source.
4. Nearest fuel, plus the emergency card.

## Second round

### Make the existing tiles better

- **Complete town and barangay outlines.** The Philippines' official administrative boundaries
  (PSA and NAMRIA, published by UN OCHA on the Humanitarian Data Exchange, down to barangay level)
  cover every municipality, where OpenStreetMap covers 57% of Luzon's. Built into the `.addr` file
  in place of, or as a fallback to, the OpenStreetMap outlines. Fixes the address panel in the
  Cordillera and the north, and every per-city idea above with it. The data's PSGC codes (the
  official place codes) also link to PSA population figures and other lists. Licence and how
  recent the outlines are need checking. Sitio and purok names (OpenStreetMap levels 11 and 12)
  could add a finer line where they exist.
- **Rivers and bridges you cross.** River names from OpenStreetMap: "Crossing the Chico River" in
  the address panel. Another record kind in the `.addr` file. River names are probably better
  covered than barangays; to be measured.
- **Pressure compared with normal.** A small table of the usual sea-level pressure per month for
  each part of Luzon (from ERA5, the free European climate record, or PAGASA's normals). "1002 hPa,
  8 below normal for November" turns the number into something readable, and a pressure well below
  normal is a sign of a nearby typhoon. Uses the sea-level pressure the model already works out.
- **Sunset behind the ridge.** With the height model, the dashboard can work out when the sun
  really drops behind the mountains in the direction of sunset, which in a valley can be long before
  the official sunset. "Sun behind the ridge 16:40 (sunset 17:25)" in the header. Matters for
  making camp before dark.
- **Satellite map layer.** Free cloud-free satellite pictures (EOX's Sentinel-2 cloudless, 10 m
  detail, free for non-commercial use) as a second map style, switched in the menu. Off-road,
  tracks that OpenStreetMap lacks are often visible in the pictures. Roughly 0.5 to 1 GB for zoom
  8 to 14 over land as JPEG; needs JPEG decoding switched on in `stb_image.h` (`pngdec.c` builds
  PNG only today). Zoom 15 adds nothing at 10 m detail.
- **Unpaved roads and tracks stand out on the map.** A change to `style/montero-dark.json` using
  the surface and track tags already in the extract.

### Services for remote areas (one points-of-interest file, like the address file)

- **Opening hours.** OpenStreetMap `opening_hours` with the GPS clock: "Fuel 14 km NE, closes
  20:00". Many provincial stations close at night. Patchy data.
- **Last ATM.** Remote towns run on cash: "Nearest ATM 40 km".
- **Vulcanizing and repair shops** (tyre repair), and Mitsubishi dealers and service centres from
  Mitsubishi Motors Philippines' dealer list. For a flat tyre or a part far from home. OpenStreetMap
  coverage of vulcanizing shops is unknown.
- **Sights and camping.** Campsites, viewpoints, waterfalls, hot springs, drinking water, as map
  icons or a "nearby" list.
- **Barangay halls** as a place to ask for help when lost.

### Expressways

- **Next exit and service area** in the direction of travel ("Dau exit 6 km, service area 12 km"),
  from OpenStreetMap exits and service areas along the expressway. Information only, no routing.
- **Toll estimate.** The Toll Regulatory Board's yearly toll table: the dashboard remembers the
  entry plaza in memory (nothing saved) and shows about how much the trip costs at the exit.

### Radio

- **FM station names.** A list of stations by frequency per area (NTC's list or public station
  lists). The radio's now-playing message (bus message `0x4A0`, `InfoTuner`) probably carries the
  frequency, so the dashboard could show "92.7, (station name)". It could also list the stations
  usually heard near the current position when the signal fades in the mountains. What the radio
  message really carries has not been seen in a log yet.
- **Two-way radio repeaters** by area, if a radio is carried. Niche.

### Off-road helpers

- **Air-down card and reminder.** A table of tyre pressures by terrain (sand, mud, rocks, highway)
  for the fitted tyres. An "aired down" switch in the menu (kept in memory only) makes the speed
  digits warn above a chosen speed until it is switched off.
- **4WD mode card.** Which Super Select mode suits which terrain and at what speed each shift is
  allowed, from the owner's manual.
- **Checklists.** Before the trail, recovery gear, after the trail. Ticks are kept in memory only.
- **Picture pages.** Any PNG in a folder on the card becomes a page in the menu: fuse box, recovery
  and jack points, tyre placard, a page from a trail guide. `pngdec.c` already decodes PNG, so this
  covers the vehicle card and more without designing each page. Photos of documents such as the
  OR/CR (registration papers) can be read by anyone who has the card.
- **Text note pages.** Permits, contact numbers, trip notes, and emergency contacts (family,
  roadside assistance, insurer) next to the emergency card.

### Calendar extras

- **Events file.** Holy Week, Undas, the Christmas exodus, election periods (COMELEC checkpoints and
  the liquor ban), and DepEd school days so school zones only warn on class days and hours (school
  locations from OpenStreetMap).
- **Days to the next long weekend,** for planning. Minor.

### No file needed (second round)

- **Water boiling point** from the car's pressure sensor, for camp cooking (about 93 C at 2,000 m).
- **Golden hour,** the hour before sunset, for photos.
- **Payday traffic** on the 15th and the last day of the month in Metro Manila.

### Not recommended

- **Faster first GPS fix** with downloaded satellite orbit data (SiRF extended ephemeris). It needs
  writing to the GPS receiver on `COM1:`, which `Main.exe` also reads for its clock; this goes
  against the spirit of the safety rules.
- **Magnetic declination** (World Magnetic Model): the difference between magnetic and true north
  is small in Luzon. Low value.
- **Geoid correction** for GPS altitude: a few metres at most. Low value.
- **Personal records** such as the highest point reached: needs saved history, which was ruled out.
- **Speed cameras:** not an off-road need, and their enforcement status keeps changing.

### Second-round picks

1. Complete town and barangay outlines: fixes the address panel and every per-city idea in exactly
   the provinces where off-road trips go.
2. Picture and note pages: cheap (the decoder exists) and they cover the vehicle card, fuse box and
   checklists without designing each one.
3. Pressure compared with normal, and sunset behind the ridge (once the height model exists).
4. Vulcanizing shops, ATMs and fuel opening hours, in the same file as nearest fuel.

## Third round: computing on the unit

These ideas lean on the head unit doing extra work: combining the files with the live GPS and car
data, as needed, instead of only looking things up.

### What the unit can compute

- The dashboard is built for ARMv4T (`-march=armv4t` in `dashboard/Makefile`), which has no
  hardware floating point, so every `float` and `double` operation is done in software and is slow.
  Maths that runs often should use whole numbers (fixed point), as `map.c` already does for its
  per-pixel blending, or run on a background thread at a low rate and keep its result, like the address lookup does.
- The processor's speed and the free memory have not been measured. Before planning the heavier
  ideas below, the dashboard log should record how long an address lookup and one screen repaint
  take on the car.
- Split the work: anything done once (fitting, indexing, simplifying, measuring river catchments)
  runs on the Mac when the files are built; the unit only does the small per-position part.

### Compute instead of store (smaller files that do not expire)

- **Tides from constants.** Instead of yearly tide tables, store a few dozen tide constants per
  station (fitted on the Mac from a year of NAMRIA tables) and work out the tide for any date on
  the unit. Never expires. Same terms question as the tables.
- **Holidays from rules.** Fixed dates, plus rules for the moving ones (Holy Week from the date of
  Easter, National Heroes Day on the last Monday of August), plus a short table for Chinese New
  Year. Eid dates can be estimated from the moon and shown as "expected" until announced. The
  yearly file then only needs the proclamation's changes.
- **Terrain as 16-bit PNG tiles.** The height model can be stored as 16-bit greyscale PNG files,
  which the existing decoder (`stb_image.h`) already reads, so no new format is needed.
- **Hillshade and contour lines drawn on the unit** from the height model, instead of baked into
  the map tiles. Smaller map files, and the shading could even follow the real sun's direction.
  Costs processor time per map tile.

### The road ahead

The unit follows the road you are on, in the direction you are driving, until it ends or the road
name or number changes at a junction. It never chooses between roads, so this is not routing. It
needs a road file with road shapes and junctions (a new build product, beside the address file)
and, for heights, the height model.

- **Climb or descent ahead.** A small profile of the next few kilometres of this road: "Climb
  12% for 1.5 km", "Long descent ahead: low range". Also the highest point ahead and the expected
  temperature there (about 6 C cooler per 1,000 m higher).
- **Bends ahead** with a suggested speed worked out from how tight each bend is ("Sharp bends for
  3 km, about 30 km/h"). For mountain roads such as the Halsema Highway. Only as good as the road
  shapes in OpenStreetMap.
- **Slope under the car while stopped.** The height model along the road's direction gives the
  grade where the car is standing, even at a crawl, when the GPS estimate is useless. A rough
  stand-in for the pitch reading the unit cannot give (30 m terrain detail; road cuts and fills
  are not in it).
- **Which road am I on, more reliably.** Use the heading and the previous road to stay on the
  right road at junctions and on parallel roads, instead of only the nearest one. Improves the
  road details and speed limit.
- **Distance along the road** to the next fuel station or exit, instead of a straight line. Close
  to routing; ask before building.

### Line of sight with the height model

- **Better signal estimate.** For the nearest mobile towers (OpenCelliD), check whether hills block
  the straight line to each one. Much better than distance alone: "Tower in view 6 km NE" or "No
  tower in view".
- **Visible peaks only.** Show only the peaks not hidden behind a nearer ridge, and name the one in
  the direction you are driving.
- **Tap the map** to see that point's distance, direction and height, and how much higher or lower
  it is than you ("1.8 km NE, 1,240 m, 310 m above you").

### Watching the live data against references

- **Storm signs.** A thunderstorm's cold outflow brings a sudden temperature drop of several
  degrees and a small pressure jump. Correct the outside temperature and pressure for the changing
  elevation (so climbing a mountain does not trigger it) and warn "Storm signs: 5 C colder in
  10 minutes". Experimental: the car's sensor also reacts to sun and engine heat.
- **Flash-flood caution at fords.** On the Mac, measure how much land drains to each ford from the
  height model. On the unit, a ford with a large mountain catchment gets a caution, stronger after
  storm signs, because such rivers rise fast after rain upstream even when it is dry where you are.
- **Off the map.** When no mapped road is within a chosen distance: "No mapped road within 200 m;
  nearest 1.2 km SW". A safety line on unmapped tracks; no recording needed.
- **Sun in your eyes.** Warn when the sun is low and close to your direction of travel (driving
  west near sunset), and with the height model only when it is above the ridge.
- **Battery while camping.** A table of resting battery voltage against charge (lead-acid) gives
  "Battery about 70%" with the engine off, and the rate of fall gives "12.0 V in about 3 h". Only
  valid after the battery has rested; under load it reads low.
- **Compass check.** Compare the car's 8-point compass with the GPS course while driving straight.
  If they keep disagreeing by more than a point, suggest calibrating it in the factory Car Info
  screen. The dashboard must not send the calibration command itself (CarInfo `12` changes state).
- **Engine heat on the outside temperature.** After stopping or crawling, the sensor reads high
  from engine heat. Mark the reading "may read high" until the car has moved for a while.
- **Break reminder** after a long stretch of driving, counted in memory only.

### Sound

- **Pre-recorded voice or chime alerts** (WAV files on the card) for fords, long descents, storm
  signs or the air-down reminder. `PlaySoundW` would be looked up at run time like the other
  functions the probe never imported. Not known: whether sound plays from the navigation slot and
  how it mixes with the radio.

### Third-round picks

1. The road ahead (climb or descent profile and bends): the most off-road value, but needs the road
   file and the height model first.
2. Off the map with the nearest mapped road: cheap once road shapes are on the unit.
3. Storm signs: no new file at all; worth trying early to see how noisy the car's sensors are.
4. Tides and holidays from rules: smaller files that never expire.
