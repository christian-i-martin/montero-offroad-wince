#include "render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32_WCE
#define snprintf _snprintf
#endif

#define DEG "\xC2\xB0" /* degree sign in UTF-8 */

const FontSpec render_fonts[FONT_COUNT] = {
    {18, FACE_SEMIBOLD},  /* FONT_CLOCK */
    {15, FACE_MEDIUM},    /* FONT_HEADER */
    {13, FACE_SEMIBOLD},  /* FONT_LABEL */
    {12, FACE_MEDIUM},    /* FONT_SMALL */
    {13, FACE_MEDIUM},    /* FONT_TREND */
    {15, FACE_MEDIUM},    /* FONT_UNIT */
    {19, FACE_MEDIUM},    /* FONT_UNIT_BIG */
    {34, FACE_CONDENSED}, /* FONT_VALUE */
    {84, FACE_CONDENSED}, /* FONT_SPEED */
    {64, FACE_CONDENSED}, /* FONT_STAT */
    {46, FACE_CONDENSED}, /* FONT_HEADING */
    {26, FACE_SEMIBOLD},  /* FONT_DIR */
    {15, FACE_SEMIBOLD},  /* FONT_DIR_SMALL */
    {21, FACE_CONDENSED}, /* FONT_ROSE */
    {19, FACE_SEMIBOLD},  /* FONT_MENU */
};

typedef struct { unsigned bg, panel, line, text, muted, accent, cool, warn, area, button; } Theme;
static const Theme g_day = {0x0B0E11, 0x151A1F, 0x262D35, 0xF1F4F6, 0xA3ADB7,
                            0xF6A623, 0x5BC0EB, 0xFF7849, 0x2B2619, 0x1C2228};
static const Theme g_night = {0x000000, 0x0A0C0E, 0x1A1F24, 0xC8CED4, 0x7E8892,
                              0xD68A12, 0x3E9CC4, 0xE0603A, 0x221B0E, 0x101418};

/* Three columns and three rows below the header, 12 px apart (design canvas). */
static const int COL_X[3] = {12, 274, 537}, COL_W[3] = {250, 251, 251};
static const int ROW_Y[3] = {52, 195, 337}, ROW_H[3] = {131, 130, 131};

enum { ICON_GPS, ICON_SUN, ICON_LIGHTS, ICON_UP, ICON_DOWN, ICON_FLAT, ICON_BOLT, ICON_BATTERY, ICON_WARN };

typedef struct { int x, y, w, h; } Box;

static int iround(double v) { return (int)floor(v + 0.5); }

static void text(const Canvas *c, int x, int baseline, int font, unsigned rgb, const char *s)
{
    if (s && *s)
        c->text(c->ctx, x, baseline, font, rgb, s);
}

static int tw(const Canvas *c, int font, const char *s) { return s && *s ? c->text_width(c->ctx, font, s) : 0; }

static void text_right(const Canvas *c, int right, int baseline, int font, unsigned rgb, const char *s)
{
    text(c, right - tw(c, font, s), baseline, font, rgb, s);
}

static void line(const Canvas *c, int x1, int y1, int x2, int y2, int width, unsigned rgb)
{
    DPoint p[2];
    p[0].x = x1; p[0].y = y1; p[1].x = x2; p[1].y = y2;
    c->polyline(c->ctx, p, 2, width, rgb);
}

/* Upper-case label with 1 px between letters, like the design's letter spacing. Returns its width. */
static int label(const Canvas *c, int x, int baseline, unsigned rgb, const char *s)
{
    char ch[2] = {0, 0};
    int start = x;
    for (; *s; s++) {
        ch[0] = *s;
        text(c, x, baseline, FONT_LABEL, rgb, ch);
        x += tw(c, FONT_LABEL, ch) + 1;
    }
    return x - start;
}

static void fmt_commas(char *out, int size, int v)
{
    char digits[16];
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    int n = snprintf(digits, sizeof digits, "%u", u), i, j = 0;
    if (v < 0)
        out[j++] = '-';
    for (i = 0; i < n && j < size - 2; i++) {
        if (i && (n - i) % 3 == 0)
            out[j++] = ',';
        out[j++] = digits[i];
    }
    out[j] = '\0';
}

/* ---------------------------------------------------------------- icons (24-unit box) */

static DPoint ip(int x, int y, int s, double ux, double uy)
{
    DPoint p;
    p.x = x + iround(ux * s / 24.0);
    p.y = y + iround(uy * s / 24.0);
    return p;
}

static void iline(const Canvas *c, int x, int y, int s, int w, unsigned rgb, double x1, double y1, double x2, double y2)
{
    DPoint p[2];
    p[0] = ip(x, y, s, x1, y1);
    p[1] = ip(x, y, s, x2, y2);
    c->polyline(c->ctx, p, 2, w, rgb);
}

static void ipath(const Canvas *c, int x, int y, int s, int w, unsigned rgb, const double *xy, int n, int filled)
{
    DPoint p[16];
    int i;
    for (i = 0; i < n && i < 16; i++)
        p[i] = ip(x, y, s, xy[2 * i], xy[2 * i + 1]);
    if (filled)
        c->polygon(c->ctx, p, n, rgb);
    else
        c->polyline(c->ctx, p, n, w, rgb);
}

static void icon(const Canvas *c, int kind, int x, int y, int s, unsigned rgb)
{
    static const double lights[] = {10, 6, 6.5, 6.8, 4.5, 9, 4, 12, 4.5, 15, 6.5, 17.2, 10, 18, 11.3, 16.5, 12, 12, 11.3, 7.5, 10, 6};
    static const double up[] = {6, 11, 12, 5, 18, 11}, down[] = {6, 13, 12, 19, 18, 13};
    static const double bolt[] = {13, 3, 6, 13, 11, 13, 10, 21, 17, 11, 12, 11};
    static const double battery[] = {3, 7, 19, 7, 19, 17, 3, 17, 3, 7};
    static const double warn[] = {12, 3, 22, 20, 2, 20, 12, 3};
    double arc[20];
    DPoint centre;
    int w = 2, i;

    switch (kind) {
    case ICON_GPS:
        centre = ip(x, y, s, 12, 12);
        c->circle(c->ctx, centre.x, centre.y, iround(6.0 * s / 24.0), NO_FILL, rgb, w);
        iline(c, x, y, s, w, rgb, 12, 2, 12, 6);
        iline(c, x, y, s, w, rgb, 12, 18, 12, 22);
        iline(c, x, y, s, w, rgb, 2, 12, 6, 12);
        iline(c, x, y, s, w, rgb, 18, 12, 22, 12);
        break;
    case ICON_SUN:
        iline(c, x, y, s, w, rgb, 4, 18, 20, 18);
        for (i = 0; i < 10; i++) {
            double a = 3.14159265358979 * (1.0 + i / 9.0);
            arc[2 * i] = 12 + 5 * cos(a);
            arc[2 * i + 1] = 18 + 5 * sin(a);
        }
        ipath(c, x, y, s, w, rgb, arc, 10, 0);
        iline(c, x, y, s, w, rgb, 12, 6, 12, 9);
        iline(c, x, y, s, w, rgb, 5.6, 9.6, 7.6, 11.6);
        iline(c, x, y, s, w, rgb, 18.4, 9.6, 16.4, 11.6);
        break;
    case ICON_LIGHTS:
        ipath(c, x, y, s, w, rgb, lights, 11, 0);
        iline(c, x, y, s, w, rgb, 15, 8, 20, 8);
        iline(c, x, y, s, w, rgb, 15, 12, 20, 12);
        iline(c, x, y, s, w, rgb, 15, 16, 20, 16);
        break;
    case ICON_UP:
        iline(c, x, y, s, w, rgb, 12, 19, 12, 5);
        ipath(c, x, y, s, w, rgb, up, 3, 0);
        break;
    case ICON_DOWN:
        iline(c, x, y, s, w, rgb, 12, 5, 12, 19);
        ipath(c, x, y, s, w, rgb, down, 3, 0);
        break;
    case ICON_FLAT:
        iline(c, x, y, s, w, rgb, 5, 12, 19, 12);
        break;
    case ICON_BOLT:
        ipath(c, x, y, s, w, rgb, bolt, 6, 1);
        break;
    case ICON_BATTERY:
        ipath(c, x, y, s, w, rgb, battery, 5, 0);
        iline(c, x, y, s, w, rgb, 21, 11, 21, 13);
        break;
    case ICON_WARN:
        ipath(c, x, y, s, w, rgb, warn, 4, 0);
        iline(c, x, y, s, w, rgb, 12, 9, 12, 14);
        iline(c, x, y, s, w, rgb, 12, 17, 12, 17.6);
        break;
    }
}

/* ---------------------------------------------------------------- building blocks */

static void tile(const Canvas *c, const Theme *t, int x, int y, int w, int h)
{
    c->round_rect(c->ctx, x, y, w, h, 10, t->panel, t->line);
}

/* Tile name on the left, a note on the right; the short note is used when the long one would
 * touch the name (wider fallback fonts). */
static void tile_header2(const Canvas *c, const Theme *t, int x, int y, int w, const char *name, const char *note,
                         const char *short_note, unsigned note_rgb)
{
    int end = x + 17 + label(c, x + 17, y + 24, t->muted, name) + 10;
    if (short_note && x + w - 17 - tw(c, FONT_SMALL, note) < end)
        note = short_note;
    if (x + w - 17 - tw(c, FONT_SMALL, note) >= end)
        text_right(c, x + w - 17, y + 24, FONT_SMALL, note_rgb, note);
}

static void tile_header(const Canvas *c, const Theme *t, int x, int y, int w, const char *name, const char *note,
                        unsigned note_rgb)
{
    tile_header2(c, t, x, y, w, name, note, NULL, note_rgb);
}

static int value_unit(const Canvas *c, const Theme *t, int x, int baseline, int vfont, const char *value, int ufont,
                      const char *unit, int gap)
{
    int w = tw(c, vfont, value);
    text(c, x, baseline, vfont, t->text, value);
    if (unit && *unit) {
        text(c, x + w + gap, baseline, ufont, t->muted, unit);
        w += gap + tw(c, ufont, unit);
    }
    return w;
}

/* Right-aligned trend text with an optional 14 px icon in front of it; left out when it would
 * reach min_x (the end of the value next to it). */
static void trend_right(const Canvas *c, int right, int baseline, int min_x, unsigned rgb, int icon_kind,
                        unsigned icon_rgb, const char *s)
{
    int x = right - tw(c, FONT_TREND, s);
    if ((icon_kind >= 0 ? x - 18 : x) < min_x)
        return;
    text(c, x, baseline, FONT_TREND, rgb, s);
    if (icon_kind >= 0)
        icon(c, icon_kind, x - 18, baseline - 12, 14, icon_rgb);
}

/* Line graph of a series. Time series are placed by age over window_ms (newest at the right
 * edge); by_index series are evenly spaced per sample. The value range is at least min_span. */
static void chart(const Canvas *c, const Theme *t, const Series *s, Box b, unsigned now, unsigned window_ms,
                  float min_span, unsigned rgb, int area, int by_index)
{
    DPoint p[SERIES_MAX + 2];
    float v, lo = 0, hi = 0;
    unsigned tick;
    int i, n = 0, used = 0, bottom = b.y + b.h - 1;

    for (i = 0; i < s->count; i++) {
        series_get(s, i, &v, &tick);
        if (!by_index && now - tick > window_ms)
            continue;
        if (used == 0 || v < lo) lo = v;
        if (used == 0 || v > hi) hi = v;
        used++;
    }
    c->fill_rect(c->ctx, b.x, bottom, b.w, 1, t->line);
    if (used < 2) {
        const char *msg = "Collecting data";
        text(c, b.x + (b.w - tw(c, FONT_SMALL, msg)) / 2, b.y + b.h / 2 + 4, FONT_SMALL, t->muted, msg);
        return;
    }
    if (hi - lo < min_span) {
        float mid = (hi + lo) / 2.0f;
        lo = mid - min_span / 2.0f;
        hi = mid + min_span / 2.0f;
    }
    n = 1; /* p[0] is the area's first corner */
    for (i = 0; i < s->count; i++) {
        series_get(s, i, &v, &tick);
        if (!by_index && now - tick > window_ms)
            continue;
        p[n].x = by_index ? b.x + b.w - 1 - iround((double)(s->count - 1 - i) * (b.w - 1) / (s->cap - 1))
                          : b.x + b.w - 1 - iround((double)(now - tick) * (b.w - 1) / window_ms);
        p[n].y = b.y + 3 + iround((hi - v) / (hi - lo) * (b.h - 7));
        n++;
    }
    if (area) {
        p[0].x = p[1].x;
        p[0].y = bottom;
        p[n].x = p[n - 1].x;
        p[n].y = bottom;
        c->polygon(c->ctx, p, n + 1, t->area);
    }
    c->polyline(c->ctx, p + 1, n - 1, 2, rgb);
}

/* ---------------------------------------------------------------- header */

static void draw_header(const Canvas *c, const Theme *t, const DashModel *m)
{
    char buf[64];
    int x;

    c->fill_rect(c->ctx, 0, HEADER_H - 1, SCREEN_W, 1, t->line);
    if (m->clock_valid)
        snprintf(buf, sizeof buf, "%02d:%02d", m->hour, m->minute);
    else
        strcpy(buf, "--:--");
    text(c, 16, 26, FONT_CLOCK, t->text, buf);
    x = 16 + tw(c, FONT_CLOCK, buf) + 20;

    if (m->gps_ok) {
        snprintf(buf, sizeof buf, "GPS %d sats", m->satellites);
        icon(c, ICON_GPS, x, 11, 18, t->muted);
        text(c, x + 24, 25, FONT_HEADER, t->muted, buf);
    } else {
        strcpy(buf, "No GPS fix");
        icon(c, ICON_GPS, x, 11, 18, t->warn);
        text(c, x + 24, 25, FONT_HEADER, t->text, buf);
    }
    x += 24 + tw(c, FONT_HEADER, buf) + 20;
    if (m->night) {
        icon(c, ICON_LIGHTS, x, 11, 18, t->accent);
        text(c, x + 24, 25, FONT_HEADER, t->text, "Headlights on");
        x += 24 + tw(c, FONT_HEADER, "Headlights on") + 20;
    }
    if (!m->car_ok) {
        icon(c, ICON_WARN, x, 11, 18, t->warn);
        text(c, x + 24, 25, FONT_HEADER, t->text, "No car data");
    }

    buf[0] = '\0';
    if (!m->have_position)
        strcpy(buf, "Sunset: waiting for GPS");
    else if (m->sun_event == SUN_SUNSET && m->sun_minutes_left >= 60)
        snprintf(buf, sizeof buf, "Sunset %02d:%02d, %d h %d min left", m->sun_hour, m->sun_minute,
                 m->sun_minutes_left / 60, m->sun_minutes_left % 60);
    else if (m->sun_event == SUN_SUNSET)
        snprintf(buf, sizeof buf, "Sunset %02d:%02d, %d min left", m->sun_hour, m->sun_minute, m->sun_minutes_left);
    else if (m->sun_event == SUN_SUNRISE)
        snprintf(buf, sizeof buf, "Sunrise %02d:%02d", m->sun_hour, m->sun_minute);
    if (buf[0]) {
        int w = tw(c, FONT_HEADER, buf);
        text(c, SCREEN_W - 16 - w, 25, FONT_HEADER, t->muted, buf);
        icon(c, ICON_SUN, SCREEN_W - 16 - w - 24, 11, 18, t->muted);
    }
}

/* ---------------------------------------------------------------- tiles */

static void draw_pressure(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char buf[16];
    int right = x + w - 17, end;
    Box b;
    tile(c, t, x, y, w, h);
    tile_header(c, t, x, y, w, "PRESSURE", "Last 1 h", t->muted);
    if (m->have_pressure)
        fmt_commas(buf, sizeof buf, m->pressure_hpa);
    else
        strcpy(buf, "--");
    end = x + 17 + value_unit(c, t, x + 17, y + 56, FONT_VALUE, buf, FONT_UNIT, "hPa", 5) + 10;
    switch (m->weather) {
    case WEATHER_FALLING_FAST: trend_right(c, right, y + 56, end, t->text, ICON_DOWN, t->warn, "Falling fast"); break;
    case WEATHER_FALLING: trend_right(c, right, y + 56, end, t->text, ICON_DOWN, t->muted, "Falling"); break;
    case WEATHER_RISING: trend_right(c, right, y + 56, end, t->text, ICON_UP, t->muted, "Rising"); break;
    case WEATHER_STEADY: trend_right(c, right, y + 56, end, t->text, ICON_FLAT, t->muted, "Weather steady"); break;
    default:
        if (m->have_pressure)
            trend_right(c, right, y + 56, end, t->muted, -1, 0, "No trend yet");
    }
    b.x = x + 17; b.w = w - 34; b.h = 44; b.y = y + h - 11 - b.h;
    chart(c, t, &m->pressure, b, m->now_ms, 3600000u, 4.0f, t->cool, 0, 0);
}

static void draw_elevation(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char buf[32];
    int right = x + w - 17, end;
    Box b;
    tile(c, t, x, y, w, h);
    if (m->elev_src == SRC_GPS)
        tile_header2(c, t, x, y, w, "ELEVATION", "GPS, last 20 km", "GPS", t->muted);
    else if (m->elev_src == SRC_CAR)
        tile_header2(c, t, x, y, w, "ELEVATION", "Car sensor, last 20 km", "Car sensor", t->text);
    else
        tile_header(c, t, x, y, w, "ELEVATION", "Last 20 km", t->muted);
    if (m->elev_src != SRC_NONE)
        fmt_commas(buf, sizeof buf, iround(m->elev_m));
    else
        strcpy(buf, "--");
    end = x + 17 + value_unit(c, t, x + 17, y + 56, FONT_VALUE, buf, FONT_UNIT, "m", 5) + 10;
    if (m->have_climb) {
        int rate = iround(m->climb_m_min);
        if (rate > -3 && rate < 3) {
            trend_right(c, right, y + 56, end, t->text, ICON_FLAT, t->muted, "Steady");
        } else {
            snprintf(buf, sizeof buf, "%s %d m/min", rate > 0 ? "Up" : "Down", rate > 0 ? rate : -rate);
            trend_right(c, right, y + 56, end, t->text, rate > 0 ? ICON_UP : ICON_DOWN, rate > 0 ? t->accent : t->cool, buf);
        }
    }
    b.x = x + 17; b.w = w - 34; b.h = 44; b.y = y + h - 11 - b.h;
    chart(c, t, &m->elevation, b, m->now_ms, 0, 40.0f, t->accent, 1, 1);
}

static void draw_outside(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char buf[40];
    int end;
    Box b;
    tile(c, t, x, y, w, h);
    tile_header(c, t, x, y, w, "OUTSIDE", "Last 3 h", t->muted);
    if (m->have_outside)
        snprintf(buf, sizeof buf, "%d", m->outside_c);
    else
        strcpy(buf, "--");
    end = x + 17 + value_unit(c, t, x + 17, y + 56, FONT_VALUE, buf, FONT_UNIT, DEG "C", 5) + 10;
    if (m->have_temp_change) {
        int d = iround(m->temp_change), minutes = (int)(m->temp_span_s / 60u);
        char span[16];
        if (minutes < 60)
            snprintf(span, sizeof span, "%d min", minutes);
        else
            snprintf(span, sizeof span, "%d h", (minutes + 30) / 60);
        if (d == 0)
            snprintf(buf, sizeof buf, "Steady");
        else
            snprintf(buf, sizeof buf, "%s %d" DEG " in %s", d > 0 ? "Up" : "Down", d > 0 ? d : -d, span);
        trend_right(c, x + w - 17, y + 56, end, t->text, -1, 0, buf);
    }
    b.x = x + 17; b.w = w - 34; b.h = 44; b.y = y + h - 11 - b.h;
    chart(c, t, &m->outside, b, m->now_ms, 10800000u, 4.0f, t->cool, 0, 0);
}

static void rose_point(int cx, int cy, double r, double screen_deg, int *x, int *y)
{
    double a = screen_deg * 3.14159265358979 / 180.0;
    *x = cx + iround(r * sin(a));
    *y = cy - iround(r * cos(a));
}

static void draw_rose(const Canvas *c, const Theme *t, int cx, int cy, float heading)
{
    static const char *const letters[4] = {"N", "E", "S", "W"};
    DPoint p[4];
    int i, x1, y1, x2, y2;

    c->circle(c->ctx, cx, cy, 80, t->bg, t->line, 2);
    for (i = 0; i < 36; i++) {
        double a = i * 10.0 - heading;
        rose_point(cx, cy, 78, a, &x1, &y1);
        rose_point(cx, cy, i % 3 == 0 ? 67 : 73, a, &x2, &y2);
        line(c, x1, y1, x2, y2, 2, t->muted);
    }
    for (i = 0; i < 4; i++) {
        rose_point(cx, cy, 48, i * 90.0 - heading, &x1, &y1);
        text(c, x1 - tw(c, FONT_ROSE, letters[i]) / 2, y1 + 8, FONT_ROSE, i == 0 ? t->accent : t->text, letters[i]);
    }
    /* the car, pointing up */
    p[0].x = cx; p[0].y = cy - 17;
    p[1].x = cx + 11; p[1].y = cy + 14;
    p[2].x = cx; p[2].y = cy + 7;
    p[3].x = cx - 11; p[3].y = cy + 14;
    c->polygon(c->ctx, p, 4, t->text);
    /* fixed mark at the top */
    p[0].x = cx - 8; p[0].y = cy - 84;
    p[1].x = cx + 8; p[1].y = cy - 84;
    p[2].x = cx; p[2].y = cy - 70;
    c->polygon(c->ctx, p, 3, t->accent);
}

static void draw_heading(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char big[16];
    const char *dir = "";
    int dir_font = FONT_DIR, total, start, cx = x + w / 2;
    unsigned dir_rgb = t->accent;

    tile(c, t, x, y, w, h);
    tile_header(c, t, x, y, w, "HEADING", m->heading_src == SRC_GPS ? "GPS" : m->heading_src == SRC_CAR ? "Car compass" : "",
                t->muted);
    draw_rose(c, t, cx, y + 115, m->heading_src == SRC_NONE ? 0.0f : m->heading_deg);
    if (m->heading_src == SRC_GPS) {
        snprintf(big, sizeof big, "%d" DEG, iround(m->heading_deg) % 360);
        dir = dash_point_name(m->heading_deg);
    } else if (m->heading_src == SRC_CAR) {
        snprintf(big, sizeof big, "%s", dash_point_name(m->heading_deg));
        dir = "8-point";
        dir_font = FONT_DIR_SMALL;
        dir_rgb = t->muted;
    } else {
        strcpy(big, "--");
    }
    total = tw(c, FONT_HEADING, big) + (*dir ? 10 + tw(c, dir_font, dir) : 0);
    start = cx - total / 2;
    text(c, start, y + h - 17, FONT_HEADING, t->text, big);
    text(c, start + tw(c, FONT_HEADING, big) + 10, y + h - 17, dir_font, dir_rgb, dir);
}

static void draw_battery(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char buf[16];
    const char *status = NULL;
    int kind = -1, right = x + w - 17;
    unsigned icon_rgb = t->muted, text_rgb = t->text;
    Box b;

    tile(c, t, x, y, w, h);
    tile_header(c, t, x, y, w, "BATTERY", NULL, t->muted);
    switch (m->battery_state) {
    case BATT_CHARGING: status = "Charging"; kind = ICON_BOLT; icon_rgb = t->cool; break;
    case BATT_NOT_CHARGING: status = "Not charging"; kind = ICON_BATTERY; text_rgb = t->muted; break;
    case BATT_LOW: status = "Low"; kind = ICON_WARN; icon_rgb = t->warn; break;
    case BATT_HIGH: status = "Too high"; kind = ICON_WARN; icon_rgb = t->warn; break;
    }
    if (status) {
        int sx = right - tw(c, FONT_LABEL, status);
        text(c, sx, y + 24, FONT_LABEL, text_rgb, status);
        icon(c, kind, sx - 18, y + 12, 14, icon_rgb);
    }
    if (m->have_battery) {
        int dv = iround(m->battery_v * 10.0);
        snprintf(buf, sizeof buf, "%d.%d", dv / 10, dv % 10);
    } else
        strcpy(buf, "--");
    value_unit(c, t, x + 17, y + 56, FONT_VALUE, buf, FONT_UNIT, "V", 5);
    text_right(c, right, y + 56, FONT_SMALL, t->muted, "Last 30 min");
    b.x = x + 17; b.w = w - 34; b.h = 40; b.y = y + h - 11 - b.h;
    chart(c, t, &m->battery, b, m->now_ms, 1800000u, 0.8f, t->cool, 0, 0);
}

static void draw_speed_tile(const Canvas *c, const Theme *t, int x, int y, int w, int h, const char *name,
                            const char *note, unsigned note_rgb, int font, int baseline, int have, float kmh,
                            const char *footer)
{
    char buf[16];
    tile(c, t, x, y, w, h);
    tile_header(c, t, x, y, w, name, note, note_rgb);
    if (have)
        snprintf(buf, sizeof buf, "%d", iround(kmh));
    else
        strcpy(buf, "--");
    value_unit(c, t, x + 17, y + baseline, font, buf, FONT_UNIT_BIG, "km/h", 8);
    if (footer)
        text(c, x + 17, y + h - 13, FONT_SMALL, t->muted, footer);
}

void render_dashboard(const Canvas *c, const DashModel *m)
{
    const Theme *t = m->night ? &g_night : &g_day;
    char since[24] = "";

    if (m->have_since)
        snprintf(since, sizeof since, "Since %02d:%02d", m->since_hour, m->since_minute);
    c->fill_rect(c->ctx, 0, 0, SCREEN_W, SCREEN_H, t->bg);
    draw_header(c, t, m);
    draw_pressure(c, t, m, COL_X[0], ROW_Y[0], COL_W[0], ROW_H[0]);
    draw_elevation(c, t, m, COL_X[0], ROW_Y[1], COL_W[0], ROW_H[1]);
    draw_outside(c, t, m, COL_X[0], ROW_Y[2], COL_W[0], ROW_H[2]);
    draw_heading(c, t, m, COL_X[1], ROW_Y[0], COL_W[1], ROW_Y[1] + ROW_H[1] - ROW_Y[0]);
    draw_battery(c, t, m, COL_X[1], ROW_Y[2], COL_W[1], ROW_H[2]);
    draw_speed_tile(c, t, COL_X[2], ROW_Y[0], COL_W[2], ROW_H[0], "SPEED", m->gps_ok ? "GPS" : "No GPS signal",
                    m->gps_ok ? t->muted : t->text, FONT_SPEED, 103, m->have_speed, m->speed_kmh, NULL);
    draw_speed_tile(c, t, COL_X[2], ROW_Y[1], COL_W[2], ROW_H[1], "AVERAGE SPEED", "While moving", t->muted,
                    FONT_STAT, 92, m->have_avg, m->avg_kmh, since[0] ? since : NULL);
    draw_speed_tile(c, t, COL_X[2], ROW_Y[2], COL_W[2], ROW_H[2], "TOP SPEED", since, t->muted, FONT_STAT, 97,
                    m->have_top, m->top_kmh, NULL);
}

/* ---------------------------------------------------------------- maintenance menu */

#define MENU_X 190
#define MENU_Y 84
#define MENU_W 420
#define MENU_H 312

static void menu_button(int index, Box *b)
{
    b->x = MENU_X + 20;
    b->y = MENU_Y + 92 + index * 68;
    b->w = MENU_W - 40;
    b->h = 56;
}

static int menu_items(int has_file_manager, int *items)
{
    int n = 0;
    if (has_file_manager)
        items[n++] = MENU_FILES;
    items[n++] = MENU_CLOSE;
    items[n++] = MENU_BACK;
    return n;
}

void render_menu(const Canvas *c, const DashModel *m, int has_file_manager, const char *version)
{
    static const char *const names[] = {"", "Open file manager", "Close dashboard", "Back to dashboard"};
    const Theme *t = m->night ? &g_night : &g_day;
    char buf[48];
    int items[3], n = menu_items(has_file_manager, items), i;
    Box b;

    c->round_rect(c->ctx, MENU_X, MENU_Y, MENU_W, MENU_H, 12, t->panel, t->line);
    text(c, MENU_X + 20, MENU_Y + 40, FONT_MENU, t->text, "Dashboard menu");
    snprintf(buf, sizeof buf, "Montero dashboard %s", version);
    text(c, MENU_X + 20, MENU_Y + 64, FONT_SMALL, t->muted, buf);
    for (i = 0; i < n; i++) {
        int back = items[i] == MENU_BACK;
        const char *name = names[items[i]];
        menu_button(i, &b);
        c->round_rect(c->ctx, b.x, b.y, b.w, b.h, 10, back ? t->accent : t->button, back ? t->accent : t->line);
        text(c, b.x + (b.w - tw(c, FONT_MENU, name)) / 2, b.y + 35, FONT_MENU, back ? t->bg : t->text, name);
    }
}

int render_menu_hit(int x, int y, int has_file_manager)
{
    int items[3], n = menu_items(has_file_manager, items), i;
    Box b;
    for (i = 0; i < n; i++) {
        menu_button(i, &b);
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
            return items[i];
    }
    return MENU_NONE;
}

int render_clock_hit(int x, int y)
{
    return x >= 0 && x < 100 && y >= 0 && y < HEADER_H;
}
