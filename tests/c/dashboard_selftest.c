/* Self-test for the dashboard's portable parts: model rules, sunrise/sunset, drawing calls. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map.h"
#include "model.h"
#include "render.h"
#include "sun.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

static DashInputs in;
static DashModel m;

/* London, 2024-12-21 12:00 local = UTC, moving north-west-ish at 10 km/h, car data present. */
static void reset(void)
{
    memset(&in, 0, sizeof in);
    in.now_ms = 1000;
    in.clock_valid = 1;
    in.year = 2024; in.month = 12; in.day = 21; in.hour = 12; in.minute = 0; in.second = 0;
    in.gps_fresh = 1;
    in.gps.have_rmc = 1; in.gps.valid = 1;
    in.gps.have_gga = 1; in.gps.fix_quality = 1; in.gps.satellites = 9;
    in.gps.speed_dkmh = 100; in.gps.course_ddeg = 1270; in.gps.altitude_dm = 2790;
    in.gps.have_time = 1; in.gps.utc_seconds = 12 * 3600;
    in.gps.have_date = 1; in.gps.year = 2024; in.gps.month = 12; in.gps.day = 21;
    in.gps.have_position = 1; in.gps.lat_e6 = 51507400; in.gps.lon_e6 = -127800;
    in.car_fresh = 1;
    in.car.have_compass = 1; in.car.compass = 4;
    in.car.have_elevation = 1; in.car.elevation_m = 200;
    in.car.have_pressure = 1; in.car.pressure_hpa = 988;
    in.car.have_outside = 1; in.car.outside_c = 30;
    in.car.have_battery = 1; in.car.battery_dv = 140;
    dash_init(&m);
}

static void step(int seconds)
{
    int i;
    for (i = 0; i < seconds; i++) {
        dash_update(&m, &in);
        in.now_ms += 1000;
        if (++in.second == 60) {
            in.second = 0;
            if (++in.minute == 60) { in.minute = 0; in.hour++; }
        }
        in.gps.utc_seconds = (in.gps.utc_seconds + 1) % 86400;
    }
}

static void test_series(void)
{
    Series s;
    float v;
    unsigned t;
    int i;
    series_init(&s, 3);
    for (i = 1; i <= 5; i++)
        series_push(&s, (float)i, (unsigned)i * 10);
    CHECK(s.count == 3);
    CHECK(series_get(&s, 0, &v, &t) && v == 3.0f && t == 30);
    CHECK(series_get(&s, 2, &v, NULL) && v == 5.0f);
    CHECK(!series_get(&s, 3, &v, NULL));
}

static void test_heading(void)
{
    reset();
    step(1);
    CHECK(m.heading_src == SRC_GPS && NEAR(m.heading_deg, 127.0, 0.01));
    CHECK(strcmp(dash_point_name(m.heading_deg), "SE") == 0);
    in.gps.speed_dkmh = 50;   /* 5 km/h: stays on GPS (hysteresis) */
    step(1);
    CHECK(m.heading_src == SRC_GPS);
    in.gps.speed_dkmh = 30;   /* 3 km/h: the car's compass */
    step(1);
    CHECK(m.heading_src == SRC_CAR && m.compass_point == 4 && NEAR(m.heading_deg, 135.0, 0.01));
    in.gps.speed_dkmh = 50;   /* back to 5 km/h: still the compass until 6 km/h */
    step(1);
    CHECK(m.heading_src == SRC_CAR);
    in.gps.speed_dkmh = 70;
    step(1);
    CHECK(m.heading_src == SRC_GPS);
    in.gps_fresh = 0;
    step(1);
    CHECK(m.heading_src == SRC_CAR && !m.gps_ok && !m.have_speed);
    in.car_fresh = 0;
    step(1);
    CHECK(m.heading_src == SRC_NONE);
    CHECK(strcmp(dash_point_name(350.0f), "N") == 0 && strcmp(dash_point_name(-90.0f), "W") == 0);
}

static void test_elevation(void)
{
    reset();
    step(2);
    CHECK(m.elev_src == SRC_CAR && m.elev_m == 200.0f); /* GPS needs 3 samples first */
    step(10);
    CHECK(m.elev_src == SRC_GPS && NEAR(m.elev_m, 279.0, 0.01));
    CHECK(m.have_offset && NEAR(m.car_offset, 79.0, 0.01));
    in.gps_fresh = 0;          /* GPS lost: the car's sensor plus the last difference */
    step(1);
    CHECK(m.elev_src == SRC_CAR && NEAR(m.elev_m, 279.0, 0.01));
    in.car.elevation_m = 205;
    step(1);
    CHECK(NEAR(m.elev_m, 284.0, 0.01));
    in.car_fresh = 0;
    step(6);
    CHECK(m.elev_src == SRC_NONE);
    in.gps_fresh = 1;          /* GPS back after more than 5 s: the average starts again */
    step(1);
    CHECK(m.elev_src == SRC_NONE);
    step(2);
    CHECK(m.elev_src == SRC_GPS);
}

static void test_speeds(void)
{
    float avg;
    reset();
    in.gps.speed_dkmh = 360;   /* 36 km/h = 10 m/s */
    step(50);
    CHECK(m.have_speed && m.speed_kmh == 36.0f && !m.have_avg);
    step(51);
    CHECK(m.have_avg && NEAR(m.avg_kmh, 36.0, 0.01));
    CHECK(m.elevation.count == 5);     /* first sample, then every 250 m: 1000 m driven */
    in.gps.speed_dkmh = 1500;  /* one spike is ignored by the median */
    step(1);
    in.gps.speed_dkmh = 360;
    step(1);
    CHECK(m.have_top && m.top_kmh == 36.0f);
    avg = m.avg_kmh;
    in.gps.speed_dkmh = 15;    /* 1.5 km/h while parked reads as 0 and is not "moving" */
    step(30);
    CHECK(m.speed_kmh == 0.0f && m.avg_kmh == avg);
    CHECK(m.have_since && m.since_hour == 12 && m.since_minute == 0);
}

static void test_battery(void)
{
    static const int dv[] = {141, 132, 131, 125, 122, 118, 150};
    static const int want[] = {BATT_CHARGING, BATT_CHARGING, BATT_NOT_CHARGING, BATT_NOT_CHARGING, BATT_NOT_CHARGING,
                               BATT_LOW, BATT_HIGH};
    int i;
    reset();
    for (i = 0; i < 7; i++) {
        in.car.battery_dv = dv[i];
        step(1);
        CHECK(m.battery_state == want[i]);
    }
    in.car_fresh = 0;
    step(1);
    CHECK(!m.have_battery && m.battery_state == BATT_UNKNOWN);
}

static void test_weather_and_temperature(void)
{
    int i;
    reset();
    in.gps.speed_dkmh = 0;
    step(3500);
    CHECK(m.weather == WEATHER_UNKNOWN);   /* less than an hour of sea-level pressure */
    step(400);
    CHECK(m.weather == WEATHER_STEADY);

    reset();
    in.gps.speed_dkmh = 0;
    for (i = 0; i < 5400; i++) {           /* 990 down to 986 hPa in 1.5 h: 8 hPa per 3 h */
        in.car.pressure_hpa = 990 - (i * 4) / 5400;
        step(1);
    }
    CHECK(m.weather == WEATHER_FALLING_FAST);

    reset();
    step(1200);
    in.car.outside_c = 26;
    step(1200);
    CHECK(m.have_temp_change && NEAR(m.temp_change, -4.0, 0.01) && m.temp_span_s >= 1800);
    CHECK(m.outside.count >= 26);
}

static void test_climb(void)
{
    int i;
    reset();
    in.gps.speed_dkmh = 0;
    for (i = 0; i < 120; i++) {            /* 0.2 m/s = 12 m/min */
        in.gps.altitude_dm = 2790 + i * 2;
        step(1);
    }
    CHECK(m.have_climb && NEAR(m.climb_m_min, 12.0, 1.0));
    in.gps_fresh = 0;                      /* the source changes: start over */
    step(1);
    CHECK(!m.have_climb);
}

static void test_clock(void)
{
    long days;
    int y, mo, d;

    reset();                        /* the unit's clock agrees with GPS: clock from GPS, zone 0 */
    step(1);
    CHECK(m.clock_valid && m.clock_src == SRC_GPS && m.utc_offset_min == 0 && m.hour == 12 && m.minute == 0);

    reset();                        /* this car: the unit's clock says 2012, so its time-zone setting */
    in.clock_valid = 0;
    in.year = 2012; in.month = 6; in.day = 8; in.hour = 15; in.minute = 4;
    in.system_utc_offset_min = 480;
    in.gps.utc_seconds = 4 * 3600;  /* 04:00 UTC = 12:00 at UTC+8 */
    step(1);
    CHECK(m.clock_valid && m.clock_src == SRC_GPS && m.utc_offset_min == 480);
    CHECK(m.hour == 12 && m.minute == 0 && m.year == 2024 && m.month == 12 && m.day == 21);
    CHECK(m.have_since && m.since_hour == 12 && m.since_minute == 0);
    in.tz_override = 1;             /* the menu choice wins */
    in.tz_override_min = -300;
    step(1);
    CHECK(m.utc_offset_min == -300 && m.hour == 23 && m.day == 20);
    in.gps_fresh = 0;               /* GPS lost: the clock keeps running from the last GPS time */
    step(90);
    CHECK(m.clock_valid && m.clock_src == SRC_GPS && m.hour == 23 && m.minute == 1);

    reset();                        /* a receiver reporting the date 1024 weeks early */
    days = sun_days_from_civil(2024, 12, 21) - 7168;
    sun_civil_from_days(days, &y, &mo, &d);
    in.gps.year = y; in.gps.month = mo; in.gps.day = d;
    step(1);
    CHECK(y < 2020 && m.year == 2024 && m.month == 12 && m.day == 21);

    reset();                        /* no fix and no clock for 10 minutes: no time, then GPS */
    in.clock_valid = 0;
    in.gps.valid = 0;
    step(600);
    CHECK(!m.clock_valid && m.clock_src == SRC_NONE && !m.have_since);
    in.gps.valid = 1;
    step(1);
    CHECK(m.clock_valid && m.hour == 12 && m.minute == 10);
    CHECK(m.have_since && m.since_hour == 12 && m.since_minute == 0); /* still the start time */
}

static void test_sun(void)
{
    double rise, set;
    int y, mo, d;

    CHECK(sun_times(2024, 12, 21, 51.5074, -0.1278, &rise, &set));
    CHECK(NEAR(rise, 8 * 60 + 4, 3) && NEAR(set, 15 * 60 + 53, 3));
    CHECK(sun_times(2024, 6, 20, 40.7128, -74.0060, &rise, &set));      /* New York, sunset after 00:00 UTC */
    CHECK(NEAR(rise, 9 * 60 + 25, 3) && NEAR(set, 24 * 60 + 31, 3));
    CHECK(sun_times(2024, 6, 21, -33.8688, 151.2093, &rise, &set));     /* Sydney, sunrise before 00:00 UTC */
    CHECK(NEAR(rise, -3 * 60, 3) && NEAR(set, 6 * 60 + 54, 3));
    CHECK(!sun_times(2024, 12, 21, 69.65, 18.96, &rise, &set));         /* Tromso: polar night */
    sun_civil_from_days(sun_days_from_civil(2024, 2, 28) + 1, &y, &mo, &d);
    CHECK(y == 2024 && mo == 2 && d == 29);
    sun_civil_from_days(sun_days_from_civil(2024, 12, 31) + 1, &y, &mo, &d);
    CHECK(y == 2025 && mo == 1 && d == 1);

    reset();
    step(1);
    CHECK(m.sun_event == SUN_SUNSET && m.sun_hour == 15 && (m.sun_minute == 53 || m.sun_minute == 54));
    CHECK(m.sun_minutes_left >= 232 && m.sun_minutes_left <= 235);

    reset();                                /* the unit's clock is an hour ahead of GPS time */
    in.hour = 13;
    step(1);
    CHECK(m.sun_event == SUN_SUNSET && m.sun_hour == 16);

    reset();
    in.hour = 16; in.minute = 30; in.gps.utc_seconds = 16 * 3600 + 30 * 60;
    step(1);
    CHECK(m.sun_event == SUN_SUNRISE && m.sun_hour == 8 && m.sun_minute <= 10);

    reset();
    in.gps.have_position = 0;
    step(1);
    CHECK(!m.have_position && m.sun_event == SUN_NONE);
}

/* ---------------------------------------------------------------- drawing calls */

static char g_texts[8192];
static int g_calls, g_bad;

static void rec_fill(void *ctx, int x, int y, int w, int h, unsigned rgb) { g_calls++; if (w < 0 || h < 0) g_bad++; }
static void rec_round(void *ctx, int x, int y, int w, int h, int r, unsigned f, unsigned b) { g_calls++; }
static DPoint g_curve[256]; /* the elevation graph's curve, as last drawn */
static int g_curve_n;
static void rec_poly(void *ctx, const DPoint *p, int n, int w, unsigned rgb)
{
    int i;
    g_calls++;
    for (i = 0; i < n; i++)
        if (p[i].x < 0 || p[i].x > SCREEN_W || p[i].y < 0 || p[i].y > SCREEN_H)
            g_bad++;
    if (w == 2 && n >= 2 && n <= 256 && p[0].x == 20 && p[0].y >= 300 && p[0].y <= 370) {
        memcpy(g_curve, p, (size_t)n * sizeof *p);
        g_curve_n = n;
    }
}
static void rec_polygon(void *ctx, const DPoint *p, int n, unsigned f) { rec_poly(ctx, p, n, 1, f); }
static unsigned g_circle_fill; /* fill of the last circle drawn (the map marker comes last) */
static void rec_circle(void *ctx, int cx, int cy, int r, unsigned f, unsigned b, int w) { g_calls++; g_circle_fill = f; }
static int g_width_percent = 50; /* character width as a share of the font size */
static int rec_width(void *ctx, int font, const char *s)
{
    return (int)strlen(s) * render_fonts[font].px * g_width_percent / 100;
}
static void rec_text(void *ctx, int x, int baseline, int font, unsigned rgb, const char *s)
{
    g_calls++;
    if (x < 0 || x > SCREEN_W || baseline < 0 || baseline > SCREEN_H)
        g_bad++;
    if (strlen(g_texts) + strlen(s) + 2 < sizeof g_texts) {
        strcat(g_texts, s);
        strcat(g_texts, "|");
    }
}

static int g_glass_calls;
static void rec_glass(void *ctx, int x, int y, int w, int h, int r, unsigned rgb, int alpha)
{
    g_calls++;
    g_glass_calls++;
    if (x < 0 || y < 0 || x + w > SCREEN_W || y + h > SCREEN_H || alpha <= 0 || alpha > 256 || 2 * r > h)
        g_bad++;
}

static const Canvas g_rec = {NULL, rec_fill, rec_round, rec_poly, rec_polygon, rec_circle, rec_width, rec_text, NULL,
                             rec_glass};

static int g_map_calls, g_map_zoom;
static int rec_map(void *ctx, int x, int y, int w, int h, int zoom, double px, double py, int night)
{
    g_calls++;
    g_map_calls++;
    g_map_zoom = zoom;
    if (x < 0 || y < 0 || x + w > SCREEN_W || y + h > SCREEN_H || px <= 0 || py <= 0)
        g_bad++;
    return MAP_SHOWN;
}

/* Speed graph sampling, the speed panel and the map tile in its states. */
static void test_speed_and_map(void)
{
    Canvas with_map = g_rec;
    float v;
    unsigned t;

    with_map.map = rec_map;
    reset();
    in.gps.speed_dkmh = 360;
    step(61);                               /* a graph point at once, then every 5 s */
    CHECK(m.speed.count == 13);
    CHECK(series_get(&m.speed, 12, &v, &t) && NEAR(v, 36.0, 0.01));
    in.gps_fresh = 0;                       /* no GPS: no points */
    step(20);
    CHECK(m.speed.count == 13);
    in.gps_fresh = 1;
    in.gps.speed_dkmh = 15;                 /* parked: 0 km/h */
    step(10);
    CHECK(m.speed.count > 13 && series_get(&m.speed, m.speed.count - 1, &v, NULL) && v == 0.0f);

    g_texts[0] = '\0';
    g_bad = g_map_calls = 0;
    render_dashboard(&with_map, &m);        /* no map files yet */
    CHECK(g_bad == 0 && g_map_calls == 0 && strstr(g_texts, "No map files") != NULL);
    CHECK(strstr(g_texts, "|SPEED|") == NULL); /* labels are drawn letter by letter */
    CHECK(strstr(g_texts, "|A|V|G|T|O|P|") != NULL);
    CHECK(strstr(g_texts, "|36|36|") != NULL);   /* average and top speed 36 */

    m.map_min_zoom = 8;
    m.map_max_zoom = 15;
    m.map_zoom = 14;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);           /* a platform without map drawing */
    CHECK(strstr(g_texts, "No map files") != NULL);
    g_texts[0] = '\0';
    render_dashboard(&with_map, &m);
    CHECK(g_bad == 0 && g_map_calls == 1 && g_map_zoom == 14);
    CHECK(strstr(g_texts, "North up") == NULL && strstr(g_texts, "\xC2\xA9 OpenStreetMap")); /* no title */
    CHECK(strstr(g_texts, "|300 m|") != NULL); /* London at zoom 14: about 6 m per pixel */
    CHECK(strstr(g_texts, "|N|") != NULL);     /* north up */
    CHECK(render_map_hit(490, 448) == MAPHIT_ZOOM_IN);  /* bottom centre: minus | plus */
    CHECK(render_map_hit(440, 448) == MAPHIT_ZOOM_OUT);
    CHECK(render_map_hit(400, 260) == MAPHIT_NONE && render_map_hit(520, 448) == MAPHIT_NONE);
    CHECK(render_map_hit(490, 420) == MAPHIT_NONE);

    in.gps_fresh = 0;                       /* GPS lost: the last position stays on the map */
    step(1);
    g_texts[0] = '\0';
    render_dashboard(&with_map, &m);
    CHECK(g_map_calls == 2 && g_circle_fill == 0x4B5763); /* the marker turns grey (day: muted) */

    dash_init(&m);                          /* map files but no position yet */
    m.map_min_zoom = 8;
    m.map_max_zoom = 15;
    g_texts[0] = '\0';
    g_bad = 0;
    render_dashboard(&with_map, &m);
    CHECK(g_bad == 0 && g_map_calls == 2 && strstr(g_texts, "Waiting for GPS") != NULL);
}

/* Graphs show a line from the first sample on; the address tile in each of its states. */
static void test_address_and_graphs(void)
{
    char long_road[ADDRESS_TEXT];

    reset();
    step(1);                                /* one sample in every series */
    g_texts[0] = '\0';
    g_bad = 0;
    render_dashboard(&g_rec, &m);
    CHECK(g_bad == 0 && strstr(g_texts, "Collecting data") == NULL);
    CHECK(strstr(g_texts, "No address file") != NULL);
    CHECK(strstr(g_texts, "Copy luzon.addr into the Map folder") != NULL);
    m.address_state = ADDRESS_UNREADABLE;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "Address file cannot be read") != NULL && strstr(g_texts, "No address file") == NULL);

    m.address_state = ADDRESS_WAITING;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "Waiting for GPS") != NULL);
    m.address_state = ADDRESS_OUTSIDE;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "Outside the address area") != NULL);

    m.address_state = ADDRESS_OK;
    strcpy(m.addr_road, "Sumulong Highway");
    strcpy(m.addr_barangay, "San Isidro");
    strcpy(m.addr_city, "Antipolo");
    strcpy(m.addr_province, "Rizal");
    strcpy(m.addr_place, "Cupang");
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|Sumulong Highway|Brgy. San Isidro \xC2\xB7 Antipolo, Rizal|") != NULL);
    CHECK(strstr(g_texts, "Last GPS position") == NULL);

    m.addr_road[0] = 0;                     /* no road, a numbered barangay, no city */
    strcpy(m.addr_barangay, "Barangay 649");
    m.addr_city[0] = 0;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|No named road nearby|Barangay 649 \xC2\xB7 Rizal|") != NULL);
    m.addr_barangay[0] = 0;                 /* no barangay: the nearest place */
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|Near Cupang \xC2\xB7 Rizal|") != NULL);

    memset(long_road, 'W', sizeof long_road - 1); /* too wide: cut with an ellipsis */
    long_road[sizeof long_road - 1] = 0;
    strcpy(m.addr_road, long_road);
    g_texts[0] = '\0';
    g_bad = 0;
    render_dashboard(&g_rec, &m);
    CHECK(g_bad == 0 && strstr(g_texts, "W\xE2\x80\xA6|") != NULL && strstr(g_texts, long_road) == NULL);

    in.gps_fresh = 0;                       /* GPS lost: the last address stays, marked */
    step(1);
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "Last GPS position") != NULL);
}

/* Dark colours from sunset to sunrise; the headlights decide only before GPS time and position. */
static void test_colours(void)
{
    reset();
    in.lights_on = 1;                       /* headlights on at noon: still day colours */
    step(1);
    CHECK(!m.night && m.night_src == SRC_GPS && m.lights_on);
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "Headlights on") != NULL);

    reset();                                /* 16:30 in London in December: after sunset */
    in.hour = 16; in.minute = 30; in.gps.utc_seconds = 16 * 3600 + 30 * 60;
    step(1);
    CHECK(m.night && m.night_src == SRC_GPS && !m.lights_on);
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|Sunrise 8:0") != NULL && strstr(g_texts, " AM|") != NULL); /* 12-hour times */
    CHECK(strstr(g_texts, "Headlights on") == NULL);

    reset();                                /* 06:00: before sunrise */
    in.hour = 6; in.gps.utc_seconds = 6 * 3600;
    step(1);
    CHECK(m.night && m.night_src == SRC_GPS);

    reset();                                /* no position yet: the headlights decide */
    in.gps.have_position = 0;
    step(1);
    CHECK(!m.night && m.night_src == SRC_CAR);
    in.lights_on = 1;
    step(1);
    CHECK(m.night && m.night_src == SRC_CAR);
    in.gps.have_position = 1;               /* the first fix: the sun from now on */
    step(1);
    CHECK(!m.night && m.night_src == SRC_GPS);
}

/* Graphs are lines through every sample, so a change of a single sample shows (the earlier smooth
 * curve averaged such changes away). */
static void test_graph_lines(void)
{
    int i, top = 1000, at_top = 0;

    reset();
    step(1);
    series_init(&m.elevation, ELEVATION_POINTS);
    for (i = 0; i < ELEVATION_POINTS; i++)   /* flat, with one sample 60 m higher */
        series_push(&m.elevation, i == 40 ? 560.0f : 500.0f, (unsigned)i);
    g_curve_n = 0;
    render_dashboard(&g_rec, &m);
    CHECK(g_curve_n == ELEVATION_POINTS + 1); /* the flat start at the left edge, then every sample */
    for (i = 0; i < g_curve_n; i++) {
        if (g_curve[i].y < top) {
            top = g_curve[i].y;
            at_top = 0;
        }
        if (g_curve[i].y == top)
            at_top++;
    }
    CHECK(top == 310 && at_top == 1);        /* the one high sample at the top of the graph */
    CHECK(g_curve[0].x == 20 && g_curve[0].y == 361 && g_curve[g_curve_n - 1].x == 132);
}

/* The car info page: header, the 16 tiles with the plate, Back; the header button that opens it. */
static void test_carinfo(void)
{
    static const char head[] = "Back|Mitsubishi Montero Sport|2016 \xC2\xB7 2.4 diesel 4x2|12:00 PM|";

    reset();
    step(1);
    g_texts[0] = '\0';
    g_bad = 0;
    render_carinfo(&g_rec, &m);
    CHECK(g_bad == 0);
    CHECK(strncmp(g_texts, head, sizeof head - 1) == 0);
    CHECK(strstr(g_texts, "|N|D|E| |4|3|9|1|") != NULL); /* the plate, letter by letter */
    CHECK(strstr(g_texts, "|700|mm|Maximum, at walking pace|") != NULL);
    CHECK(strstr(g_texts, "|265/60 R18|18 in alloy wheels|") != NULL);
    CHECK(strstr(g_texts, "|4x2|Rear wheels, 8-speed auto|") != NULL);
    CHECK(strstr(g_texts, "|2,710|kg|Fully loaded maximum|") != NULL);
    m.night = 1;
    g_bad = 0;
    render_carinfo(&g_rec, &m);
    CHECK(g_bad == 0);
    CHECK(render_carinfo_back_hit(40, 20) && !render_carinfo_back_hit(300, 20) && !render_carinfo_back_hit(40, 100));
    CHECK(render_carinfo_button_hit(760, 20) && !render_carinfo_button_hit(600, 20));
    CHECK(!render_carinfo_button_hit(760, 100));
    CHECK(!render_clock_hit(760, 20));       /* the button is not the clock */
}

static void test_render(void)
{
    reset();
    in.gps.speed_dkmh = 420;
    step(200);
    g_texts[0] = '\0';
    g_calls = g_bad = 0;
    render_dashboard(&g_rec, &m);
    CHECK(g_calls > 100 && g_bad == 0 && g_glass_calls >= 10);
    CHECK(strstr(g_texts, "|km/h|42|") != NULL);
    CHECK(strstr(g_texts, "|279|m|") != NULL);
    CHECK(strstr(g_texts, "|988|hPa|") != NULL);
    CHECK(strstr(g_texts, "|14.0|V|") != NULL);
    CHECK(strstr(g_texts, "|SE|127\xC2\xB0|") != NULL && strstr(g_texts, "GPS course") != NULL);
    CHECK(strncmp(g_texts, "12:03|PM|", 9) == 0);           /* 12-hour clock, drawn first */
    CHECK(strstr(g_texts, "|Sunset 3:5") != NULL && strstr(g_texts, " PM, 3 h ") != NULL);
    CHECK(strstr(g_texts, "GPS 9 sats") != NULL);
    CHECK(strstr(g_texts, "No car data") == NULL && strstr(g_texts, "Headlights on") == NULL);

    in.gps_fresh = 0;
    in.car_fresh = 0;
    in.lights_on = 1;
    step(1);
    g_texts[0] = '\0';
    g_bad = 0;
    render_dashboard(&g_rec, &m);
    CHECK(g_bad == 0);
    CHECK(strstr(g_texts, "Waiting for GPS") && strstr(g_texts, "No car data") && strstr(g_texts, "Headlights on"));
    CHECK(strstr(g_texts, "|Sunset 3:5") != NULL && strstr(g_texts, "min left") == NULL); /* shortened: no room */
    CHECK(strstr(g_texts, "|km/h|--|") != NULL);

    in.car_fresh = 1;                       /* the car's sensor: named, shortened when it does not fit */
    step(1);
    CHECK(m.elev_src == SRC_CAR);
    g_width_percent = 30;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|Car sensor|") != NULL && strstr(g_texts, "20 km") == NULL);
    g_width_percent = 50;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|Car sensor|") == NULL && strstr(g_texts, "|Car|") != NULL);
    CHECK(strstr(g_texts, "Car compass") != NULL);
    g_width_percent = 200;                  /* very wide fallback font: the note is left out */
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|Car|") == NULL);
    g_width_percent = 50;

    dash_init(&m);                          /* nothing at all yet */
    g_texts[0] = '\0';
    g_bad = 0;
    render_dashboard(&g_rec, &m);
    render_menu(&g_rec, &m, 1, "0.1.0");
    CHECK(g_bad == 0 && strstr(g_texts, "Collecting data") && strstr(g_texts, "Open file manager"));
    CHECK(strstr(g_texts, "Time zone") && strstr(g_texts, "|UTC|") && strstr(g_texts, "No clock yet"));
    CHECK(render_menu_hit(230, 240, 1) == MENU_FILES);
    CHECK(render_menu_hit(230, 308, 1) == MENU_CLOSE);
    CHECK(render_menu_hit(230, 376, 1) == MENU_BACK);
    CHECK(render_menu_hit(230, 240, 0) == MENU_CLOSE);
    CHECK(render_menu_hit(390, 160, 1) == MENU_TZ_MINUS);
    CHECK(render_menu_hit(560, 160, 1) == MENU_TZ_PLUS);
    CHECK(render_menu_hit(10, 10, 1) == MENU_NONE);
    CHECK(render_clock_hit(30, 20) && !render_clock_hit(300, 20) && !render_clock_hit(30, 100));
}

int main(void)
{
    test_series();
    test_heading();
    test_elevation();
    test_speeds();
    test_battery();
    test_weather_and_temperature();
    test_climb();
    test_clock();
    test_sun();
    test_colours();
    test_graph_lines();
    test_carinfo();
    test_render();
    test_speed_and_map();
    test_address_and_graphs();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dashboard self-test passed\n");
    return 0;
}
