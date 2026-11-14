#include "render.h"

#include "map.h"

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
    {34, FACE_CONDENSED}, /* FONT_VALUE */
    {56, FACE_CONDENSED}, /* FONT_SPEED */
    {26, FACE_CONDENSED}, /* FONT_STAT: average and top speed */
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
/* Widths 1:2:2 of the 752 px between the gaps: a narrow first column, two wide ones. */
static const int COL_X[3] = {12, 174, 487}, COL_W[3] = {150, 301, 301};
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
static void tile_header_pad(const Canvas *c, const Theme *t, int x, int y, int w, int pad, const char *name,
                            const char *note, const char *short_note, unsigned note_rgb)
{
    int end = x + pad + label(c, x + pad, y + 24, t->muted, name) + 10;
    if (short_note && x + w - pad - tw(c, FONT_SMALL, note) < end)
        note = short_note;
    if (x + w - pad - tw(c, FONT_SMALL, note) >= end)
        text_right(c, x + w - pad, y + 24, FONT_SMALL, note_rgb, note);
}

static void tile_header2(const Canvas *c, const Theme *t, int x, int y, int w, const char *name, const char *note,
                         const char *short_note, unsigned note_rgb)
{
    tile_header_pad(c, t, x, y, w, 17, name, note, short_note, note_rgb);
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
 * edge); by_index series are evenly spaced per sample. The value range is at least min_span;
 * from_zero graphs start at 0 (speed). */
static void chart(const Canvas *c, const Theme *t, const Series *s, Box b, unsigned now, unsigned window_ms,
                  float min_span, unsigned rgb, int area, int by_index, int from_zero)
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
    if (from_zero) {
        lo = 0.0f;
        if (hi < min_span)
            hi = min_span;
    } else if (hi - lo < min_span) {
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

/* Narrow tiles (first column, 150 px): name and graph window on top, the value, a status line
 * under it, then the graph. NP is the padding inside the border. */
#define NP 13

/* Left-aligned status line with an optional 14 px icon; the short text is used when the long one
 * does not fit in max_w, and nothing when neither does. */
static void status_line(const Canvas *c, int x, int baseline, int max_w, unsigned rgb, int icon_kind,
                        unsigned icon_rgb, const char *s, const char *short_s)
{
    int lead = icon_kind >= 0 ? 18 : 0;
    if (lead + tw(c, FONT_TREND, s) > max_w)
        s = short_s;
    if (s == NULL || lead + tw(c, FONT_TREND, s) > max_w)
        return;
    if (icon_kind >= 0)
        icon(c, icon_kind, x, baseline - 12, 14, icon_rgb);
    text(c, x + lead, baseline, FONT_TREND, rgb, s);
}

static void narrow_chart_box(int x, int y, int w, int h, Box *b)
{
    b->x = x + NP;
    b->w = w - 2 * NP;
    b->y = y + 84;
    b->h = y + h - 11 - b->y;
}

static void draw_pressure(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char buf[16];
    int sx = x + NP, sw = w - 2 * NP;
    Box b;
    tile(c, t, x, y, w, h);
    tile_header_pad(c, t, x, y, w, NP, "PRESSURE", "1 h", NULL, t->muted);
    if (m->have_pressure)
        fmt_commas(buf, sizeof buf, m->pressure_hpa);
    else
        strcpy(buf, "--");
    value_unit(c, t, sx, y + 56, FONT_VALUE, buf, FONT_UNIT, "hPa", 5);
    switch (m->weather) {
    case WEATHER_FALLING_FAST: status_line(c, sx, y + 76, sw, t->text, ICON_DOWN, t->warn, "Falling fast", "Falling"); break;
    case WEATHER_FALLING: status_line(c, sx, y + 76, sw, t->text, ICON_DOWN, t->muted, "Falling", NULL); break;
    case WEATHER_RISING: status_line(c, sx, y + 76, sw, t->text, ICON_UP, t->muted, "Rising", NULL); break;
    case WEATHER_STEADY: status_line(c, sx, y + 76, sw, t->text, ICON_FLAT, t->muted, "Weather steady", "Steady"); break;
    default:
        if (m->have_pressure)
            status_line(c, sx, y + 76, sw, t->muted, -1, 0, "No trend yet", "No trend");
    }
    narrow_chart_box(x, y, w, h, &b);
    chart(c, t, &m->pressure, b, m->now_ms, 3600000u, 4.0f, t->cool, 0, 0, 0);
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
    chart(c, t, &m->elevation, b, m->now_ms, 0, 40.0f, t->accent, 1, 1, 0);
}

static void draw_outside(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char buf[40], brief[24];
    int sx = x + NP;
    Box b;
    tile(c, t, x, y, w, h);
    tile_header_pad(c, t, x, y, w, NP, "OUTSIDE", "3 h", NULL, t->muted);
    if (m->have_outside)
        snprintf(buf, sizeof buf, "%d", m->outside_c);
    else
        strcpy(buf, "--");
    value_unit(c, t, sx, y + 56, FONT_VALUE, buf, FONT_UNIT, DEG "C", 5);
    if (m->have_temp_change) {
        int d = iround(m->temp_change), minutes = (int)(m->temp_span_s / 60u);
        char span[16];
        if (minutes < 60)
            snprintf(span, sizeof span, "%d min", minutes);
        else
            snprintf(span, sizeof span, "%d h", (minutes + 30) / 60);
        if (d == 0) {
            strcpy(buf, "Steady");
            strcpy(brief, "Steady");
        } else {
            snprintf(buf, sizeof buf, "%s %d" DEG " in %s", d > 0 ? "Up" : "Down", d > 0 ? d : -d, span);
            snprintf(brief, sizeof brief, "%s %d" DEG, d > 0 ? "Up" : "Down", d > 0 ? d : -d);
        }
        status_line(c, sx, y + 76, w - 2 * NP, t->text, -1, 0, buf, brief);
    }
    narrow_chart_box(x, y, w, h, &b);
    chart(c, t, &m->outside, b, m->now_ms, 10800000u, 4.0f, t->cool, 0, 0, 0);
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
    const char *status = NULL, *brief = NULL;
    int kind = -1, sx = x + NP;
    unsigned icon_rgb = t->muted, text_rgb = t->text;
    Box b;

    tile(c, t, x, y, w, h);
    tile_header_pad(c, t, x, y, w, NP, "BATTERY", "30 min", NULL, t->muted);
    switch (m->battery_state) {
    case BATT_CHARGING: status = "Charging"; kind = ICON_BOLT; icon_rgb = t->cool; break;
    case BATT_NOT_CHARGING: status = "Not charging"; brief = "Idle"; kind = ICON_BATTERY; text_rgb = t->muted; break;
    case BATT_LOW: status = "Low"; kind = ICON_WARN; icon_rgb = t->warn; break;
    case BATT_HIGH: status = "Too high"; brief = "High"; kind = ICON_WARN; icon_rgb = t->warn; break;
    }
    if (m->have_battery) {
        int dv = iround(m->battery_v * 10.0);
        snprintf(buf, sizeof buf, "%d.%d", dv / 10, dv % 10);
    } else
        strcpy(buf, "--");
    value_unit(c, t, sx, y + 56, FONT_VALUE, buf, FONT_UNIT, "V", 5);
    if (status)
        status_line(c, sx, y + 76, w - 2 * NP, text_rgb, kind, icon_rgb, status, brief);
    narrow_chart_box(x, y, w, h, &b);
    chart(c, t, &m->battery, b, m->now_ms, 1800000u, 0.8f, t->cool, 0, 0, 0);
}

/* Small "Avg 32" style figure, right-aligned. */
static void speed_stat(const Canvas *c, const Theme *t, int right, int baseline, const char *name, int have, float kmh)
{
    char buf[16];
    int w;
    if (have)
        snprintf(buf, sizeof buf, "%d", iround(kmh));
    else
        strcpy(buf, "--");
    w = tw(c, FONT_STAT, buf);
    text(c, right - w, baseline, FONT_STAT, t->text, buf);
    text_right(c, right - w - 6, baseline, FONT_SMALL, t->muted, name);
}

/* Current speed large on the left, average (while moving) and top speed on the right, and the
 * last 10 minutes as a graph. */
static void draw_speed(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    char buf[16];
    int right = x + w - 17;
    Box b;

    tile(c, t, x, y, w, h);
    tile_header(c, t, x, y, w, "SPEED", "Last 10 min", t->muted);
    if (m->have_speed)
        snprintf(buf, sizeof buf, "%d", iround(m->speed_kmh));
    else
        strcpy(buf, "--");
    value_unit(c, t, x + 17, y + 80, FONT_SPEED, buf, FONT_UNIT, "km/h", 6);
    speed_stat(c, t, right, y + 52, "Avg", m->have_avg, m->avg_kmh);
    speed_stat(c, t, right, y + 80, "Top", m->have_top, m->top_kmh);
    b.x = x + 17; b.w = w - 34; b.h = 30; b.y = y + h - 11 - b.h;
    chart(c, t, &m->speed, b, m->now_ms, SPEED_WINDOW_MS, 20.0f, t->accent, 0, 0, 1);
}

/* ---------------------------------------------------------------- map */

/* The map picture: column 3, rows 1 and 2, the whole tile inside its 1 px border (no title). */
static void map_box(Box *b)
{
    b->x = COL_X[2] + 1;
    b->y = ROW_Y[0] + 1;
    b->w = COL_W[2] - 2;
    b->h = ROW_Y[1] + ROW_H[1] - ROW_Y[0] - 2;
}

/* Paints the screen colour outside the rounded corners (radius r) of the box, so a picture drawn
 * to the edges keeps the tile's rounded shape. */
static void round_corners(const Canvas *c, unsigned bg, int x, int y, int w, int h, int r)
{
    static const double arc[5][2] = {{1, 0}, {0.924, 0.383}, {0.707, 0.707}, {0.383, 0.924}, {0, 1}};
    int k, i;
    for (k = 0; k < 4; k++) { /* top left, top right, bottom right, bottom left */
        int sx = k == 0 || k == 3 ? -1 : 1, sy = k < 2 ? -1 : 1;
        int cx = sx < 0 ? x + r : x + w - r, cy = sy < 0 ? y + r : y + h - r;
        DPoint p[6];
        p[0].x = sx < 0 ? x : x + w;
        p[0].y = sy < 0 ? y : y + h;
        for (i = 0; i < 5; i++) {
            p[i + 1].x = cx + sx * iround(r * arc[i][0]);
            p[i + 1].y = cy + sy * iround(r * arc[i][1]);
        }
        c->polygon(c->ctx, p, 6, bg);
    }
}

static void zoom_button(int index, Box *z)
{
    Box b;
    map_box(&b);
    z->w = z->h = 44;
    z->x = b.x + b.w - 6 - z->w;
    z->y = b.y + 6 + index * 50;
}

static void centred(const Canvas *c, const Box *b, int baseline, int font, unsigned rgb, const char *s)
{
    int w = tw(c, font, s);
    if (w <= b->w - 8)
        text(c, b->x + (b->w - w) / 2, baseline, font, rgb, s);
}

/* The car: an arrow along the GPS course while moving, otherwise a dot; grey without GPS. */
static void draw_marker(const Canvas *c, const Theme *t, const DashModel *m, int cx, int cy)
{
    static const double arrow[4][2] = {{0, -14}, {10, 11}, {0, 5}, {-10, 11}};
    if (m->gps_ok && m->heading_src == SRC_GPS) {
        double a = m->heading_deg * 3.14159265358979 / 180.0, s = sin(a), co = cos(a);
        DPoint p[5];
        int i;
        for (i = 0; i < 4; i++) {
            p[i].x = cx + iround(arrow[i][0] * co - arrow[i][1] * s);
            p[i].y = cy + iround(arrow[i][0] * s + arrow[i][1] * co);
        }
        p[4] = p[0];
        c->polyline(c->ctx, p, 5, 4, t->bg); /* dark outline so it stands out on any map colour */
        c->polygon(c->ctx, p, 4, t->accent);
    } else {
        c->circle(c->ctx, cx, cy, 7, m->gps_ok ? t->accent : t->muted, t->bg, 3);
    }
}

/* Scale bar with a round distance of at most 64 px, bottom left. */
static void draw_scale(const Canvas *c, const Theme *t, const DashModel *m, const Box *b)
{
    static const int nice[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};
    double mpp = map_metres_per_pixel(m->lat, m->map_zoom);
    char buf[16];
    DPoint p[4];
    int i, len = 0, metres = 0, bw, bx, by;

    for (i = 0; i < (int)(sizeof nice / sizeof nice[0]); i++) {
        int px = mpp > 0 ? iround(nice[i] / mpp) : 0;
        if (px > 64)
            break;
        metres = nice[i];
        len = px;
    }
    if (metres == 0 || len < 8)
        return;
    if (metres >= 1000)
        snprintf(buf, sizeof buf, "%d km", metres / 1000);
    else
        snprintf(buf, sizeof buf, "%d m", metres);
    bw = (len > tw(c, FONT_SMALL, buf) ? len : tw(c, FONT_SMALL, buf)) + 14;
    bx = b->x + 4;
    by = b->y + b->h - 4 - 27;
    c->round_rect(c->ctx, bx, by, bw, 27, 4, t->panel, t->panel);
    text(c, bx + 7, by + 13, FONT_SMALL, t->text, buf);
    p[0].x = bx + 7; p[0].y = by + 16;
    p[1].x = bx + 7; p[1].y = by + 21;
    p[2].x = bx + 7 + len; p[2].y = by + 21;
    p[3].x = bx + 7 + len; p[3].y = by + 16;
    c->polyline(c->ctx, p, 4, 2, t->text);
}

static void draw_zoom_buttons(const Canvas *c, const Theme *t, const DashModel *m)
{
    int i;
    for (i = 0; i < 2; i++) {
        Box z;
        int enabled = i == 0 ? m->map_zoom < m->map_max_zoom : m->map_zoom > m->map_min_zoom;
        unsigned rgb = enabled ? t->text : t->line;
        int cx, cy;
        zoom_button(i, &z);
        cx = z.x + z.w / 2;
        cy = z.y + z.h / 2;
        c->round_rect(c->ctx, z.x, z.y, z.w, z.h, 8, t->panel, t->line);
        line(c, cx - 8, cy, cx + 8, cy, 3, rgb);
        if (i == 0)
            line(c, cx, cy - 8, cx, cy + 8, 3, rgb);
    }
}

static void draw_map(const Canvas *c, const Theme *t, const DashModel *m, int x, int y, int w, int h)
{
    /* credits the map's licences ask for: OpenMapTiles (CC-BY 4.0), OpenStreetMap data (ODbL) */
    static const char *const credit = "\xC2\xA9 OpenMapTiles \xC2\xA9 OpenStreetMap";
    int files = c->map != NULL && m->map_max_zoom >= 0;
    Box b;

    tile(c, t, x, y, w, h);
    map_box(&b);
    if (!files || !m->have_position) { /* messages on the plain tile */
        if (!files) {
            centred(c, &b, b.y + b.h / 2 - 4, FONT_HEADER, t->text, "No map files");
            centred(c, &b, b.y + b.h / 2 + 16, FONT_SMALL, t->muted, "Copy the Map folder to the SD card");
        } else {
            centred(c, &b, b.y + b.h / 2 + 5, FONT_HEADER, t->muted, "Waiting for GPS");
        }
    } else {
        double px, py;
        int cw;
        map_world_pixel(m->lat, m->lon, m->map_zoom, &px, &py);
        if (c->map(c->ctx, b.x, b.y, b.w, b.h, m->map_zoom, px, py, m->night) != MAP_SHOWN) {
            const char *msg = "No map for this area";
            int mw = tw(c, FONT_HEADER, msg);
            c->round_rect(c->ctx, b.x + (b.w - mw) / 2 - 8, b.y + b.h / 2 + 26, mw + 16, 26, 4, t->panel, t->line);
            centred(c, &b, b.y + b.h / 2 + 44, FONT_HEADER, t->text, msg);
        }
        draw_marker(c, t, m, b.x + b.w / 2, b.y + b.h / 2);
        draw_scale(c, t, m, &b);
        cw = tw(c, FONT_SMALL, credit);
        c->round_rect(c->ctx, b.x + b.w - 4 - cw - 10, b.y + b.h - 4 - 20, cw + 10, 20, 4, t->panel, t->panel);
        text(c, b.x + b.w - 4 - cw - 5, b.y + b.h - 4 - 6, FONT_SMALL, t->muted, credit);
        draw_zoom_buttons(c, t, m);
        round_corners(c, t->bg, x, y, w, h, 10);
        c->round_rect(c->ctx, x, y, w, h, 10, NO_FILL, t->line);
    }
}

int render_map_hit(int x, int y)
{
    int i;
    for (i = 0; i < 2; i++) {
        Box z;
        zoom_button(i, &z);
        if (x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h)
            return i == 0 ? MAPHIT_ZOOM_IN : MAPHIT_ZOOM_OUT;
    }
    return MAPHIT_NONE;
}

void render_dashboard(const Canvas *c, const DashModel *m)
{
    const Theme *t = m->night ? &g_night : &g_day;

    c->fill_rect(c->ctx, 0, 0, SCREEN_W, SCREEN_H, t->bg);
    draw_header(c, t, m);
    draw_outside(c, t, m, COL_X[0], ROW_Y[0], COL_W[0], ROW_H[0]);
    draw_pressure(c, t, m, COL_X[0], ROW_Y[1], COL_W[0], ROW_H[1]);
    draw_battery(c, t, m, COL_X[0], ROW_Y[2], COL_W[0], ROW_H[2]);
    draw_heading(c, t, m, COL_X[1], ROW_Y[0], COL_W[1], ROW_Y[1] + ROW_H[1] - ROW_Y[0]);
    draw_speed(c, t, m, COL_X[1], ROW_Y[2], COL_W[1], ROW_H[2]);
    draw_map(c, t, m, COL_X[2], ROW_Y[0], COL_W[2], ROW_Y[1] + ROW_H[1] - ROW_Y[0]);
    draw_elevation(c, t, m, COL_X[2], ROW_Y[2], COL_W[2], ROW_H[2]);
}

/* ---------------------------------------------------------------- maintenance menu */

#define MENU_X 190
#define MENU_Y 48
#define MENU_W 420
#define MENU_H 384
#define TZ_Y (MENU_Y + 84)           /* time-zone row: [-] value [+], 56 px tall */
#define TZ_PLUS_X (MENU_X + MENU_W - 20 - 56)
#define TZ_MINUS_X (TZ_PLUS_X - 110 - 56)

static void menu_button(int index, Box *b)
{
    b->x = MENU_X + 20;
    b->y = MENU_Y + 172 + index * 68;
    b->w = MENU_W - 40;
    b->h = 56;
}

static void tz_text(char *buf, int size, int offset_min)
{
    int a = offset_min < 0 ? -offset_min : offset_min;
    if (offset_min == 0)
        snprintf(buf, size, "UTC");
    else if (a % 60)
        snprintf(buf, size, "UTC%c%d:%02d", offset_min < 0 ? '-' : '+', a / 60, a % 60);
    else
        snprintf(buf, size, "UTC%c%d", offset_min < 0 ? '-' : '+', a / 60);
}

static void draw_tz_row(const Canvas *c, const Theme *t, const DashModel *m)
{
    char buf[24];
    int mid = TZ_Y + 28;
    const char *source = m->clock_src == SRC_GPS ? "Clock from GPS time"
                       : m->clock_src == SRC_CAR ? "Clock from the head unit" : "No clock yet: waiting for GPS";

    text(c, MENU_X + 20, mid + 6, FONT_MENU, t->text, "Time zone");
    c->round_rect(c->ctx, TZ_MINUS_X, TZ_Y, 56, 56, 10, t->button, t->line);
    c->round_rect(c->ctx, TZ_PLUS_X, TZ_Y, 56, 56, 10, t->button, t->line);
    line(c, TZ_MINUS_X + 18, mid, TZ_MINUS_X + 38, mid, 3, t->text);
    line(c, TZ_PLUS_X + 18, mid, TZ_PLUS_X + 38, mid, 3, t->text);
    line(c, TZ_PLUS_X + 28, mid - 10, TZ_PLUS_X + 28, mid + 10, 3, t->text);
    tz_text(buf, sizeof buf, m->utc_offset_min);
    text(c, TZ_MINUS_X + 56 + (110 - tw(c, FONT_MENU, buf)) / 2, mid + 7, FONT_MENU, t->text, buf);
    text(c, MENU_X + 20, TZ_Y + 76, FONT_SMALL, t->muted, source);
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
    draw_tz_row(c, t, m);
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
    if (y >= TZ_Y && y < TZ_Y + 56) {
        if (x >= TZ_MINUS_X && x < TZ_MINUS_X + 56)
            return MENU_TZ_MINUS;
        if (x >= TZ_PLUS_X && x < TZ_PLUS_X + 56)
            return MENU_TZ_PLUS;
    }
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
