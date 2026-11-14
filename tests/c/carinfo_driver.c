/* Test driver: apply hex frames given on the command line, then print the decoded state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carinfo.h"

static int parse_hex(const char *text, unsigned char *out, int max)
{
    int n = 0;
    while (*text && n < max) {
        unsigned value;
        if (*text == ' ') { text++; continue; }
        if (sscanf(text, "%2x", &value) != 1) return -1;
        out[n++] = (unsigned char)value;
        text += 2;
    }
    return n;
}

int main(int argc, char **argv)
{
    CarInfoState state;
    unsigned char frame[256];
    char line[300];
    int i;

    memset(&state, 0, sizeof state);
    for (i = 1; i < argc; i++) {
        int n = parse_hex(argv[i], frame, sizeof frame);
        if (n < 0) return 2;
        printf("ret=%d\n", carinfo_apply(&state, frame, n));
    }
#define FIELD(flag, name) if (state.have_##flag) printf(#name "=%d\n", state.name)
    FIELD(compass, compass); FIELD(pressure, pressure_hpa); FIELD(elevation, elevation_m);
    FIELD(outside, outside_c); FIELD(battery, battery_dv); FIELD(avg_speed, avg_speed_kmh);
    FIELD(avg_fuel, avg_fuel_dkmpl); FIELD(range, range_km); FIELD(service_dist, service_dist);
    FIELD(service_month, service_month); FIELD(service_due, service_due); FIELD(lights, lights);
#undef FIELD
    printf("frames=%u\n", state.frames);
    carinfo_format(&state, line, sizeof line);
    printf("summary=%s\n", line);
    return 0;
}
