# Builds the dashboard (Navigation.exe) with CeGCC (arm-mingw32ce). Run through
# tools/build_dashboard.sh, which provides the toolchain in Docker. The bus, CarInfo and GPS
# readers are shared with the probe (../probe/src). The vector map: src/vmap.c and friends (zlib
# inflating from third_party/stb_image.h through pngdec.c).
# holidays.txt is built by tools/holidays/build_holidays.py and copied next to the exe.
CC = arm-mingw32ce-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -march=armv4t -marm -Isrc -I../probe/src
OUT ?= build

SHARED = ../probe/src/carinfo.c ../probe/src/busring.c ../probe/src/nmea.c
VMAP = src/vmap.c src/vtiles.c src/vraster.c src/vfont.c src/vicons.c src/pngdec.c
SOURCES = src/dash.c src/model.c src/render.c src/sun.c src/mapview.c src/addr.c src/holiday.c src/island.c $(VMAP) $(SHARED)
HEADERS = src/model.h src/render.h src/sun.h src/mapview.h src/addr.h src/holiday.h src/island.h src/vmap.h src/vtiles.h src/vraster.h src/vfont.h src/vicons.h src/map.h third_party/stb_image.h ../probe/src/carinfo.h ../probe/src/busring.h ../probe/src/nmea.h
FONTS = fonts/Barlow-Medium.ttf fonts/Barlow-SemiBold.ttf fonts/BarlowCondensed-SemiBold.ttf fonts/OFL.txt

$(OUT)/Navigation.exe: $(SOURCES) $(HEADERS) $(FONTS) README.txt holidays.txt
	mkdir -p $(OUT)/fonts
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -lm -s
	cp $(FONTS) $(OUT)/fonts/
	cp README.txt $(OUT)/README.txt
	cp holidays.txt $(OUT)/holidays.txt

clean:
	rm -rf $(OUT)

.PHONY: clean
