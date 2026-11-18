/* Self-test for the dashboard's portable parts: model rules, sunrise/sunset, drawing calls. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
#include "render.h"
#include "sun.h"
#include "vmap.h"

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
    CHECK(m.elevation.count == 7);     /* about 980 m driven since GPS took over: 6 points of 150 m */
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
static int g_header_left; /* shapes drawn in the left half of the header (the GPS and lights icons) */
static void rec_poly(void *ctx, const DPoint *p, int n, int w, unsigned rgb)
{
    int i, left = 1;
    g_calls++;
    for (i = 0; i < n; i++) {
        if (p[i].x < 0 || p[i].x > SCREEN_W || p[i].y < 0 || p[i].y > SCREEN_H)
            g_bad++;
        if (p[i].x >= SCREEN_W / 2 || p[i].y >= HEADER_H)
            left = 0;
    }
    g_header_left += left;
    if (w == 2 && n >= 2 && n <= 256 && p[0].x == 20 && p[0].y >= 300 && p[0].y <= 370) {
        memcpy(g_curve, p, (size_t)n * sizeof *p);
        g_curve_n = n;
    }
}
static void rec_polygon(void *ctx, const DPoint *p, int n, unsigned f) { rec_poly(ctx, p, n, 1, f); }
static unsigned g_circle_fill; /* fill of the last circle drawn (the map marker comes last) */
static int g_circle_x, g_circle_y; /* and its centre */
static void rec_circle(void *ctx, int cx, int cy, int r, unsigned f, unsigned b, int w)
{
    g_calls++;
    g_circle_fill = f;
    g_circle_x = cx;
    g_circle_y = cy;
}
static int g_width_percent = 50; /* character width as a share of the font size */
static int rec_width(void *ctx, int font, const char *s)
{
    return (int)strlen(s) * render_fonts[font].px * g_width_percent / 100;
}
static const char *g_watch; /* a text whose colour is recorded in g_watch_rgb */
static unsigned g_watch_rgb;
static void rec_text(void *ctx, int x, int baseline, int font, unsigned rgb, const char *s)
{
    g_calls++;
    if (g_watch && strcmp(s, g_watch) == 0)
        g_watch_rgb = rgb;
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

static int g_map_calls, g_map_cx, g_map_cy;
static double g_map_zoom, g_map_mx, g_map_my, g_map_bearing; /* the view asked for */
static int rec_map(void *ctx, int x, int y, int w, int h, double mx, double my, double zoom, double bearing, int cx,
                   int cy, int night)
{
    g_calls++;
    g_map_calls++;
    g_map_zoom = zoom;
    g_map_mx = mx;
    g_map_my = my;
    g_map_bearing = bearing;
    g_map_cx = cx;
    g_map_cy = cy;
    if (x < 0 || y < 0 || x + w > SCREEN_W || y + h > SCREEN_H || mx <= 0 || mx >= 1 || my <= 0 || my >= 1)
        g_bad++;
    return MAPDRAW_SHOWN;
}

/* The program's part: the map shows the car's position, turned to its heading. */
static void map_at_car(DashModel *d, double bearing)
{
    vmap_mercator(d->lat, d->lon, &d->map_mx, &d->map_my);
    d->map_bearing = bearing;
}

/* Speed graph sampling, the speed panel and the map in its states. */
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
    render_dashboard(&with_map, &m);        /* no map file yet */
    CHECK(g_bad == 0 && g_map_calls == 0 && strstr(g_texts, "No map file") != NULL);
    CHECK(strstr(g_texts, "|SPEED|") == NULL); /* labels are drawn letter by letter */
    CHECK(strstr(g_texts, "|A|V|G|T|O|P|") != NULL);
    CHECK(strstr(g_texts, "|36|36|") != NULL);   /* average and top speed 36 */

    m.map_min_zoom = 10;
    m.map_max_zoom = 19;
    m.map_zoom = 14;
    map_at_car(&m, 30);
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);           /* a platform without map drawing */
    CHECK(strstr(g_texts, "No map file") != NULL);
    g_texts[0] = '\0';
    render_dashboard(&with_map, &m);
    CHECK(g_bad == 0 && g_map_calls == 1 && g_map_zoom == 14 && g_map_bearing == 30);
    CHECK(g_map_mx == m.map_mx && g_map_my == m.map_my && g_map_cx == CAR_X && g_map_cy == CAR_Y);
    CHECK(strstr(g_texts, "North up") == NULL && strstr(g_texts, "\xC2\xA9 OpenStreetMap")); /* no title */
    CHECK(strstr(g_texts, "|300 m|") != NULL); /* London at zoom 14: about 6 m per pixel */
    CHECK(strstr(g_texts, "|N|") != NULL);     /* the compass needle's N */
    m.map_zoom = 14.5;
    g_texts[0] = '\0';
    render_dashboard(&with_map, &m);
    CHECK(strstr(g_texts, "|200 m|") != NULL); /* half a level closer: 4.4 m per pixel */
    m.map_zoom = 14;
    CHECK(render_map_hit(490, 448) == MAPHIT_ZOOM_IN);  /* bottom centre: minus | plus */
    CHECK(render_map_hit(440, 448) == MAPHIT_ZOOM_OUT);
    CHECK(render_map_hit(400, 260) == MAPHIT_NONE && render_map_hit(520, 448) == MAPHIT_NONE);
    CHECK(render_map_hit(490, 420) == MAPHIT_NONE);

    in.gps_fresh = 0;                       /* GPS lost: the last position stays on the map */
    step(1);
    g_texts[0] = '\0';
    render_dashboard(&with_map, &m);
    CHECK(g_map_calls == 3 && g_circle_fill == 0x4B5763); /* the marker turns grey (day: muted) */

    dash_init(&m);                          /* a map file but no position yet */
    m.map_min_zoom = 10;
    m.map_max_zoom = 19;
    g_texts[0] = '\0';
    g_bad = 0;
    render_dashboard(&with_map, &m);
    CHECK(g_bad == 0 && g_map_calls == 3 && strstr(g_texts, "Waiting for GPS") != NULL);
}

/* The map page: the tap that opens it, following the car turned as the dashboard, a drag (past
 * the jitter allowance, kept inside the map's box, the turn kept), zoom around the middle,
 * following again with "Centre on car" or after 30 s untouched; what it draws and where its
 * buttons are. */
static void test_map_page(void)
{
    Canvas with_map = g_rec;
    MapView v;
    MapBox box;
    double cx, cy, sx, sy, scale = 256.0 * 65536.0;

    with_map.map = rec_map;
    reset();
    in.gps.speed_dkmh = 0;                  /* parked: the marker is a dot, whose centre is recorded */
    step(3);
    m.map_min_zoom = 10;
    m.map_max_zoom = 19;
    m.map_zoom = 16;
    map_at_car(&m, 40);

    /* the tap on the dashboard: a 100 px box around the car, clear of the zoom buttons */
    CHECK(render_car_hit(CAR_X, CAR_Y) && render_car_hit(CAR_X - 50, CAR_Y - 50) && render_car_hit(CAR_X + 49, CAR_Y + 49));
    CHECK(!render_car_hit(CAR_X - 51, CAR_Y) && !render_car_hit(CAR_X, CAR_Y + 50));
    CHECK(!render_car_hit(440, 448) && !render_car_hit(490, 448) && render_map_hit(CAR_X, CAR_Y) == MAPHIT_NONE);

    mapview_open(&v, m.map_zoom, 1000);
    mapview_centre(&v, &m, &cx, &cy);
    CHECK(v.follow && v.zoom == 16 && cx == m.map_mx && cy == m.map_my && mapview_bearing(&v, &m) == 40);
    g_texts[0] = '\0';
    g_bad = g_map_calls = g_glass_calls = 0;
    render_mappage(&with_map, &m, &v);
    CHECK(g_bad == 0 && g_map_calls == 1 && g_map_zoom == 16 && g_map_mx == m.map_mx && g_map_my == m.map_my);
    CHECK(g_map_bearing == 40 && g_map_cx == 400 && g_map_cy == 240);
    CHECK(g_circle_x == 400 && g_circle_y == 240);   /* the car in the middle of the screen */
    CHECK(strncmp(g_texts, "Back|", 5) == 0 && strstr(g_texts, "|N|") && strstr(g_texts, "\xC2\xA9 OpenStreetMap"));
    CHECK(strstr(g_texts, " m|") != NULL);           /* the scale bar */
    CHECK(strstr(g_texts, "Centre on car") == NULL); /* following already */
    CHECK(strstr(g_texts, "|A|V|G|") == NULL && strstr(g_texts, "sats") == NULL); /* no dashboard panels */
    CHECK(g_glass_calls == 5);                       /* Back, compass, scale, credit, zoom */

    /* a tap with a few pixels of jitter leaves the map where it is */
    mapview_press(&v, &m, 300, 200, 2000);
    CHECK(!mapview_move(&v, 309, 191, NULL, 2010) && v.follow && !v.dragging);
    mapview_release(&v, 2020);
    CHECK(v.follow && !v.pressed);
    CHECK(!mapview_move(&v, 500, 500, NULL, 2030)); /* no finger down: nothing */

    /* a drag: the point under the finger stays under it, the view stops following, the turn stays */
    mapview_press(&v, &m, 300, 200, 3000);
    CHECK(mapview_move(&v, 340, 170, NULL, 3050));
    CHECK(!v.follow && v.dragging && v.bearing == 40);
    mapview_to_screen(v.mx, v.my, v.zoom, v.bearing, 400, 240, m.map_mx, m.map_my, &sx, &sy);
    CHECK(NEAR(sx, 440, 0.01) && NEAR(sy, 210, 0.01)); /* the old middle moved with the finger */
    CHECK(!mapview_move(&v, 340, 170, NULL, 3060)); /* not moved: nothing to draw again */
    CHECK(mapview_move(&v, 335, 175, NULL, 3070));
    mapview_to_screen(v.mx, v.my, v.zoom, v.bearing, 400, 240, m.map_mx, m.map_my, &sx, &sy);
    CHECK(NEAR(sx, 435, 0.01) && NEAR(sy, 215, 0.01));
    mapview_release(&v, 3100);
    CHECK(!v.pressed && !v.dragging && !v.follow);
    cx = v.mx;
    cy = v.my;
    in.gps.lat_e6 += 2000;                  /* the car moves on, and turns; the view stays */
    step(1);
    map_at_car(&m, 90);
    CHECK(mapview_bearing(&v, &m) == 40);
    g_texts[0] = '\0';
    g_bad = 0;
    render_mappage(&with_map, &m, &v);
    CHECK(g_bad == 0 && g_map_mx == cx && g_map_my == cy && g_map_bearing == 40);
    mapview_to_screen(cx, cy, 16, 40, 400, 240, m.map_mx, m.map_my, &sx, &sy);
    CHECK(g_circle_x == (int)floor(sx + 0.5) && g_circle_y == (int)floor(sy + 0.5));
    CHECK(strstr(g_texts, "|Centre on car|") != NULL);

    /* the buttons */
    CHECK(render_mappage_hit(30, 30, &v) == MAPPAGE_BACK && render_mappage_hit(0, 0, &v) == MAPPAGE_BACK);
    CHECK(render_mappage_hit(111, 59, &v) == MAPPAGE_BACK && render_mappage_hit(113, 30, &v) == MAPPAGE_NONE);
    CHECK(render_mappage_hit(770, 448, &v) == MAPPAGE_ZOOM_IN && render_mappage_hit(720, 448, &v) == MAPPAGE_ZOOM_OUT);
    CHECK(render_mappage_hit(600, 448, &v) == MAPPAGE_CENTRE && render_mappage_hit(400, 240, &v) == MAPPAGE_NONE);
    CHECK(render_mappage_hit(600, 400, &v) == MAPPAGE_NONE && render_mappage_hit(760, 30, &v) == MAPPAGE_NONE);

    /* zoom keeps the middle of the screen */
    mapview_set_zoom(&v, 15.5, 4000);
    CHECK(v.zoom == 15.5 && v.mx == cx && v.my == cy && !v.follow);
    mapview_set_zoom(&v, 16, 4300);

    /* the map's box stops a drag: north up for easy sums, 100 px of room left and up, 50 down */
    v.bearing = 0;
    box.left = v.mx - 100 / scale;
    box.top = v.my - 100 / scale;
    box.right = v.mx + 100 / scale;
    box.bottom = v.my + 50 / scale;
    mapview_press(&v, &m, 100, 100, 5000);
    CHECK(mapview_move(&v, 600, 20, &box, 5050) && v.mx == box.left && v.my == box.bottom);
    CHECK(!mapview_move(&v, 650, 0, &box, 5060));  /* already at the edge */
    mapview_release(&v, 5100);

    /* following again: 30 s after the last touch, not while a finger is down; or "Centre on car" */
    CHECK(!mapview_idle(&v, 5100 + MAPVIEW_IDLE_MS - 1) && !v.follow);
    mapview_press(&v, &m, 100, 100, 5200);
    CHECK(!mapview_idle(&v, 5200 + MAPVIEW_IDLE_MS + 5000) && !v.follow);
    mapview_release(&v, 5200 + MAPVIEW_IDLE_MS + 5000);
    CHECK(mapview_idle(&v, 5200 + 2 * MAPVIEW_IDLE_MS + 5000) && v.follow);
    CHECK(!mapview_idle(&v, 5200 + 3 * MAPVIEW_IDLE_MS));
    CHECK(render_mappage_hit(600, 448, &v) == MAPPAGE_NONE); /* no "Centre on car" while following */
    mapview_press(&v, &m, 100, 100, 9000);
    mapview_move(&v, 200, 100, NULL, 9010);
    mapview_release(&v, 9020);
    mapview_follow(&v);
    mapview_centre(&v, &m, &cx, &cy);
    CHECK(v.follow && cx == m.map_mx && cy == m.map_my && mapview_bearing(&v, &m) == 90);
    mapview_set_zoom(&v, 15, 9100);          /* following: the car stays in the middle */
    mapview_centre(&v, &m, &cx, &cy);
    CHECK(v.follow && cx == m.map_mx && cy == m.map_my);

    /* night colours, and the page without a map file */
    m.night = 1;
    g_bad = 0;
    render_mappage(&with_map, &m, &v);
    CHECK(g_bad == 0);
    m.map_max_zoom = -1;
    g_texts[0] = '\0';
    g_map_calls = 0;
    render_mappage(&with_map, &m, &v);
    CHECK(g_bad == 0 && g_map_calls == 0 && strstr(g_texts, "Back|") && strstr(g_texts, "No map file"));
    CHECK(strstr(g_texts, "OpenStreetMap") == NULL);
    m.night = 0;
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
        series_push(&m.elevation, i == 10 ? 560.0f : 500.0f, (unsigned)i);
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

/* A short history fills the graph's width: 200 to 230 m in 4 points used to sit in the last few
 * pixels at the right, so the graph looked like a flat line. The note says what the graph covers
 * until it is full: for elevation the road its points span (3 km when full), for the others the
 * time (30 min, 1 h, 3 h when full). */
static void test_graph_fills_width(void)
{
    static const float climb[] = {200.0f, 210.0f, 220.0f, 230.0f};
    int i;

    reset();
    step(700);                               /* GPS elevation */
    series_init(&m.elevation, ELEVATION_POINTS);
    for (i = 0; i < 4; i++)                  /* 4 points, 150 m apart: 450 m of road */
        series_push(&m.elevation, climb[i], m.now_ms - 600000u + (unsigned)i * 200000u);
    g_curve_n = 0;
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(g_curve_n == 5);
    CHECK(g_curve_n == 5 && g_curve[1].x == 20 && g_curve[2].x > 50 && g_curve[3].x > 90 && g_curve[4].x == 132);
    CHECK(g_curve_n == 5 && g_curve[1].y - g_curve[4].y > 30);  /* 30 m of climb is plainly visible */
    CHECK(strstr(g_texts, "|E|L|E|V|A|T|I|O|N|450 m|") != NULL);

    reset();                                 /* 3 minutes in: the battery graph spans 3 min, not 30 */
    for (i = 0; i < 180; i++) {
        in.car.battery_dv = i < 90 ? 140 : 125;
        step(1);
    }
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|B|A|T|T|E|R|Y|3 min|") != NULL);   /* 15-s samples over 2 min 45 s */
    CHECK(strstr(g_texts, "|P|R|E|S|S|U|R|E|3 min|") != NULL); /* 30-s samples over 2 min 30 s */
    CHECK(strstr(g_texts, "|O|U|T|S|I|D|E|2 min|") != NULL);   /* two 90-s samples */
    CHECK(strstr(g_texts, "30 min") == NULL && strstr(g_texts, "|1 h|") == NULL && strstr(g_texts, "|3 h|") == NULL);
}

/* The elevation graph: a point every 150 m driven, on a level road too, nothing while parked
 * (however the GPS height drifts), the last 20 kept: 3 km of road. The first point is taken again
 * when GPS takes over from the car's sensor, so the graph does not start with a false step. */
static void test_elevation_points(void)
{
    float v, prev;
    int i;

    reset();
    in.gps.speed_dkmh = 0;                   /* parked */
    step(12);                                /* the car's 200 m first, then GPS's 279 m */
    CHECK(m.elev_src == SRC_GPS && m.elevation.count == 1);
    CHECK(series_get(&m.elevation, 0, &v, NULL) && NEAR(v, 279.0, 0.01));
    for (i = 0; i < 600; i++) {              /* 10 minutes, the GPS height drifting 15 m: nothing */
        in.gps.altitude_dm = 2790 + (i % 2 ? 150 : -150);
        step(1);
    }
    in.gps.altitude_dm = 2790;
    step(10);
    CHECK(m.elevation.count == 1);
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|E|L|E|V|A|T|I|O|N|") != NULL && strstr(g_texts, "|E|L|E|V|A|T|I|O|N|0 m|") == NULL);

    in.gps.speed_dkmh = 540;                 /* 54 km/h = 15 m/s on the level: a point every 10 s */
    step(105);                               /* 1,575 m */
    CHECK(m.elevation.count == 11);
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|E|L|E|V|A|T|I|O|N|1.5 km|") != NULL);

    for (i = 1; i <= 300; i++) {             /* 4.5 km more, climbing 1 m a second */
        in.gps.altitude_dm = 2790 + i * 10;
        step(1);
    }
    CHECK(m.elevation.count == ELEVATION_POINTS);
    for (i = 1; i < m.elevation.count; i++) { /* 150 m of road and 10 m of climb between points */
        series_get(&m.elevation, i - 1, &prev, NULL);
        series_get(&m.elevation, i, &v, NULL);
        CHECK(NEAR(v - prev, 10.0, 0.5));
    }
    g_texts[0] = '\0';
    render_dashboard(&g_rec, &m);
    CHECK(strstr(g_texts, "|E|L|E|V|A|T|I|O|N|3 km|") != NULL);
}

/* The header's GPS icon blinks while there is no fix (shown at the first update without one, then
 * every other second); the text stays. With a fix it is always shown. */
static void test_gps_blink(void)
{
    int shown[4], i;

    reset();
    in.gps_fresh = 0;
    for (i = 0; i < 4; i++) {
        step(1);
        g_header_left = 0;
        g_texts[0] = '\0';
        render_dashboard(&g_rec, &m);
        shown[i] = g_header_left;
        CHECK(strstr(g_texts, "|Waiting for GPS|") != NULL);
    }
    CHECK(shown[0] > 0 && shown[1] == 0 && shown[2] == shown[0] && shown[3] == 0);
    in.gps_fresh = 1;
    for (i = 0; i < 2; i++) {
        step(1);
        g_header_left = 0;
        render_dashboard(&g_rec, &m);
        CHECK(g_header_left == shown[0]);
    }
    in.gps_fresh = 0;                        /* the signal lost again: shown first */
    step(1);
    g_header_left = 0;
    render_dashboard(&g_rec, &m);
    CHECK(g_header_left == shown[0]);
}

/* The car info page: header, the side bar, each category's tiles, Back; the header button. */
static void test_carinfo(void)
{
    static const char head[] = "Back|Mitsubishi Montero Sport|2016 \xC2\xB7 2.4 diesel 4x2|12:00 PM|";
    static const char side[] = "|General|Plate, fuel, engine, tyres|Size and clearance|Clearances, angles, size|"
                               "Fluids|Types, amounts, intervals|";
    int night;

    reset();
    step(1);
    g_width_percent = 45; /* closer to Barlow than the usual 50 %: the page's longest lines fit then */
    g_texts[0] = '\0';
    g_bad = 0;
    g_watch = "Fluids";
    render_carinfo(&g_rec, &m, CARINFO_GENERAL);
    CHECK(g_bad == 0);
    CHECK(strncmp(g_texts, head, sizeof head - 1) == 0);
    CHECK(strstr(g_texts, side) != NULL);
    CHECK(g_watch_rgb == 0x17202A);                       /* not chosen: the text colour */
    CHECK(strstr(g_texts, "|N|D|E| |4|3|9|1|") != NULL);  /* the plate, letter by letter */
    CHECK(strstr(g_texts, "|2.4|L turbo|4N15 diesel|181 PS \xC2\xB7 430 Nm|") != NULL);
    CHECK(strstr(g_texts, "|4x2|Rear-wheel drive|No low range|") != NULL);
    CHECK(strstr(g_texts, "|265/60 R18|18 in alloy wheels|") != NULL);
    CHECK(strstr(g_texts, "|2,710|kg|Fully loaded maximum|") != NULL);
    CHECK(strstr(g_texts, "|700|mm|") == NULL && strstr(g_texts, "|7.4|L|") == NULL);

    g_texts[0] = '\0';
    render_carinfo(&g_rec, &m, CARINFO_SIZE);
    CHECK(strstr(g_texts, "|700|mm|Maximum, at walking pace|") != NULL);
    CHECK(strstr(g_texts, "|23.1\xC2\xB0|Crest between the axles|") != NULL);
    CHECK(strstr(g_texts, "|4,785|mm|Bumper to bumper|") != NULL);
    CHECK(strstr(g_texts, "|11.2|m|Kerb to kerb|") != NULL);
    CHECK(strstr(g_texts, "|N|D|E|") == NULL);

    for (night = 0; night < 2; night++) {     /* engine oil: double width, every detail */
        m.night = night;
        g_texts[0] = '\0';
        g_bad = 0;
        render_carinfo(&g_rec, &m, CARINFO_FLUIDS);
        CHECK(g_bad == 0);
        CHECK(g_watch_rgb == (night ? 0x10141A : 0xFAFAF6));  /* chosen: drawn on the text colour */
        CHECK(strstr(g_texts, "|7.4|L|Total, with filter|Sump 7.0 + filter 0.3|+ oil cooler 0.1|") != NULL);
        CHECK(strstr(g_texts, "|G|R|A|D|E|5W-30, or 10W-30, 15W-40|") != NULL);
        CHECK(strstr(g_texts, "|A|P|I|CD or better|") != NULL);
        CHECK(strstr(g_texts, "|M|I|N|E|R|A|L|5,000 km or 4 months|") != NULL);
        CHECK(strstr(g_texts, "|F|I|L|T|E|R|Genuine, at every change|") != NULL);
        CHECK(strstr(g_texts, "|12.8|L|Mitsubishi ATF-MA1 only|60,000 km or 48 months|") != NULL);
        CHECK(strstr(g_texts, "|770-810|g|HFC-134a|") != NULL);
    }
    g_watch = NULL;
    m.night = 0;
    g_texts[0] = '\0';
    g_bad = 0;
    render_carinfo(&g_rec, &m, 7);            /* out of range: General */
    CHECK(g_bad == 0 && strstr(g_texts, "|N|D|E|") != NULL);

    CHECK(render_carinfo_category_hit(100, 90) == CARINFO_GENERAL);
    CHECK(render_carinfo_category_hit(30, 170) == CARINFO_SIZE);
    CHECK(render_carinfo_category_hit(190, 250) == CARINFO_FLUIDS);
    CHECK(render_carinfo_category_hit(100, 400) == -1 && render_carinfo_category_hit(300, 90) == -1);
    CHECK(render_carinfo_category_hit(40, 20) == -1);   /* Back is not a category */
    CHECK(render_carinfo_back_hit(40, 20) && !render_carinfo_back_hit(300, 20) && !render_carinfo_back_hit(40, 100));
    CHECK(render_carinfo_button_hit(760, 20) && !render_carinfo_button_hit(600, 20));
    CHECK(!render_carinfo_button_hit(760, 100));
    CHECK(!render_clock_hit(760, 20));       /* the button is not the clock */
    g_width_percent = 50;
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

/* ---------------------------------------------------------------- holidays and the notification */

static void test_holiday_list(void)
{
    static char text[] =
        "# a comment\n"
        "V\t2026-01-01\t2027-12-31\n"
        "P\t0\tCavite\n"
        "P\t1\tQuezon\n"
        "T\t0\t1432944\t12093667\t5356\tDasmari\xC3\xB1" "as\n"
        "T\t0\t1426000\t12094000\t3000\tSilang\n"
        "T\t1\t1411333\t12155694\t6444\tLucban\n"
        "H\t2026-05-15\t2026-05-15\tS\tPahiyas Festival\tT2\n"
        "H\t2026-11-16\t2026-11-18\tS\tSummit\tP1\n"
        "H\t2026-11-26\t2026-11-26\tS\tFounding anniversary\tT0\n"
        "H\t2026-11-30\t2026-11-30\tR\tBonifacio Day\t\n"
        "H\t2026-11-30\t2026-11-30\tS\tProvince day\tP0\n"
        "H\t2026-11-30\t2026-11-30\tW\tTown day\tT0 T1\n"
        "H\t2027-08-19\t2027-08-19\tW\tQuezon Day\n"
        "H\t2027-08-19\t2027-08-19\tS\tQuezon Day\tP1\n"
        "H\t2027-13-01\t2027-13-01\tR\tBad month\n"
        "T\t9\t1\t1\t1\tBad province\n"
        "X\tunknown record\n";
    static HolidayList h;
    HolidayRow r[HOLIDAY_ROWS];
    int n;

    CHECK(holiday_parse(&h, text, (int)strlen(text)) == 8);
    CHECK(h.province_count == 2 && h.town_count == 3 && h.bad_lines == 3);
    CHECK(holiday_covers(&h, 2026, 1, 1) && holiday_covers(&h, 2027, 12, 31) && !holiday_covers(&h, 2028, 1, 1));
    CHECK(holiday_locate(&h, 1432944, 12093667) == 0);       /* at a centre: that town */
    CHECK(holiday_locate(&h, 1427000, 12094000) == 1);       /* nearer Silang's centre */
    CHECK(holiday_locate(&h, 1430500, 12094000) == 0);       /* nearer Dasmariñas, for its size */
    CHECK(holiday_locate(&h, 1600000, 12094000) == -1);      /* far from every town */
    CHECK(holiday_locate(&h, 1432944, 11000000) == -1);      /* outside every town's box */

    n = holiday_today(&h, 2026, 11, 30, 0, r, HOLIDAY_ROWS);   /* national, then province, then town */
    CHECK(n == 3 && strcmp(r[0].name, "Bonifacio Day") == 0 && r[0].local == 0 && r[0].place[0] == 0 &&
          r[0].kind == HOLIDAY_REGULAR);
    CHECK(n == 3 && strcmp(r[1].name, "Province day") == 0 && r[1].local && strcmp(r[1].place, "Cavite") == 0);
    CHECK(n == 3 && strcmp(r[2].name, "Town day") == 0 && strcmp(r[2].place, "Dasmari\xC3\xB1" "as") == 0 &&
          r[2].kind == HOLIDAY_WORKING);
    CHECK(holiday_today(&h, 2026, 11, 30, 0, r, 2) == 2);
    CHECK(holiday_today(&h, 2026, 11, 30, -1, r, HOLIDAY_ROWS) == 1);    /* the town not known: national only */
    CHECK(holiday_today(&h, 2026, 11, 30, 2, r, HOLIDAY_ROWS) == 1);     /* Lucban: no local one */
    CHECK(holiday_today(&h, 2026, 11, 17, 2, r, HOLIDAY_ROWS) == 1 && strcmp(r[0].place, "Quezon") == 0);
    CHECK(holiday_today(&h, 2026, 11, 19, 2, r, HOLIDAY_ROWS) == 0);
    CHECK(holiday_today(&h, 2026, 5, 15, 2, r, HOLIDAY_ROWS) == 1 && strcmp(r[0].name, "Pahiyas Festival") == 0);
    n = holiday_today(&h, 2027, 8, 19, 2, r, HOLIDAY_ROWS);   /* one name at two levels: shown once, local */
    CHECK(n == 1 && r[0].local && r[0].kind == HOLIDAY_SPECIAL);
    n = holiday_today(&h, 2027, 8, 19, 0, r, HOLIDAY_ROWS);   /* elsewhere: the national one */
    CHECK(n == 1 && !r[0].local && r[0].kind == HOLIDAY_WORKING);
    CHECK(HOLIDAY_DAY_OFF(HOLIDAY_REGULAR) && HOLIDAY_DAY_OFF(HOLIDAY_SPECIAL) && !HOLIDAY_DAY_OFF(HOLIDAY_WORKING));
}

/* Every frame from t0 to t1 (5 ms apart) inside the redraw area, without jumps. */
static void island_frames_ok(const Island *s, unsigned t0, unsigned t1)
{
    IslandFrame f, last;
    unsigned t;
    int first = 1, x, y, w, h, bad = 0, jump = 0;
    for (t = t0; t <= t1; t += 5) {
        island_frame(s, t, &f);
        island_bounds(&f, &x, &y, &w, &h);
        if (f.w > 0 && f.h > 0 && (x < ISLAND_AREA_X || y < ISLAND_AREA_Y || x + w > ISLAND_AREA_X + ISLAND_AREA_W ||
                                   y + h > ISLAND_AREA_Y + ISLAND_AREA_H))
            bad++;
        if (!first && (abs(f.x - last.x) > 30 || abs(f.w - last.w) > 40 || abs(f.h - last.h) > 30 ||
                       abs(f.icon_x - last.icon_x) > 12))
            jump++;
        last = f;
        first = 0;
    }
    CHECK(bad == 0);
    CHECK(jump == 0);
}

static void test_island(void)
{
    static Island s;
    IslandFrame f;
    unsigned t = 100000;
    int i, x, y, w, h;

    island_init(&s);
    island_frame(&s, t, &f);
    CHECK(f.w == 0 && f.h == 0 && island_hit(&s, t, 175, 70) == ISLAND_HIT_NONE);
    CHECK(!island_step(&s, t));

    island_set_rows(&s, 2, t);                       /* the button appears */
    CHECK(island_step(&s, t + 10) && s.rest == ISLAND_DOT);
    island_frames_ok(&s, t, t + 400);
    CHECK(!island_step(&s, t + 400));
    island_frame(&s, t + 400, &f);
    CHECK(f.x == ISLAND_DOT_X && f.y == ISLAND_DOT_Y && f.w == ISLAND_DOT_D && f.h == ISLAND_DOT_D && f.radius == 22);
    CHECK(f.icon && !f.panel && f.alpha[0] == 0);
    CHECK(island_hit(&s, t + 400, 175, 70) == ISLAND_HIT_DOT && island_hit(&s, t + 400, 400, 300) == ISLAND_HIT_NONE);

    t += 1000;                                       /* tapped: it opens */
    island_open(&s, t);
    island_frames_ok(&s, t, t + 700);
    island_frame(&s, t + 150, &f);
    CHECK(f.alpha[0] == 0);                          /* no text while it grows */
    island_frame(&s, t + 230, &f);
    CHECK(f.w > ISLAND_PANEL_W);                     /* the bounce: a little wider than the panel */
    CHECK(island_step(&s, t + 400) && !island_step(&s, t + 500));
    island_frame(&s, t + 500, &f);
    CHECK(f.x == ISLAND_PANEL_X && f.y == ISLAND_PANEL_Y && f.w == ISLAND_PANEL_W &&
          f.h == 2 * ISLAND_PAD + 2 * ISLAND_ROW_H && f.panel && f.rows == 2);
    CHECK(f.alpha[0] == 256 && f.alpha[1] == 256 && f.alpha[2] == 0);
    CHECK(f.icon_x == ISLAND_PANEL_X + 16);
    CHECK(island_hit(&s, t + 500, 400, 100) == ISLAND_HIT_PANEL);

    island_set_rows(&s, 3, t + 1000);               /* a local holiday found: it grows a row */
    island_frames_ok(&s, t + 1000, t + 1500);
    island_frame(&s, t + 1100, &f);
    CHECK(f.alpha[0] == 256 && f.alpha[2] == 0);     /* the rows shown stay, the new one fades in */
    CHECK(!island_step(&s, t + 1400));
    island_frame(&s, t + 1400, &f);
    CHECK(f.h == 2 * ISLAND_PAD + 3 * ISLAND_ROW_H && f.alpha[2] == 256);

    CHECK(!island_step(&s, t + 1360 + ISLAND_HOLD_MS - 10) && s.rest == ISLAND_OPEN);
    CHECK(island_step(&s, t + 1360 + ISLAND_HOLD_MS) && s.rest == ISLAND_DOT); /* closes by itself */
    t += 1360 + ISLAND_HOLD_MS;
    island_frames_ok(&s, t, t + 600);
    island_frame(&s, t + 60, &f);
    CHECK(f.alpha[0] > 0 && f.alpha[0] < 256);       /* the text fades out first */
    CHECK(!island_step(&s, t + 450));
    island_frame(&s, t + 450, &f);
    CHECK(f.w == ISLAND_DOT_D && f.x == ISLAND_DOT_X && f.alpha[0] == 0);

    t += 1000;                                       /* opened, then tapped again while it grows */
    island_open(&s, t);
    island_frames_ok(&s, t, t + 200);
    {
        IslandFrame before, after;
        island_frame(&s, t + 200, &before);
        island_close(&s, t + 200);                   /* it turns back from where it was */
        island_frame(&s, t + 200, &after);
        CHECK(before.x == after.x && before.w == after.w && before.h == after.h && before.icon_x == after.icon_x);
    }
    island_frames_ok(&s, t + 200, t + 900);
    CHECK(!island_step(&s, t + 700) && s.rest == ISLAND_DOT);

    t += 1000;                                       /* open, then the page is left */
    island_open(&s, t);
    island_snap(&s);
    CHECK(!island_step(&s, t + 10) && s.rest == ISLAND_DOT);

    island_set_rows(&s, 0, t + 100);                 /* no holiday any more: the button goes */
    island_frames_ok(&s, t + 100, t + 500);
    CHECK(!island_step(&s, t + 400) && s.rest == ISLAND_HIDDEN);
    island_frame(&s, t + 400, &f);
    CHECK(f.w == 0 && f.h == 0);

    island_open(&s, t + 1000);                       /* nothing to show: stays hidden */
    CHECK(s.rest == ISLAND_HIDDEN);
    island_set_rows(&s, 1, t + 1000);                /* straight from nothing into the panel */
    island_open(&s, t + 1000);
    island_frames_ok(&s, t + 1000, t + 1600);
    CHECK(!island_step(&s, t + 1500));
    island_frame(&s, t + 1500, &f);
    CHECK(f.h == 2 * ISLAND_PAD + ISLAND_ROW_H && f.alpha[0] == 256 && f.alpha[1] == 0);

    for (i = 1; i <= HOLIDAY_ROWS; i++) {            /* the largest panel, at its bounce, fits the area */
        static Island big;
        island_init(&big);
        island_set_rows(&big, i, 0);
        island_snap(&big);
        island_open(&big, 10);
        island_frames_ok(&big, 10, 700);
        island_frame(&big, 10 + 260, &f);
        island_bounds(&f, &x, &y, &w, &h);
        CHECK(y + h <= ISLAND_AREA_Y + ISLAND_AREA_H && x + w <= 603); /* clear of the north mark */
    }
}

static void test_render_island(void)
{
    static Island s;
    IslandFrame f;

    reset();
    step(5);
    m.year = 2026; m.month = 11; m.day = 30;
    m.holiday_count = 2;
    strcpy(m.holidays[0].name, "Bonifacio Day");
    m.holidays[0].place[0] = 0;
    m.holidays[0].kind = HOLIDAY_REGULAR;
    m.holidays[0].local = 0;
    strcpy(m.holidays[1].name, "Town day");
    strcpy(m.holidays[1].place, "Lucban");
    m.holidays[1].kind = HOLIDAY_WORKING;
    m.holidays[1].local = 1;
    island_init(&s);
    island_set_rows(&s, 2, 0);
    island_open(&s, 0);
    island_frame(&s, 2000, &f);
    g_texts[0] = '\0';
    g_calls = g_bad = 0;
    render_island(&g_rec, &m, &f);
    CHECK(g_bad == 0);
    CHECK(strstr(g_texts, "Bonifacio Day|") && strstr(g_texts, "|Monday, November 30, 2026|"));
    CHECK(strstr(g_texts, "|Regular holiday|") && strstr(g_texts, "|Working holiday in Lucban|"));
    CHECK(strstr(g_texts, "D|A|Y| |O|F|F|") != NULL);              /* the badge, once: only the day off */
    CHECK(strstr(strstr(g_texts, "D|A|Y| |O|F|F|") + 1, "D|A|Y| |O|F|F|") == NULL);

    island_frame(&s, 100, &f);                       /* growing: the shape and icon, no text yet */
    g_texts[0] = '\0';
    render_island(&g_rec, &m, &f);
    CHECK(g_texts[0] == 0);
    m.holidays[1].kind = HOLIDAY_SPECIAL;
    island_frame(&s, 2000, &f);
    g_texts[0] = '\0';
    render_island(&g_rec, &m, &f);
    CHECK(strstr(g_texts, "|Non-working day in Lucban|") != NULL);
}

int main(void)
{
    test_holiday_list();
    test_island();
    test_render_island();
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
    test_graph_fills_width();
    test_elevation_points();
    test_gps_blink();
    test_carinfo();
    test_render();
    test_speed_and_map();
    test_map_page();
    test_address_and_graphs();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dashboard self-test passed\n");
    return 0;
}
