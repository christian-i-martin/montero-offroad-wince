/* Everything the dashboard shows, worked out from GPS and CarInfo data.
 *
 * Rules agreed for the design (see docs/vehicle-data-research.md, "Dashboard"):
 *   elevation  GPS altitude (10-second average) first; the car's barometric elevation, shifted by
 *              the last GPS-minus-car difference, only while GPS has no fix
 *   heading    GPS course while moving (on above 6 km/h, off below 4 km/h); the car's 8-point
 *              compass when stopped or without GPS
 *   speeds     average while moving and top speed, kept in memory since the dashboard started;
 *              the speed graph holds the last 10 minutes (a point every 5 s, the mean of that time)
 *   day/night  dark colours from sunset to sunrise, worked out from the GPS position and time;
 *              the headlights decide only until GPS has given both
 * Portable C (no Windows headers) so it can be tested on the build machine. */
#ifndef MODEL_H
#define MODEL_H

#include "carinfo.h"
#include "holiday.h"
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
enum { ADDRESS_NO_FILE, ADDRESS_WAITING, ADDRESS_OUTSIDE, ADDRESS_OK, ADDRESS_UNREADABLE };
#define ADDRESS_TEXT 96

/* Sampling: pressure every 30 s (1 h shown), outside temperature every 90 s (3 h), battery every
 * 15 s (30 min), sea-level pressure every 90 s (3 h). Elevation: a point every 150 m driven, the
 * last 20 kept, so the graph shows the last 3 km of road (the user's choice). */
#define PRESSURE_EVERY_MS 30000u
#define OUTSIDE_EVERY_MS 90000u
#define BATTERY_EVERY_MS 15000u
#define SEALEVEL_EVERY_MS 90000u
#define ELEVATION_EVERY_M 150.0f
#define ELEVATION_POINTS 20
#define ELEVATION_RANGE "3 km"      /* ELEVATION_POINTS x ELEVATION_EVERY_M, the graph's note when full */
#define SPEED_EVERY_MS 5000u         /* 120 x 5 s = 10 min */
#define SPEED_WINDOW_MS 600000u
#define MAP_DEFAULT_ZOOM 16.0        /* the dashboard map's zoom until another is chosen (then remembered) */

typedef struct {
    unsigned now_ms;                  /* GetTickCount() */
    int clock_valid;                  /* the unit's local clock looks set (year 2020 or later) */
    int year, month, day, hour, minute, second;
    int system_utc_offset_min;        /* local minus UTC from the unit's time-zone setting */
    int tz_override, tz_override_min; /* time zone chosen in the dashboard menu */
    int gps_fresh;                    /* an RMC sentence arrived in the last 3 s */
    NmeaState gps;
    int car_fresh;                    /* a CarInfo reply arrived in the last 30 s */
    CarInfoState car;
    int lights_on;                    /* headlights, from the bus (0x52A index 9 or CarInfo 0x33) */
} DashInputs;

typedef struct {
    unsigned now_ms;
    /* Local time: GPS time plus the time zone when GPS time has been seen (the unit's clock resets
     * to 2012 on this car), otherwise the unit's clock if it looks set. */
    int clock_valid, clock_src;       /* SRC_GPS, SRC_CAR (the head unit's clock) or SRC_NONE */
    int year, month, day, hour, minute, second;
    int utc_offset_min;               /* in use: menu choice, else the unit's clock or setting */
    int gps_ok, satellites, car_ok;
    int gps_wait;                     /* updates in a row without a GPS fix (1 at the first): the
                                       * header's GPS icon shows on odd counts, so it blinks, 1 s on and
                                       * 1 s off at the 1 s update, starting shown */
    int lights_on;                    /* headlights, as the bus reports them */
    int night, night_src;             /* dark colours; SRC_GPS: from the sun, SRC_CAR: from the headlights */

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

    Series pressure, elevation, outside, battery, speed;

    /* map view, set by the program (dash.c), not by dash_update: the zoom shown (any fraction) and
     * the range the map file allows (map_max_zoom < 0 when there is no map file); the position drawn
     * where the car is (Web Mercator, 0 to 1: it glides between GPS fixes) and the direction up the
     * screen (degrees: the heading, so the map turns with the car; 0 north up). */
    double map_zoom, map_min_zoom, map_max_zoom;
    double map_mx, map_my, map_bearing;

    /* address, set by the program (dash.c) from the address file; UTF-8, empty when unknown.
     * For the screen only: never log it (it is as revealing as the position). */
    int address_state;                /* ADDRESS_NO_FILE until the program finds an address file;
                                       * ADDRESS_UNREADABLE when *.addr files exist but none opens */
    char addr_road[ADDRESS_TEXT], addr_barangay[ADDRESS_TEXT], addr_city[ADDRESS_TEXT];
    char addr_province[ADDRESS_TEXT], addr_place[ADDRESS_TEXT];
    int addr_road_m, addr_place_m;    /* metres, -1 without a name */

    /* today's holidays, set by the program (dash.c) from the holiday list (holiday.h): national
     * ones from the date, local ones from the town nearest the GPS position. The place of a local
     * one is for the screen only: never log it. */
    int holiday_count;
    HolidayRow holidays[HOLIDAY_ROWS];

    /* internal */
    int started;
    unsigned last_ms;
    float alt[10]; int alt_count, alt_next; unsigned alt_last_ms;
    int have_offset; float car_offset;          /* GPS elevation minus the car's elevation */
    int gps_heading;                            /* hysteresis state */
    float speeds[3]; int speed_count;
    float speed_sum; int speed_n; unsigned speed_ms;   /* speed graph point being collected */
    double moving_m, moving_s, elev_dist_m;      /* elev_dist_m: driven since the last graph point */
    int elev_graph_src;                         /* where the elevation graph's first point came from */
    float climb_v[24]; unsigned climb_t[24]; int climb_count, climb_next, climb_src; unsigned climb_last_ms;
    Series sealevel;
    unsigned pressure_ms, outside_ms, battery_ms, sealevel_ms;
    int have_position; double lat, lon;
    unsigned sun_ms; int sun_valid;
    int have_utc; double utc_base; unsigned utc_base_ms;   /* seconds since 1970 at a tick */
    unsigned start_ms;
} DashModel;

void dash_init(DashModel *m);
/* Call about once a second. */
void dash_update(DashModel *m, const DashInputs *in);

/* "N", "NE", ... for a heading in degrees. */
const char *dash_point_name(float degrees);

#endif
