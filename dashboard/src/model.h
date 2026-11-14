/* Everything the dashboard shows, worked out from GPS and CarInfo data.
 *
 * Rules agreed for the design (see docs/vehicle-data-research.md, "Dashboard"):
 *   elevation  GPS altitude (10-second average) first; the car's barometric elevation, shifted by
 *              the last GPS-minus-car difference, only while GPS has no fix
 *   heading    GPS course while moving (on above 6 km/h, off below 4 km/h); the car's 8-point
 *              compass when stopped or without GPS
 *   speeds     average while moving and top speed, kept in memory since the dashboard started
 * Portable C (no Windows headers) so it can be tested on the build machine. */
#ifndef MODEL_H
#define MODEL_H

#include "carinfo.h"
#include "nmea.h"

#define SERIES_MAX 120

typedef struct {
    float v[SERIES_MAX];
    unsigned t[SERIES_MAX];     /* tick (ms) when each sample was taken */
    int cap, count, head;       /* head = index of the oldest sample */
} Series;

void series_init(Series *s, int cap);
void series_push(Series *s, float value, unsigned tick);
/* i = 0 is the oldest sample. Returns 0 when i is out of range. */
int series_get(const Series *s, int i, float *value, unsigned *tick);

enum { SRC_NONE, SRC_GPS, SRC_CAR };
enum { BATT_UNKNOWN, BATT_CHARGING, BATT_NOT_CHARGING, BATT_LOW, BATT_HIGH };
enum { WEATHER_UNKNOWN, WEATHER_FALLING_FAST, WEATHER_FALLING, WEATHER_STEADY, WEATHER_RISING };
enum { SUN_NONE, SUN_SUNSET, SUN_SUNRISE };

/* Sampling: pressure every 30 s (1 h shown), outside temperature every 90 s (3 h), battery every
 * 15 s (30 min), elevation every 250 m driven (20 km), sea-level pressure every 90 s (3 h). */
#define PRESSURE_EVERY_MS 30000u
#define OUTSIDE_EVERY_MS 90000u
#define BATTERY_EVERY_MS 15000u
#define SEALEVEL_EVERY_MS 90000u
#define ELEVATION_EVERY_M 250.0f
#define ELEVATION_POINTS 80

typedef struct {
    unsigned now_ms;                  /* GetTickCount() */
    int clock_valid;                  /* the unit's local clock */
    int year, month, day, hour, minute, second;
    int system_utc_offset_min;        /* local minus UTC from the unit's settings (fallback) */
    int gps_fresh;                    /* an RMC sentence arrived in the last 3 s */
    NmeaState gps;
    int car_fresh;                    /* a CarInfo reply arrived in the last 30 s */
    CarInfoState car;
    int lights_on;                    /* headlights, from the bus (0x52A index 9 or CarInfo 0x33) */
} DashInputs;

typedef struct {
    unsigned now_ms;
    int hour, minute, clock_valid;
    int gps_ok, satellites, car_ok, night;

    int have_speed;   float speed_kmh;
    int heading_src;  float heading_deg;  int compass_point;   /* compass_point 1..8 for SRC_CAR */
    int elev_src;     float elev_m;
    int have_climb;   float climb_m_min;
    int have_pressure; int pressure_hpa; int weather;
    int have_outside;  int outside_c;  int have_temp_change; float temp_change; unsigned temp_span_s;
    int have_battery;  float battery_v; int battery_state;
    int have_avg;      float avg_kmh;
    int have_top;      float top_kmh;
    int have_since;    int since_hour, since_minute;
    int sun_event;     int sun_hour, sun_minute, sun_minutes_left;   /* minutes_left for SUN_SUNSET */

    Series pressure, elevation, outside, battery;

    /* internal */
    int started;
    unsigned last_ms;
    float alt[10]; int alt_count, alt_next; unsigned alt_last_ms;
    int have_offset; float car_offset;          /* GPS elevation minus the car's elevation */
    int gps_heading;                            /* hysteresis state */
    float speeds[3]; int speed_count;
    double moving_m, moving_s, elev_dist_m;
    float climb_v[24]; unsigned climb_t[24]; int climb_count, climb_next, climb_src; unsigned climb_last_ms;
    Series sealevel;
    unsigned pressure_ms, outside_ms, battery_ms, sealevel_ms;
    int have_position; double lat, lon;
    unsigned sun_ms; int sun_valid;
} DashModel;

void dash_init(DashModel *m);
/* Call about once a second. */
void dash_update(DashModel *m, const DashInputs *in);

/* "N", "NE", ... for a heading in degrees. */
const char *dash_point_name(float degrees);

#endif
