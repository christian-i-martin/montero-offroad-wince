#include "carinfo.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32_WCE
#define snprintf _snprintf
#endif

#define NO_DATA_16 0xFFFF
#define NO_DATA_8 0xFF

static int be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static int sbe16(const unsigned char *p) { int v = be16(p); return v >= 0x8000 ? v - 0x10000 : v; }
static int s8(unsigned char v) { return v >= 0x80 ? (int)v - 0x100 : (int)v; }

static int set(int *have, int *field, int value)
{
    *have = 1;
    *field = value;
    return 1;
}

/* Trip computer block (ids 0x0A..0x10), used by commands 0x00 (from p7) and 0x41 (from p0).
 * Ids 0x0A and 0x0C are not shown by the factory screen and are ignored here too. */
static int apply_status(CarInfoState *s, const unsigned char *b)
{
    int n = 0;
    if (b[1] != NO_DATA_8) n += set(&s->have_avg_speed, &s->avg_speed_kmh, b[1]);
    if (be16(b + 4) != NO_DATA_16) n += set(&s->have_avg_fuel, &s->avg_fuel_dkmpl, be16(b + 4));
    if (be16(b + 6) != NO_DATA_16) n += set(&s->have_range, &s->range_km, be16(b + 6));
    if (be16(b + 8) != NO_DATA_16) n += set(&s->have_service_dist, &s->service_dist, be16(b + 8));
    n += set(&s->have_service_due, &s->service_due, b[10] & 1);
    return n;
}

int carinfo_apply(CarInfoState *s, const unsigned char *frame, int length)
{
    const unsigned char *p = frame + 2;
    int payload = length - 2;
    int n = 0;

    if (frame == 0 || length < 2)
        return -1;
    switch (frame[0]) {
    case 0x00: /* full "RV meter" frame */
        if (payload < 7) return -1;
        n += set(&s->have_compass, &s->compass, p[0]);
        n += set(&s->have_pressure, &s->pressure_hpa, be16(p + 1));
        n += set(&s->have_elevation, &s->elevation_m, sbe16(p + 3));
        n += set(&s->have_outside, &s->outside_c, s8(p[5]));
        if (payload >= 7 + 11)
            n += apply_status(s, p + 7);
        break;
    case 0x10: case 0x11:
        if (payload < 1) return -1;
        n += set(&s->have_compass, &s->compass, p[0]);
        break;
    case 0x20: case 0x21:
        if (payload < 4) return -1;
        n += set(&s->have_pressure, &s->pressure_hpa, be16(p));
        n += set(&s->have_elevation, &s->elevation_m, sbe16(p + 2));
        break;
    case 0x23:
        if (payload < 2) return -1;
        n += set(&s->have_pressure, &s->pressure_hpa, be16(p));
        break;
    case 0x24: case 0x25:
        if (payload < 2) return -1;
        n += set(&s->have_elevation, &s->elevation_m, sbe16(p));
        break;
    case 0x30: case 0x31:
        if (payload < 1) return -1;
        n += set(&s->have_outside, &s->outside_c, s8(p[0]));
        break;
    case 0x40: /* service reminder and battery (Mitsubishi "Strada" variant) */
        if (payload < 4) return -1;
        if ((p[0] | (p[1] << 8)) != NO_DATA_16)
            n += set(&s->have_service_dist, &s->service_dist, p[0] | (p[1] << 8));
        if (p[2] != NO_DATA_8) n += set(&s->have_service_month, &s->service_month, p[2]);
        n += set(&s->have_battery, &s->battery_dv, p[3]);
        break;
    case 0x41:
        if (payload < 11) return -1;
        n += apply_status(s, p);
        break;
    case 0x33: /* lights status pushed by the MCU (probe run 3: headlights on = 0x04) */
        if (payload < 1) return -1;
        n += set(&s->have_lights, &s->lights, p[0]);
        break;
    case 0x12: case 0x13: case 0x14: case 0x15: case 0x22:
    case 0x42: case 0x43: case 0x44: case 0x58:
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:
    case 0x67: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x90:
        break; /* acknowledgements, calibration and tyre frames: logged, not shown */
    default:
        return -1;
    }
    s->frames++;
    return n;
}

const char *carinfo_compass_name(int value)
{
    static const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return value >= 1 && value <= 8 ? names[value - 1] : "?";
}

static int append(char *out, int size, int used, const char *text)
{
    int len = (int)strlen(text);
    if (used + len >= size)
        len = size - used - 1;
    if (len > 0) {
        memcpy(out + used, text, (size_t)len);
        used += len;
    }
    out[used] = '\0';
    return used;
}

int carinfo_format(const CarInfoState *s, char *out, int size)
{
    char part[48];
    int used = 0;

    if (out == 0 || size <= 0)
        return 0;
    out[0] = '\0';
#define ITEM(cond, ...) do { snprintf(part, sizeof part, __VA_ARGS__); part[sizeof part - 1] = '\0'; \
        used = append(out, size, used, used ? " | " : ""); \
        used = append(out, size, used, (cond) ? part : "--"); } while (0)
    ITEM(s->have_compass, "Compass %s", carinfo_compass_name(s->compass));
    ITEM(s->have_pressure, "%d hPa", s->pressure_hpa);
    ITEM(s->have_elevation, "%d m", s->elevation_m);
    ITEM(s->have_outside, "Out %d C", s->outside_c);
    ITEM(s->have_battery, "%d.%d V", s->battery_dv / 10, s->battery_dv % 10);
    ITEM(s->have_avg_speed, "Avg %d km/h", s->avg_speed_kmh);
    ITEM(s->have_avg_fuel, "%d.%d km/L", s->avg_fuel_dkmpl / 10, s->avg_fuel_dkmpl % 10);
    ITEM(s->have_range, "Range %d km", s->range_km);
    ITEM(s->have_lights, "Lights %02x", s->lights);
#undef ITEM
    return used;
}
