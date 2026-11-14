# Builds Probe.exe with CeGCC (arm-mingw32ce). Run through tools/build_probe.sh, which
# provides the toolchain in Docker.
CC = arm-mingw32ce-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -march=armv4t -marm
OUT ?= build

SOURCES = src/probe.c src/carinfo.c src/busring.c src/nmea.c

$(OUT)/Probe.exe: $(SOURCES) src/carinfo.h src/busring.h src/nmea.h README.txt
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -s
	cp README.txt $(OUT)/README.txt

clean:
	rm -rf $(OUT)

.PHONY: clean
