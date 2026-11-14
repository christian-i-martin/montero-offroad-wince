# Builds the dashboard (Navigation.exe) with CeGCC (arm-mingw32ce). Run through
# tools/build_dashboard.sh, which provides the toolchain in Docker. The bus, CarInfo and GPS
# readers are shared with the probe (../probe/src). PNG decoding for the map: third_party/stb_image.h.
CC = arm-mingw32ce-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -march=armv4t -marm -Isrc -I../probe/src
OUT ?= build

SHARED = ../probe/src/carinfo.c ../probe/src/busring.c ../probe/src/nmea.c
SOURCES = src/dash.c src/model.c src/render.c src/sun.c src/map.c src/pngdec.c $(SHARED)
HEADERS = src/model.h src/render.h src/sun.h src/map.h third_party/stb_image.h ../probe/src/carinfo.h ../probe/src/busring.h ../probe/src/nmea.h
FONTS = fonts/Barlow-Medium.ttf fonts/Barlow-SemiBold.ttf fonts/BarlowCondensed-SemiBold.ttf fonts/OFL.txt

$(OUT)/Navigation.exe: $(SOURCES) $(HEADERS) $(FONTS) README.txt
	mkdir -p $(OUT)/fonts
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -lm -s
	cp $(FONTS) $(OUT)/fonts/
	cp README.txt $(OUT)/README.txt

clean:
	rm -rf $(OUT)

.PHONY: clean
