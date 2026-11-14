/* Self-test for the dashboard's portable parts: model rules, sunrise/sunset, drawing calls. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void rec_poly(void *ctx, const DPoint *p, int n, int w, unsigned rgb)
{
    int i;
    g_calls++;
    for (i = 0; i < n; i++)
        if (p[i].x < 0 || p[i].x > SCREEN_W || p[i].y < 0 || p[i].y > SCREEN_H)
            g_bad++;
}
static void rec_polygon(void *ctx, const DPoint *p, int n, unsigned f) { rec_poly(ctx, p, n, 1, f); }
static void rec_circle(void *ctx, int cx, int cy, int r, unsigned f, unsigned b, int w) { g_calls++; }
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

static const Canvas g_rec = {NULL, rec_fill, rec_round, rec_poly, rec_polygon, rec_circle, rec_width, rec_text};

static void test_render(void)
{
    reset();
    in.gps.speed_dkmh = 420;
    step(200);
    g_texts[0] = '\0';
    g_calls = g_bad = 0;
    render_dashboard(&g_rec, &m);
    CHECK(g_calls > 100 && g_bad == 0);
    CHECK(strstr(g_texts, "|42|km/h|") != NULL);
    CHECK(strstr(g_texts, "|279|m|") != NULL);
    CHECK(strstr(g_texts, "|988|hPa|") != NULL);
    CHECK(strstr(g_texts, "|14.0|V|") != NULL);
    CHECK(strstr(g_texts, "|127\xC2\xB0|SE|") != NULL);
    CHECK(strstr(g_texts, "Sunset 15:5") != NULL);
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
    CHECK(strstr(g_texts, "No GPS fix") && strstr(g_texts, "No car data") && strstr(g_texts, "Headlights on"));
    CHECK(strstr(g_texts, "|--|km/h|") != NULL);

    in.car_fresh = 1;                       /* wide fallback font: short note instead of overlapping */
    step(1);
    CHECK(m.elev_src == SRC_CAR);
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "Car sensor, last 20 km") != NULL);
    g_width_percent = 70;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "Car sensor, last 20 km") == NULL && strstr(g_texts, "|Car sensor|") != NULL);
    g_width_percent = 50;

    dash_init(&m);                          /* nothing at all yet */
    g_texts[0] = '\0';
    g_bad = 0;
    render_dashboard(&g_rec, &m);
    render_menu(&g_rec, &m, 1, "0.1.0");
    CHECK(g_bad == 0 && strstr(g_texts, "Collecting data") && strstr(g_texts, "Open file manager"));
    CHECK(render_menu_hit(230, 200, 1) == MENU_FILES);
    CHECK(render_menu_hit(230, 268, 1) == MENU_CLOSE);
    CHECK(render_menu_hit(230, 336, 1) == MENU_BACK);
    CHECK(render_menu_hit(230, 200, 0) == MENU_CLOSE);
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
    test_sun();
    test_render();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dashboard self-test passed\n");
    return 0;
}
