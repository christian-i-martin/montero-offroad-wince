Montero vector map test 0.5.1
=============================

A test of a map that the head unit draws itself from map shapes (roads, areas, names), at any zoom
and any rotation, instead of showing ready-made pictures. It covers Dasmarinas, Cavite only.

Since 0.2 the map is drawn once into small pictures kept in memory, and dragging and turning only
move and turn those pictures, with the names drawn on top. Once a turned map stops moving, it is
drawn again in full for the sharpest picture. Nothing of the map's detail is left out.

Since 0.3: frames are drawn in 32 bits (the screen's own format), names keep their places while
the map moves (placed afresh when it stops), and the program is built for this unit's processor.

Since 0.4: place markers (fuel, parking, malls, hospitals and clinics, police, pharmacies, banks
and ATMs, supermarkets, convenience stores, food, car repair, hotels; since 0.5.1 also toll booths,
campsites, churches, schools, town halls, sights, bus stations, fire stations, cafes, bakeries,
hardware stores, motorcycle shops, post offices, dentists, toilets, drinking water) with their
names when zoomed in; the other day or night look is prepared ahead; the button bar is drawn once. Toggle buttons
show what is on: Day or Night, Smooth or Fast, Pictures or Direct, Labels or Hidden.

New in 0.5: turned frames are put together in small squares, which suits the processor's cache,
and the program tries a few square sizes at the start and keeps the quickest. The NEON check of
0.4 is gone (this unit does not allow NEON).

Two programs are in this folder:
  VMapTest.exe        built for the Cortex-A9: start this one
  VMapTest-ARMv4.exe  built the old way: only if VMapTest.exe does not start or closes at once

Copy to the SD card root (replace the old VMapTest folder; VMap stays as it is):
  VMapTest\   this folder (VMapTest.exe, fonts\)
  VMap\       the map file (dasmarinas.vmap), made on the Mac with tools/vmap/build_vmap.py

Start VMapTest.exe from Total Commander. It first checks the screen and runs a series of views on
its own (about a minute: zooms, spins, drags, and 10 seconds of simulated driving), then:
  drag the map with a finger
  -  +        zoom out and in by half a level
  Turn L/R    turn the map by 15 degrees
  Spin        turn it once round in 15 degree steps
  Drive       15 seconds of simulated driving, heading up, as the dashboard would
  Night/Day   dark or light map
  Labels      names on or off
  Smooth/Fast how turned pictures are mixed (Smooth looks better, Fast is quicker)
  Pictures/Direct   frames from pictures (new) or drawn in full every time (as 0.1), to compare
  Bench       run the series again
  Exit        close

Please also just play with it for a minute: drag, turn, zoom, Drive. Then tap Exit and bring
back vmaptest-log.txt from this folder. It reads nothing from the car and the log has no
positions.

The map data: (c) OpenStreetMap contributors, (c) OpenMapTiles.
The Barlow font: SIL Open Font License (fonts\OFL.txt).
