# Builds the vector map test (VMapTest.exe) with CeGCC (arm-mingw32ce). Run through
# tools/build_vmaptest.sh, which provides the toolchain in Docker. The vector map code is the
# dashboard's (../dashboard/src/vmap.c, vraster.c, vfont.c; zlib inflating from pngdec.c).
CC = arm-mingw32ce-gcc
WARN = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -marm -I../dashboard/src
# VMapTest.exe for the unit's processor (Cortex-A9, ARMv7-A; ARM mode, no unaligned access, no
# floating-point or NEON instructions), VMapTest-ARMv4.exe as built before, in case the first does
# not start. -mword-relocations: the Windows CE object format cannot hold the MOVW/MOVT address
# relocations ARMv7 code uses by default.
CFLAGS = $(WARN) -march=armv7-a -mtune=cortex-a9 -mno-unaligned-access -mword-relocations
CFLAGS4 = $(WARN) -march=armv4t
OUT ?= build

SHARED = ../dashboard/src/vtiles.c ../dashboard/src/vmap.c ../dashboard/src/vicons.c ../dashboard/src/vraster.c ../dashboard/src/vfont.c ../dashboard/src/pngdec.c
SOURCES = src/vmaptest.c $(SHARED)
HEADERS = ../dashboard/src/vtiles.h ../dashboard/src/vmap.h ../dashboard/src/vicons.h ../dashboard/src/vraster.h ../dashboard/src/vfont.h ../dashboard/src/map.h ../dashboard/third_party/stb_image.h
FONTS = ../dashboard/fonts/Barlow-Medium.ttf ../dashboard/fonts/OFL.txt

$(OUT)/VMapTest.exe: $(SOURCES) $(HEADERS) $(FONTS) README.txt
	mkdir -p $(OUT)/fonts
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -lm -s
	$(CC) $(CFLAGS4) -o $(OUT)/VMapTest-ARMv4.exe $(SOURCES) -lm -s
	cp $(FONTS) $(OUT)/fonts/
	cp README.txt $(OUT)/README.txt

clean:
	rm -rf $(OUT)

.PHONY: clean
