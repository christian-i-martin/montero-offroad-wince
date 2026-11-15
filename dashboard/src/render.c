#include "render.h"

#include "map.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32_WCE
#define snprintf _snprintf
#endif

#define DEG "\xC2\xB0"           /* degree sign in UTF-8 */
#define DOT " \xC2\xB7 "         /* middle dot between parts of the address */

const FontSpec render_fonts[FONT_COUNT] = {
    {28, FACE_CONDENSED}, /* FONT_CLOCK */
    {15, FACE_MEDIUM},    /* FONT_HEADER */
    {11, FACE_SEMIBOLD},  /* FONT_LABEL: upper-case panel names */
    {12, FACE_MEDIUM},    /* FONT_SMALL */
    {13, FACE_MEDIUM},    /* FONT_TREND */
    {14, FACE_SEMIBOLD},  /* FONT_UNIT */
    {36, FACE_CONDENSED}, /* FONT_VALUE: elevation */
    {30, FACE_CONDENSED}, /* FONT_TILE: outside, pressure, battery */
    {82, FACE_CONDENSED}, /* FONT_SPEED */
    {62, FACE_CONDENSED}, /* FONT_SPEED3: speeds of three digits */
    {22, FACE_CONDENSED}, /* FONT_STAT: average and top speed */
    {34, FACE_CONDENSED}, /* FONT_HEADING */
    {15, FACE_CONDENSED}, /* FONT_TAPE: compass letters */
    {19, FACE_SEMIBOLD},  /* FONT_MENU */
    {18, FACE_SEMIBOLD},  /* FONT_ADDR: the road name */
};

typedef struct {
    unsigned bg;                 /* the screen where no map is drawn */
    unsigned panel; int alpha;   /* semi-transparent panels: colour and strength (0 to 256) */
    unsigned solid;              /* opaque panels: the menu, and platforms that cannot mix colours */
    unsigned border, line, text, muted, accent, graph, cool, warn, area, spark, button, on_accent, outline;
    unsigned plate;              /* the number plate's background on the car info page */
} Theme;
/* Day: light panels; night: dark panels, warm amber. Both mixed in at 90 % (230 of 256, the user's
 * choice). accent is for text, graph for lines and marks, cool for the car on the map, area for the
 * filled part of the elevation graph. */
static const Theme g_day = {0xE9E7DF, 0xFAFAF6, 230, 0xF4F4F0, 0xD5D7D3, 0xC6C9C5, 0x17202A, 0x4B5763, 0xB5400C,
                            0xE8590C, 0x1971C2, 0xC92A2A, 0xF5DECD, 0x5E6B78, 0xE4E5E1, 0xFFFFFF, 0xFFFFFF,
                            0xFFFFFF};
static const Theme g_night = {0x1E242A, 0x10141A, 230, 0x161B21, 0x2C3138, 0x3A414A, 0xEDE8DE, 0xA3ADB7, 0xF6A623,
                              0xF6A623, 0x5BC0EB, 0xFF7849, 0x382F1F, 0x8A96A3, 0x232931, 0x14181C, 0x0B0E11,
                              0x1A2027};

/* Layout (design canvas): 10 px from the screen edges, 8 px between panels. */
#define EDGE 10
#define GAP 8
#define PAD 10                                   /* inside a panel */
#define RADIUS 14
#define TOP (HEADER_H + GAP)                     /* 48 */
#define BOTTOM (SCREEN_H - EDGE)                 /* 470 */
#define SIDE_W 133                               /* side columns: 1/6 of the screen */
#define WIDE_W 266                               /* bottom panels: 2/6 */
#define LOW_H 88
#define LOW_Y (BOTTOM - LOW_H)                   /* 382 */
#define COL_BOTTOM (LOW_Y - GAP)                 /* 374 */
#define LEFT_X EDGE
#define RIGHT_X (SCREEN_W - EDGE - SIDE_W)       /* 657 */
#define ELEV_H 156
#define SPEED_H (COL_BOTTOM - TOP - GAP - ELEV_H) /* 162 */
#define ZOOM_W 92                                /* zoom buttons, bottom centre: minus | plus */
#define ZOOM_H 44
#define ZOOM_X (SCREEN_W - EDGE - WIDE_W - EDGE - ZOOM_W)
#define ZOOM_Y (BOTTOM - ZOOM_H)

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

static int label_width(const Canvas *c, const char *s)
{
    char ch[2] = {0, 0};
    int w = 0;
    for (; *s; s++) {
        ch[0] = *s;
        w += tw(c, FONT_LABEL, ch) + 1;
    }
    return w;
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

/* ---------------------------------------------------------------- pixel icons */

/* Icons for the low-resolution screen: drawn at the size they are designed for, from filled
 * polygons with whole-pixel corners (2 px strokes, 45-degree or 2:1 slopes), so every edge falls
 * on a pixel. Each icon is a list of polygons, each written as its point count and the points,
 * ending with 0. */
enum {
    ICON_GPS, ICON_SUNSET, ICON_SUNRISE, ICON_LIGHTS, ICON_WARN, ICON_WARN_MARK, ICON_UP, ICON_DOWN, ICON_FLAT,
    ICON_PIN, ICON_PLUS, ICON_MINUS, ICON_NORTH, ICON_COUNT
};

#define R(x, y, w, h) 4, x, y, (x) + (w), y, (x) + (w), (y) + (h), x, (y) + (h)
#define SUN_ON_HORIZON 12, 8, 11, 12, 11, 12, 12, 14, 12, 14, 13, 15, 13, 15, 17, 5, 17, 5, 13, 6, 13, 6, 12, 8, 12, \
                       R(0, 13, 3, 2), R(17, 13, 3, 2), R(0, 17, 20, 2)

static const signed char g_gps[] = {   /* 20 px: a ring of eight pieces, a centre dot, four ticks */
    4, 7, 3, 13, 3, 12, 5, 8, 5,       4, 13, 3, 17, 7, 15, 8, 12, 5,
    4, 17, 7, 17, 13, 15, 12, 15, 8,   4, 17, 13, 13, 17, 12, 15, 15, 12,
    4, 13, 17, 7, 17, 8, 15, 12, 15,   4, 7, 17, 3, 13, 5, 12, 8, 15,
    4, 3, 13, 3, 7, 5, 8, 5, 12,       4, 3, 7, 7, 3, 8, 5, 5, 8,
    R(8, 8, 4, 4), R(9, 0, 2, 3), R(9, 17, 2, 3), R(0, 9, 3, 2), R(17, 9, 3, 2), 0};
static const signed char g_sunset[] = {R(9, 0, 2, 5), 3, 6, 5, 14, 5, 10, 9, SUN_ON_HORIZON, 0};     /* 20 px */
static const signed char g_sunrise[] = {3, 10, 0, 14, 4, 6, 4, R(9, 4, 2, 5), SUN_ON_HORIZON, 0};    /* 20 px */
static const signed char g_lights[] = {6, 11, 3, 15, 3, 18, 6, 18, 14, 15, 17, 11, 17,               /* 20 px */
                                       R(5, 4, 4, 2), R(1, 5, 4, 2), R(5, 9, 4, 2), R(1, 10, 4, 2),
                                       R(5, 14, 4, 2), R(1, 15, 4, 2), 0};
static const signed char g_warn[] = {3, 10, 1, 19, 18, 1, 18, 0};                                    /* 20 px */
static const signed char g_warn_mark[] = {R(9, 7, 2, 6), R(9, 14, 2, 2), 0};
static const signed char g_up[] = {7, 7, 1, 13, 7, 9, 7, 9, 13, 5, 13, 5, 7, 1, 7, 0};               /* 14 px */
static const signed char g_down[] = {7, 5, 1, 9, 1, 9, 7, 13, 7, 7, 13, 1, 7, 5, 7, 0};              /* 14 px */
static const signed char g_flat[] = {R(1, 6, 12, 2), 0};                                             /* 14 px */
static const signed char g_pin[] = {4, 5, 1, 11, 1, 14, 4, 2, 4, R(2, 4, 4, 4), R(10, 4, 4, 4),      /* 16 px */
                                    3, 2, 8, 14, 8, 8, 14, 0};
static const signed char g_plus[] = {R(8, 3, 2, 12), R(3, 8, 12, 2), 0};                             /* 18 px */
static const signed char g_minus[] = {R(3, 8, 12, 2), 0};                                            /* 18 px */
static const signed char g_north[] = {3, 12, 1, 17, 11, 7, 11, 0};                                   /* 24 px */

static const signed char *const g_icons[ICON_COUNT] = {g_gps, g_sunset, g_sunrise, g_lights, g_warn, g_warn_mark,
                                                       g_up, g_down, g_flat, g_pin, g_plus, g_minus, g_north};

/* Header icons of the car info page and its button. Rings are drawn as pieces (no polygon has a
 * hole), like the GPS icon. */
static const signed char g_back[] = {6, 12, 3, 5, 10, 12, 17, 14, 15, 9, 10, 14, 5, 0};              /* 20 px */
static const signed char g_clock[] = {                                                               /* 20 px */
    4, 7, 2, 13, 2, 12, 4, 8, 4,        4, 13, 2, 18, 7, 16, 8, 12, 4,
    4, 18, 7, 18, 13, 16, 12, 16, 8,    4, 18, 13, 13, 18, 12, 16, 16, 12,
    4, 13, 18, 7, 18, 8, 16, 12, 16,    4, 7, 18, 2, 13, 4, 12, 8, 16,
    4, 2, 13, 2, 7, 4, 8, 4, 12,        4, 2, 7, 7, 2, 8, 4, 4, 8,
    R(9, 6, 2, 5), R(11, 9, 3, 2), 0};
/* The car info button, 26 x 20 (drawn 1 px up): the front of a car with an "i" badge cut into its
 * lower right corner. */
static const signed char g_carbtn_car[] = {
    4, 2, 2, 16, 2, 17, 4, 1, 4,        4, 1, 4, 4, 4, 4, 6, 0, 6,      4, 14, 4, 17, 4, 18, 6, 14, 6,
    4, 0, 6, 16, 6, 14, 8, 0, 8,        R(0, 8, 2, 2),                  4, 5, 8, 14, 8, 12, 10, 5, 10,
    R(0, 10, 12, 3), R(2, 13, 4, 2), 0};
static const signed char g_carbtn_badge[] = {
    4, 17, 8, 23, 8, 25, 10, 15, 10,    5, 15, 10, 19, 10, 19, 12, 14, 12, 14, 11,
    5, 21, 10, 25, 10, 26, 11, 26, 12, 21, 12,                          R(14, 12, 12, 2),
    5, 14, 14, 19, 14, 19, 18, 15, 18, 14, 17,                          5, 21, 14, 26, 14, 26, 17, 25, 18, 21, 18,
    4, 15, 18, 25, 18, 23, 20, 17, 20, 0};

static void icon_shape(const Canvas *c, const signed char *q, int x, int y, unsigned rgb)
{
    while (*q) {
        DPoint p[16];
        int n = *q++, i;
        for (i = 0; i < n; i++, q += 2) {
            p[i].x = x + q[0];
            p[i].y = y + q[1];
        }
        c->polygon(c->ctx, p, n, rgb);
    }
}

static void icon(const Canvas *c, int kind, int x, int y, unsigned rgb)
{
    icon_shape(c, g_icons[kind], x, y, rgb);
}

static void warn_icon(const Canvas *c, const Theme *t, int x, int y)
{
    icon(c, ICON_WARN, x, y, t->warn);
    icon(c, ICON_WARN_MARK, x, y, t->on_accent);
}

/* ---------------------------------------------------------------- building blocks */

/* A semi-transparent panel with a 1 px border; solid where the platform cannot mix colours. */
static void glass(const Canvas *c, const Theme *t, int x, int y, int w, int h, int r)
{
    if (c->glass)
        c->glass(c->ctx, x, y, w, h, r, t->panel, t->alpha);
    else
        c->round_rect(c->ctx, x, y, w, h, r, t->solid, t->solid);
    c->round_rect(c->ctx, x, y, w, h, r, NO_FILL, t->border);
}

/* Panel name on the left, a note on the right; the short note is used when the long one would
 * touch the name (wider fallback fonts), and none when neither fits. */
static void panel_header(const Canvas *c, const Theme *t, int x, int y, int w, const char *name, const char *note,
                         const char *short_note, unsigned note_rgb)
{
    int end = x + PAD + label(c, x + PAD, y + 20, t->muted, name) + 6, right = x + w - PAD;
    if (note && short_note && right - tw(c, FONT_SMALL, note) < end)
        note = short_note;
    if (note && right - tw(c, FONT_SMALL, note) >= end)
        text_right(c, right, y + 20, FONT_SMALL, note_rgb, note);
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

/* s, or short_s when s is wider than max_w, or nothing when neither fits. */
static void text_fit(const Canvas *c, int x, int baseline, int font, unsigned rgb, int max_w, const char *s,
                     const char *short_s)
{
    if (tw(c, font, s) > max_w)
        s = short_s;
    if (s && tw(c, font, s) <= max_w)
        text(c, x, baseline, font, rgb, s);
}

/* s cut to max_w with an ellipsis when it is wider (whole UTF-8 characters only). */
static void fit_text(const Canvas *c, int font, const char *s, int max_w, char *out, int size)
{
    int n;
    snprintf(out, (size_t)size, "%s", s);
    if (tw(c, font, out) <= max_w)
        return;
    n = (int)strlen(out);
    while (n > 0) {
        do
            n--;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80);
        if (n + 4 > size)
            continue;
        out[n] = (char)0xE2; /* the ellipsis character, in place */
        out[n + 1] = (char)0x80;
        out[n + 2] = (char)0xA6;
        out[n + 3] = 0;
        if (tw(c, font, out) <= max_w)
            return;
    }
    out[0] = 0;
}

static void fitted(const Canvas *c, int x, int baseline, int font, unsigned rgb, int max_w, const char *s)
{
    char buf[3 * ADDRESS_TEXT + 32];
    fit_text(c, font, s, max_w, buf, sizeof buf);
    text(c, x, baseline, font, rgb, buf);
}

static void centred(const Canvas *c, int x, int w, int baseline, int font, unsigned rgb, const char *s)
{
    int sw = tw(c, font, s);
    if (sw <= w - 8)
        text(c, x + (w - sw) / 2, baseline, font, rgb, s);
}

/* ---------------------------------------------------------------- graphs */

/* Graph of a series as a line through every sample (straight pieces: a curve smoothed through
 * averaged points hid short changes). The samples there are always span the whole width, oldest at
 * the left edge and newest at the right: time series by their time (only the last window_ms),
 * by_index series evenly. Until a graph's window is full it is therefore stretched over the samples
 * it has; placed by age over the full window, a short history sat in the last few pixels at the
 * right and the graph looked flat. A single sample is a flat line across. The value range is at
 * least min_span. area graphs are filled below the line and have a base line. */
static void chart(const Canvas *c, const Theme *t, const Series *s, Box b, unsigned now, unsigned window_ms,
                  float min_span, unsigned rgb, int area, int by_index)
{
    DPoint p[SERIES_MAX + 3];
    float v, lo = 0, hi = 0;
    unsigned tick, first = 0, last = 0, span;
    int i, n = 0, used = 0, bottom = b.y + b.h - 1, right = b.x + b.w - 1;

    for (i = 0; i < s->count; i++) {
        series_get(s, i, &v, &tick);
        if (!by_index && now - tick > window_ms)
            continue;
        if (used == 0)
            first = tick;
        last = tick;
        if (used == 0 || v < lo) lo = v;
        if (used == 0 || v > hi) hi = v;
        used++;
    }
    span = last - first;
    if (area)
        c->fill_rect(c->ctx, b.x, bottom, b.w, 1, t->line);
    if (used == 0) {
        centred(c, b.x, b.w, b.y + b.h / 2 + 4, FONT_SMALL, t->muted, "Collecting data");
        return;
    }
    if (hi - lo < min_span) {
        float mid = (hi + lo) / 2.0f;
        lo = mid - min_span / 2.0f;
        hi = mid + min_span / 2.0f;
    }
    n = 2; /* p[0]: the area's first corner, p[1]: the first value at the left edge */
    for (i = 0; i < s->count; i++) {
        series_get(s, i, &v, &tick);
        if (!by_index && now - tick > window_ms)
            continue;
        if (by_index)
            p[n].x = s->count < 2 ? right : right - iround((double)(s->count - 1 - i) * (b.w - 1) / (s->count - 1));
        else
            p[n].x = span == 0 ? right : right - iround((double)(last - tick) * (b.w - 1) / span);
        p[n].y = b.y + 2 + iround((hi - v) / (hi - lo) * (b.h - 5));
        n++;
    }
    p[1].x = b.x; /* the same point as the first sample, except when there is only one */
    p[1].y = p[2].y;
    if (area) {
        p[0].x = b.x;
        p[0].y = bottom;
        p[n].x = p[n - 1].x;
        p[n].y = bottom;
        c->polygon(c->ctx, p, n + 1, t->area);
    }
    c->polyline(c->ctx, p + 1, n - 1, 2, rgb);
}

/* A time span for a graph's note: "3 min" (at least 1), "1.5 h", "2 h". */
static void span_text(char *buf, int size, unsigned span)
{
    unsigned minutes = (span + 30000u) / 60000u, tenths = (span + 180000u) / 360000u;
    if (minutes < 60)
        snprintf(buf, (size_t)size, "%u min", minutes ? minutes : 1u);
    else if (tenths % 10)
        snprintf(buf, (size_t)size, "%u.%u h", tenths / 10, tenths % 10);
    else
        snprintf(buf, (size_t)size, "%u h", tenths / 10);
}

/* The note over a time graph: its window (full) once the samples fill it, otherwise the time from
 * the oldest to the newest sample ("3 min", "1.5 h"); NULL while there is a single sample. */
static const char *time_note(char *buf, int size, const Series *s, unsigned now, unsigned window_ms,
                             unsigned every_ms, const char *full)
{
    unsigned tick, first = 0, last = 0, span;
    int i, used = 0;

    for (i = 0; i < s->count; i++) {
        series_get(s, i, NULL, &tick);
        if (now - tick > window_ms)
            continue;
        if (used++ == 0)
            first = tick;
        last = tick;
    }
    if (used < 2)
        return NULL;
    span = last - first;
    if (span + every_ms >= window_ms)
        return full;
    span_text(buf, size, span);
    return buf;
}

/* The note over the elevation graph (a point per 10 m of climb or descent, the last 20): how long
 * ago its oldest point was taken ("25 min"); NULL while there is a single point. */
static const char *elevation_note(char *buf, int size, const Series *s, unsigned now)
{
    unsigned tick;
    if (s->count < 2 || !series_get(s, 0, NULL, &tick))
        return NULL;
    span_text(buf, size, now - tick);
    return buf;
}

/* ---------------------------------------------------------------- header */

/* "2:05 PM": times are shown in the 12-hour form (the user's choice). */
static void fmt_time12(char *buf, int size, int hour, int minute)
{
    snprintf(buf, (size_t)size, "%d:%02d %s", (hour + 11) % 12 + 1, minute, hour < 12 ? "AM" : "PM");
}

/* The semi-transparent bar across the top, with its bottom line. */
static void header_bar(const Canvas *c, const Theme *t)
{
    if (c->glass)
        c->glass(c->ctx, 0, 0, SCREEN_W, HEADER_H, 0, t->panel, t->alpha);
    else
        c->fill_rect(c->ctx, 0, 0, SCREEN_W, HEADER_H, t->solid);
    c->fill_rect(c->ctx, 0, HEADER_H - 1, SCREEN_W, 1, t->border);
}

/* The car info button at the right end of the header: an icon on an outlined pill, the whole
 * header height and a little more to the left counting as the button. */
#define CARBTN_W 46
#define CARBTN_H 30
#define CARBTN_X (SCREEN_W - 16 - CARBTN_W)
#define CARBTN_Y ((HEADER_H - CARBTN_H) / 2)

static void draw_carinfo_button(const Canvas *c, const Theme *t)
{
    if (c->glass)
        c->glass(c->ctx, CARBTN_X, CARBTN_Y, CARBTN_W, CARBTN_H, CARBTN_H / 2, t->panel, t->alpha);
    c->round_rect(c->ctx, CARBTN_X, CARBTN_Y, CARBTN_W, CARBTN_H, CARBTN_H / 2, NO_FILL, t->line);
    icon_shape(c, g_carbtn_car, CARBTN_X + 10, CARBTN_Y + 4, t->text);
    icon_shape(c, g_carbtn_badge, CARBTN_X + 10, CARBTN_Y + 4, t->accent);
}

int render_carinfo_button_hit(int x, int y)
{
    return x >= CARBTN_X - 8 && x < SCREEN_W && y >= 0 && y < HEADER_H + 4;
}

static void draw_header(const Canvas *c, const Theme *t, const DashModel *m)
{
    char buf[64], when[16], longer[64], shorter[40];
    const char *sun = NULL;
    int x, left_end, right = CARBTN_X - 12;

    header_bar(c, t);
    if (m->clock_valid) { /* the digits large, AM or PM beside them */
        snprintf(buf, sizeof buf, "%d:%02d", (m->hour + 11) % 12 + 1, m->minute);
        text(c, 16, 30, FONT_CLOCK, t->text, buf);
        x = 16 + tw(c, FONT_CLOCK, buf) + 4;
        text(c, x, 30, FONT_HEADER, t->text, m->hour < 12 ? "AM" : "PM");
        x += tw(c, FONT_HEADER, m->hour < 12 ? "AM" : "PM") + 20;
    } else {
        text(c, 16, 30, FONT_CLOCK, t->text, "--:--");
        x = 16 + tw(c, FONT_CLOCK, "--:--") + 20;
    }

    if (m->gps_ok) {
        snprintf(buf, sizeof buf, "GPS %d sats", m->satellites);
        icon(c, ICON_GPS, x, 10, t->muted);
        text(c, x + 26, 25, FONT_HEADER, t->muted, buf);
    } else {
        strcpy(buf, "Waiting for GPS");
        icon(c, ICON_GPS, x, 10, t->warn);
        text(c, x + 26, 25, FONT_HEADER, t->text, buf);
    }
    x += 26 + tw(c, FONT_HEADER, buf) + 20;
    if (m->lights_on) {
        icon(c, ICON_LIGHTS, x, 10, t->accent);
        text(c, x + 26, 25, FONT_HEADER, t->text, "Headlights on");
        x += 26 + tw(c, FONT_HEADER, "Headlights on") + 20;
    }
    if (!m->car_ok) {
        warn_icon(c, t, x, 10);
        text(c, x + 26, 25, FONT_HEADER, t->text, "No car data");
        x += 26 + tw(c, FONT_HEADER, "No car data") + 20;
    }
    left_end = x - 20;

    draw_carinfo_button(c, t);
    /* sunset or sunrise before the button; without the time left, or not at all, when it would
     * run into the items on the left */
    longer[0] = shorter[0] = '\0';
    if (!m->have_position) {
        strcpy(longer, "Sunset: waiting for GPS");
    } else if (m->sun_event != SUN_NONE) {
        fmt_time12(when, sizeof when, m->sun_hour, m->sun_minute);
        snprintf(shorter, sizeof shorter, "%s %s", m->sun_event == SUN_SUNRISE ? "Sunrise" : "Sunset", when);
        if (m->sun_event == SUN_SUNSET && m->sun_minutes_left >= 60)
            snprintf(longer, sizeof longer, "Sunset %s, %d h %d min left", when, m->sun_minutes_left / 60,
                     m->sun_minutes_left % 60);
        else if (m->sun_event == SUN_SUNSET)
            snprintf(longer, sizeof longer, "Sunset %s, %d min left", when, m->sun_minutes_left);
        else /* snprintf, not strcpy: GCC would import strcpy, which no program has used on the car */
            snprintf(longer, sizeof longer, "%s", shorter);
    }
    if (longer[0] && right - tw(c, FONT_HEADER, longer) - 26 >= left_end + 16)
        sun = longer;
    else if (shorter[0] && right - tw(c, FONT_HEADER, shorter) - 26 >= left_end + 16)
        sun = shorter;
    if (sun) {
        int w = tw(c, FONT_HEADER, sun);
        text(c, right - w, 25, FONT_HEADER, t->muted, sun);
        icon(c, m->sun_event == SUN_SUNRISE ? ICON_SUNRISE : ICON_SUNSET, right - w - 26, 10, t->muted);
    }
}

/* ---------------------------------------------------------------- left column */

/* Current speed large, average (while moving) and top speed under it. */
static void draw_speed(const Canvas *c, const Theme *t, const DashModel *m)
{
    char buf[16];
    int x = LEFT_X, y = TOP, w = SIDE_W, font = FONT_SPEED, half = x + PAD + (w - 2 * PAD) / 2 + 4;

    glass(c, t, x, y, w, SPEED_H, RADIUS);
    panel_header(c, t, x, y, w, "SPEED", "km/h", NULL, t->muted);
    if (m->have_speed)
        snprintf(buf, sizeof buf, "%d", iround(m->speed_kmh));
    else
        strcpy(buf, "--");
    if (tw(c, font, buf) > w - 2 * PAD)
        font = FONT_SPEED3;
    text(c, x + PAD, y + 100, font, t->text, buf);

    label(c, x + PAD, y + 128, t->muted, "AVG");
    label(c, half, y + 128, t->muted, "TOP");
    if (m->have_avg)
        snprintf(buf, sizeof buf, "%d", iround(m->avg_kmh));
    else
        strcpy(buf, "--");
    text(c, x + PAD, y + 150, FONT_STAT, t->text, buf);
    if (m->have_top)
        snprintf(buf, sizeof buf, "%d", iround(m->top_kmh));
    else
        strcpy(buf, "--");
    text(c, half, y + 150, FONT_STAT, t->text, buf);
}

static void draw_elevation(const Canvas *c, const Theme *t, const DashModel *m)
{
    char buf[32];
    int x = LEFT_X, y = TOP + SPEED_H + GAP, w = SIDE_W, sx = x + PAD;
    Box b;

    glass(c, t, x, y, w, ELEV_H, RADIUS);
    if (m->elev_src == SRC_CAR)
        panel_header(c, t, x, y, w, "ELEVATION", "Car sensor", "Car", t->text);
    else
        panel_header(c, t, x, y, w, "ELEVATION", elevation_note(buf, sizeof buf, &m->elevation, m->now_ms), NULL,
                     t->muted);
    if (m->elev_src != SRC_NONE)
        fmt_commas(buf, sizeof buf, iround(m->elev_m));
    else
        strcpy(buf, "--");
    value_unit(c, t, sx, y + 60, FONT_VALUE, buf, FONT_UNIT, "m", 4);
    if (m->have_climb) {
        int rate = iround(m->climb_m_min);
        if (rate > -3 && rate < 3) {
            icon(c, ICON_FLAT, sx, y + 68, t->muted);
            text(c, sx + 18, y + 80, FONT_TREND, t->text, "Steady");
        } else {
            snprintf(buf, sizeof buf, "%s %d m/min", rate > 0 ? "Up" : "Down", rate > 0 ? rate : -rate);
            icon(c, rate > 0 ? ICON_UP : ICON_DOWN, sx, y + 68, t->accent);
            text_fit(c, sx + 18, y + 80, FONT_TREND, t->accent, w - 2 * PAD - 18, buf, buf + (rate > 0 ? 3 : 5));
        }
    }
    b.x = sx; b.w = w - 2 * PAD; b.h = 56; b.y = y + ELEV_H - PAD - b.h;
    chart(c, t, &m->elevation, b, m->now_ms, 0, 40.0f, t->graph, 1, 1);
}

/* ---------------------------------------------------------------- right column */

/* One of the three small panels: name and graph window, value and unit, a status line, the graph. */
static void small_panel(const Canvas *c, const Theme *t, int y, int h, const char *name, const char *window,
                        const char *value, const char *unit)
{
    int x = RIGHT_X;
    glass(c, t, x, y, SIDE_W, h, RADIUS);
    panel_header(c, t, x, y, SIDE_W, name, window, NULL, t->muted);
    value_unit(c, t, x + PAD, y + 52, FONT_TILE, value, FONT_UNIT, unit, 3);
}

static void small_graph(const Canvas *c, const Theme *t, const DashModel *m, const Series *s, int y, int h,
                        unsigned window_ms, float min_span)
{
    Box b;
    b.x = RIGHT_X + PAD; b.w = SIDE_W - 2 * PAD; b.h = 16; b.y = y + h - PAD - b.h;
    chart(c, t, s, b, m->now_ms, window_ms, min_span, t->spark, 0, 0);
}

static void draw_outside(const Canvas *c, const Theme *t, const DashModel *m, int y, int h)
{
    char buf[40], brief[24], note[16];
    if (m->have_outside)
        snprintf(buf, sizeof buf, "%d", m->outside_c);
    else
        strcpy(buf, "--");
    small_panel(c, t, y, h, "OUTSIDE", time_note(note, sizeof note, &m->outside, m->now_ms, 10800000u, OUTSIDE_EVERY_MS, "3 h"),
                buf, DEG "C");
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
        text_fit(c, RIGHT_X + PAD, y + 70, FONT_SMALL, t->text, SIDE_W - 2 * PAD, buf, brief);
    }
    small_graph(c, t, m, &m->outside, y, h, 10800000u, 4.0f);
}

static void draw_pressure(const Canvas *c, const Theme *t, const DashModel *m, int y, int h)
{
    char buf[16], note[16];
    int sx = RIGHT_X + PAD, sw = SIDE_W - 2 * PAD;
    if (m->have_pressure)
        fmt_commas(buf, sizeof buf, m->pressure_hpa);
    else
        strcpy(buf, "--");
    small_panel(c, t, y, h, "PRESSURE", time_note(note, sizeof note, &m->pressure, m->now_ms, 3600000u, PRESSURE_EVERY_MS, "1 h"),
                buf, "hPa");
    switch (m->weather) {
    case WEATHER_FALLING_FAST: text_fit(c, sx, y + 70, FONT_SMALL, t->warn, sw, "Falling fast", "Falling"); break;
    case WEATHER_FALLING: text_fit(c, sx, y + 70, FONT_SMALL, t->text, sw, "Falling", NULL); break;
    case WEATHER_RISING: text_fit(c, sx, y + 70, FONT_SMALL, t->text, sw, "Rising", NULL); break;
    case WEATHER_STEADY: text_fit(c, sx, y + 70, FONT_SMALL, t->text, sw, "Weather steady", "Steady"); break;
    default:
        if (m->have_pressure)
            text_fit(c, sx, y + 70, FONT_SMALL, t->muted, sw, "No trend yet", "No trend");
    }
    small_graph(c, t, m, &m->pressure, y, h, 3600000u, 4.0f);
}

static void draw_battery(const Canvas *c, const Theme *t, const DashModel *m, int y, int h)
{
    char buf[16], note[16];
    const char *status = NULL, *brief = NULL;
    unsigned rgb = t->text;

    switch (m->battery_state) {
    case BATT_CHARGING: status = "Charging"; break;
    case BATT_NOT_CHARGING: status = "Not charging"; brief = "Idle"; rgb = t->muted; break;
    case BATT_LOW: status = "Low"; rgb = t->warn; break;
    case BATT_HIGH: status = "Too high"; brief = "High"; rgb = t->warn; break;
    }
    if (m->have_battery) {
        int dv = iround(m->battery_v * 10.0);
        snprintf(buf, sizeof buf, "%d.%d", dv / 10, dv % 10);
    } else
        strcpy(buf, "--");
    small_panel(c, t, y, h, "BATTERY", time_note(note, sizeof note, &m->battery, m->now_ms, 1800000u, BATTERY_EVERY_MS, "30 min"),
                buf, "V");
    if (status)
        text_fit(c, RIGHT_X + PAD, y + 70, FONT_SMALL, rgb, SIDE_W - 2 * PAD, status, brief);
    small_graph(c, t, m, &m->battery, y, h, 1800000u, 0.8f);
}

/* ---------------------------------------------------------------- heading (bottom left) */

/* Compass tape w px wide (designed at 242): 2.4 px per degree, a tick every 5 degrees (taller
 * every 15), the eight points as letters; the heading sits under the fixed mark in the middle. */
static void draw_tape(const Canvas *c, const Theme *t, int x, int y, int w, double heading)
{
    static const char *const names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int cx = x + w / 2, deg, first = (int)floor((heading - 60.0) / 5.0) * 5;
    DPoint p[3];

    for (deg = first; deg <= heading + 60.0; deg += 5) {
        int px = cx + iround((deg - heading) * 2.4), tall = deg % 15 == 0;
        if (px < x + 2 || px > x + w - 3)
            continue;
        c->fill_rect(c->ctx, px - 1, tall ? y + 20 : y + 24, 2, tall ? 8 : 4, t->muted);
        if (deg % 45 == 0) {
            const char *s = names[((deg / 45) % 8 + 8) % 8];
            int sw = tw(c, FONT_TAPE, s);
            if (px - sw / 2 >= x && px + sw / 2 <= x + w)
                text(c, px - sw / 2, y + 13, FONT_TAPE, deg % 90 == 0 ? t->text : t->muted, s);
        }
    }
    p[0].x = cx - 7; p[0].y = y + 28;
    p[1].x = cx; p[1].y = y + 21;
    p[2].x = cx + 7; p[2].y = y + 28;
    c->polygon(c->ctx, p, 3, t->graph);
}

static void draw_heading(const Canvas *c, const Theme *t, const DashModel *m)
{
    char deg[16];
    const char *dir = "--", *note = "No heading yet";
    int x = LEFT_X, y = LOW_Y, w = WIDE_W, right = x + w - PAD - 2;
    double heading = 0.0;

    glass(c, t, x, y, w, LOW_H, RADIUS);
    deg[0] = 0;
    if (m->heading_src == SRC_GPS) {
        heading = m->heading_deg;
        snprintf(deg, sizeof deg, "%d" DEG, iround(m->heading_deg) % 360);
        dir = dash_point_name(m->heading_deg);
        note = "GPS course";
    } else if (m->heading_src == SRC_CAR) {
        heading = m->heading_deg;
        dir = dash_point_name(m->heading_deg);
        note = "Car compass";
    }
    draw_tape(c, t, x + PAD + 2, y + PAD, w - 2 * PAD - 4, heading);
    text(c, x + PAD + 2, y + 80, FONT_HEADING, m->heading_src == SRC_NONE ? t->muted : t->accent, dir);
    text(c, x + PAD + 2 + tw(c, FONT_HEADING, dir) + 8, y + 80, FONT_HEADING, t->text, deg);
    label(c, right - label_width(c, "HEADING") + 1, y + 60, t->muted, "HEADING");
    text_right(c, right, y + 78, FONT_SMALL, t->muted, note);
}

/* ---------------------------------------------------------------- address (bottom right) */

/* Nearest named road, then the barangay (or the nearest named place) with city and province, from
 * the address file (OpenStreetMap data). */
static void draw_address(const Canvas *c, const Theme *t, const DashModel *m)
{
    char place[ADDRESS_TEXT + 16], area[2 * ADDRESS_TEXT + 8], both[3 * ADDRESS_TEXT + 32];
    int x = SCREEN_W - EDGE - WIDE_W, y = LOW_Y, w = WIDE_W, sx = x + PAD + 2, max_w = w - 2 * PAD - 4;

    glass(c, t, x, y, w, LOW_H, RADIUS);
    icon(c, ICON_PIN, sx, y + 11, t->muted);
    label(c, sx + 21, y + 23, t->muted, "LOCATION");
    if (m->have_position && !m->gps_ok && m->address_state == ADDRESS_OK) {
        const char *note = "Last GPS position";
        if (sx + 21 + label_width(c, "LOCATION") + 6 + tw(c, FONT_SMALL, note) <= x + w - PAD)
            text_right(c, x + w - PAD - 2, y + 23, FONT_SMALL, t->text, note);
    }
    if (m->address_state == ADDRESS_NO_FILE) {
        centred(c, x, w, y + 52, FONT_HEADER, t->text, "No address file");
        centred(c, x, w, y + 72, FONT_SMALL, t->muted, "Copy luzon.addr into the Map folder");
        return;
    }
    if (m->address_state == ADDRESS_UNREADABLE) {
        centred(c, x, w, y + 52, FONT_HEADER, t->text, "Address file cannot be read");
        centred(c, x, w, y + 72, FONT_SMALL, t->muted, "Copy luzon.addr again");
        return;
    }
    if (m->address_state == ADDRESS_WAITING || !m->have_position) {
        centred(c, x, w, y + 60, FONT_HEADER, t->muted, "Waiting for GPS");
        return;
    }
    if (m->address_state == ADDRESS_OUTSIDE) {
        centred(c, x, w, y + 60, FONT_HEADER, t->muted, "Outside the address area");
        return;
    }
    if (m->addr_road[0])
        fitted(c, sx, y + 51, FONT_ADDR, t->text, max_w, m->addr_road);
    else
        fitted(c, sx, y + 51, FONT_HEADER, t->muted, max_w, "No named road nearby");

    place[0] = 0;
    if (m->addr_barangay[0]) {
        if (strncmp(m->addr_barangay, "Barangay", 8) == 0 || strncmp(m->addr_barangay, "Brgy", 4) == 0)
            snprintf(place, sizeof place, "%s", m->addr_barangay);
        else
            snprintf(place, sizeof place, "Brgy. %s", m->addr_barangay);
    } else if (m->addr_place[0]) {
        snprintf(place, sizeof place, "Near %s", m->addr_place);
    }
    if (m->addr_city[0] && m->addr_province[0] && strcmp(m->addr_city, m->addr_province) != 0)
        snprintf(area, sizeof area, "%s, %s", m->addr_city, m->addr_province);
    else
        snprintf(area, sizeof area, "%s", m->addr_city[0] ? m->addr_city : m->addr_province);
    /* one line: the place and the area; without the province when that is too wide */
    snprintf(both, sizeof both, "%s%s%s", place, place[0] && area[0] ? DOT : "", area);
    if (tw(c, FONT_TREND, both) > max_w && place[0] && m->addr_city[0])
        snprintf(both, sizeof both, "%s" DOT "%s", place, m->addr_city);
    if (both[0])
        fitted(c, sx, y + 72, FONT_TREND, t->muted, max_w, both);
}

/* ---------------------------------------------------------------- map */

/* The car: an arrow along the GPS course while moving, otherwise a dot; grey without GPS. */
static void draw_marker(const Canvas *c, const Theme *t, const DashModel *m, int cx, int cy)
{
    static const double arrow[4][2] = {{0, -17}, {13, 16}, {0, 8}, {-13, 16}};
    if (m->gps_ok && m->heading_src == SRC_GPS) {
        double a = m->heading_deg * 3.14159265358979 / 180.0, s = sin(a), co = cos(a);
        DPoint p[5];
        int i;
        for (i = 0; i < 4; i++) {
            p[i].x = cx + iround(arrow[i][0] * co - arrow[i][1] * s);
            p[i].y = cy + iround(arrow[i][0] * s + arrow[i][1] * co);
        }
        p[4] = p[0];
        c->polyline(c->ctx, p, 5, 4, t->outline); /* outline so it stands out on any map colour */
        c->polygon(c->ctx, p, 4, t->cool);
    } else {
        c->circle(c->ctx, cx, cy, 8, m->gps_ok ? t->cool : t->muted, t->outline, 3);
    }
}

/* Scale bar of a round distance and at most 64 px, on a small panel at the bottom. */
static void draw_scale(const Canvas *c, const Theme *t, const DashModel *m)
{
    static const int nice[] = {10, 20, 50, 100, 200, 300, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};
    double mpp = map_metres_per_pixel(m->lat, m->map_zoom);
    char buf[16];
    int i, len = 0, metres = 0, x = LEFT_X + WIDE_W + EDGE, y = BOTTOM - 38, w;

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
    w = 12 + len + 6 + tw(c, FONT_SMALL, buf) + 12;
    glass(c, t, x, y, w, 32, 16);
    c->fill_rect(c->ctx, x + 12, y + 12, 2, 7, t->text);         /* pixel bar: |____| */
    c->fill_rect(c->ctx, x + 12, y + 17, len, 2, t->text);
    c->fill_rect(c->ctx, x + 12 + len - 2, y + 12, 2, 7, t->text);
    text(c, x + 12 + len + 6, y + 21, FONT_SMALL, t->text, buf);
}

static void zoom_button(int index, Box *z)
{
    z->w = ZOOM_W / 2;
    z->h = ZOOM_H;
    z->x = ZOOM_X + (index == 0 ? ZOOM_W / 2 : 0); /* index 0 zoom in (right), 1 zoom out (left) */
    z->y = ZOOM_Y;
}

static void draw_map_controls(const Canvas *c, const Theme *t, const DashModel *m)
{
    /* credits the map's licences ask for: OpenMapTiles (CC-BY 4.0), OpenStreetMap data (ODbL) */
    static const char *const credit = "\xC2\xA9 OpenMapTiles \xC2\xA9 OpenStreetMap";
    int nx = RIGHT_X - EDGE - 44, cw = tw(c, FONT_SMALL, credit), i;

    glass(c, t, nx, TOP, 44, 44, 22);                       /* north up */
    icon(c, ICON_NORTH, nx + 10, TOP + 8, t->graph);
    centred(c, nx, 44, TOP + 35, FONT_TAPE, t->text, "N");

    glass(c, t, CAR_X - cw / 2 - 8, ZOOM_Y - 25, cw + 16, 19, 9);
    text(c, CAR_X - cw / 2, ZOOM_Y - 11, FONT_SMALL, t->muted, credit);
    draw_scale(c, t, m);

    glass(c, t, ZOOM_X, ZOOM_Y, ZOOM_W, ZOOM_H, ZOOM_H / 2);
    c->fill_rect(c->ctx, ZOOM_X + ZOOM_W / 2, ZOOM_Y + 12, 1, ZOOM_H - 24, t->border);
    for (i = 0; i < 2; i++) {
        Box z;
        int enabled = i == 0 ? m->map_zoom < m->map_max_zoom : m->map_zoom > m->map_min_zoom;
        zoom_button(i, &z);
        icon(c, i == 0 ? ICON_PLUS : ICON_MINUS, z.x + (z.w - 18) / 2, z.y + (z.h - 18) / 2,
             enabled ? t->text : t->line);
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

/* A message on a small panel in the middle of the map area. */
static void message(const Canvas *c, const Theme *t, int cy, const char *line1, const char *line2)
{
    int w1 = tw(c, FONT_HEADER, line1), w2 = line2 ? tw(c, FONT_SMALL, line2) : 0;
    int w = (w1 > w2 ? w1 : w2) + 32, h = line2 ? 56 : 36, x = CAR_X - w / 2, y = cy - h / 2;
    glass(c, t, x, y, w, h, 12);
    text(c, CAR_X - w1 / 2, line2 ? y + 24 : y + 23, FONT_HEADER, t->text, line1);
    if (line2)
        text(c, CAR_X - w2 / 2, y + 44, FONT_SMALL, t->muted, line2);
}

void render_dashboard(const Canvas *c, const DashModel *m)
{
    const Theme *t = m->night ? &g_night : &g_day;
    int files = c->map != NULL && m->map_max_zoom >= 0, shown = files && m->have_position, result = MAP_NO_FILES;

    if (shown) { /* the whole screen, centred so the car sits in the middle below the header */
        double px, py;
        map_world_pixel(m->lat, m->lon, m->map_zoom, &px, &py);
        result = c->map(c->ctx, 0, 0, SCREEN_W, SCREEN_H, m->map_zoom, px, py - (CAR_Y - SCREEN_H / 2), m->night);
        if (result == MAP_NO_FILES)
            files = shown = 0;
    }
    if (!shown)
        c->fill_rect(c->ctx, 0, 0, SCREEN_W, SCREEN_H, t->bg);
    else
        draw_marker(c, t, m, CAR_X, CAR_Y);

    draw_header(c, t, m);
    draw_speed(c, t, m);
    draw_elevation(c, t, m);
    {
        int h = (COL_BOTTOM - TOP - 2 * GAP) / 3, y2 = TOP + h + GAP, y3 = y2 + h + GAP;
        draw_outside(c, t, m, TOP, h);
        draw_pressure(c, t, m, y2, h);
        draw_battery(c, t, m, y3, COL_BOTTOM - y3);
    }
    draw_heading(c, t, m);
    draw_address(c, t, m);
    if (shown)
        draw_map_controls(c, t, m);
    if (!files)
        message(c, t, CAR_Y - 20, "No map files", "Copy the Map folder to the SD card");
    else if (!m->have_position)
        message(c, t, CAR_Y - 20, "Waiting for GPS", NULL);
    else if (result != MAP_SHOWN)
        message(c, t, CAR_Y + 56, "No map for this area", NULL);
}

/* ---------------------------------------------------------------- car info page */

/* Tile icons, 24 px, on the same pixel grid as the others. Each has a muted part and, where the
 * tile is a measurement, an accent part showing what is measured (the water line, the gap under
 * the car, the slope). The car seen from the side, 20 x 13 (CAR_TOP without wheels for wading),
 * and a shorter one, 16 x 11, for the three angles. */
#define CAR_TOP(dx, dy) \
    4, (dx) + 4, (dy), (dx) + 12, (dy), (dx) + 13, (dy) + 2, (dx) + 3, (dy) + 2, \
    4, (dx) + 3, (dy) + 2, (dx) + 5, (dy) + 2, (dx) + 5, (dy) + 4, (dx) + 2, (dy) + 4, \
    4, (dx) + 11, (dy) + 2, (dx) + 13, (dy) + 2, (dx) + 14, (dy) + 4, (dx) + 11, (dy) + 4, \
    R((dx), (dy) + 4, 20, 5)
#define CAR_WHEELS(dx, dy) R((dx) + 2, (dy) + 9, 4, 4), R((dx) + 14, (dy) + 9, 4, 4)
#define SMALL_CAR(dx, dy) \
    4, (dx) + 3, (dy) + 1, (dx) + 10, (dy) + 1, (dx) + 11, (dy) + 3, (dx) + 2, (dy) + 3, \
    4, (dx) + 2, (dy) + 3, (dx) + 4, (dy) + 3, (dx) + 4, (dy) + 5, (dx) + 1, (dy) + 5, \
    4, (dx) + 9, (dy) + 3, (dx) + 11, (dy) + 3, (dx) + 12, (dy) + 5, (dx) + 9, (dy) + 5, \
    R((dx), (dy) + 5, 16, 4), R((dx) + 1, (dy) + 9, 4, 3), R((dx) + 11, (dy) + 9, 4, 3)
#define WAVE(dy) /* a zigzag band 3 px tall across 24 px, in two halves */ \
    14, 0, (dy) + 2, 2, (dy), 4, (dy) + 2, 6, (dy), 8, (dy) + 2, 10, (dy), 12, (dy) + 2, \
        12, (dy) + 5, 10, (dy) + 3, 8, (dy) + 5, 6, (dy) + 3, 4, (dy) + 5, 2, (dy) + 3, 0, (dy) + 5, \
    14, 12, (dy) + 2, 14, (dy), 16, (dy) + 2, 18, (dy), 20, (dy) + 2, 22, (dy), 24, (dy) + 2, \
        24, (dy) + 5, 22, (dy) + 3, 20, (dy) + 5, 18, (dy) + 3, 16, (dy) + 5, 14, (dy) + 3, 12, (dy) + 5

static const signed char ci_plate[] = {R(1, 6, 22, 2), R(1, 16, 22, 2), R(1, 8, 2, 8), R(21, 8, 2, 8),
                                       R(5, 10, 6, 4), R(13, 10, 6, 4), 0};
static const signed char ci_fuel[] = {R(3, 3, 10, 2), R(3, 5, 2, 5), R(11, 5, 2, 5), R(3, 10, 10, 11), R(1, 21, 14, 2),
                                      6, 13, 6, 19, 6, 19, 18, 17, 18, 17, 8, 13, 8, 0};
static const signed char ci_tank[] = {R(3, 9, 18, 13), 8, 5, 3, 13, 3, 13, 9, 11, 9, 11, 5, 7, 5, 7, 9, 5, 9,
                                      4, 15, 7, 18, 4, 20, 6, 17, 9, 0};
static const signed char ci_engine[] = {R(6, 8, 12, 3), R(6, 16, 12, 3), R(6, 11, 3, 5), R(15, 11, 3, 5),
                                        8, 8, 4, 16, 4, 16, 6, 13, 6, 13, 8, 11, 8, 11, 6, 8, 6,
                                        8, 2, 11, 4, 11, 4, 12, 6, 12, 6, 15, 4, 15, 4, 16, 2, 16,
                                        8, 18, 12, 20, 12, 20, 10, 22, 10, 22, 17, 20, 17, 20, 15, 18, 15, 0};
static const signed char ci_wading[] = {CAR_TOP(2, 2), 0};
static const signed char ci_wading_a[] = {WAVE(13), WAVE(19), 0};
static const signed char ci_ground[] = {CAR_TOP(2, 1), CAR_WHEELS(2, 1), R(0, 21, 24, 2), 0};
static const signed char ci_ground_a[] = {12, 10, 12, 14, 12, 14, 14, 13, 14, 13, 17, 14, 17, 14, 19, 10, 19, 10, 17,
                                          11, 17, 11, 14, 10, 14, 0};
static const signed char ci_height[] = {CAR_TOP(2, 11), CAR_WHEELS(2, 11), 0};
static const signed char ci_height_a[] = {R(0, 1, 24, 2), 3, 8, 5, 16, 5, 12, 9, 0};
static const signed char ci_width[] = {4, 5, 4, 19, 4, 20, 6, 4, 6,   4, 4, 6, 7, 6, 7, 8, 3, 8,  /* front view */
                                       4, 17, 6, 20, 6, 21, 8, 17, 8, R(3, 8, 18, 2), R(3, 10, 2, 2), R(8, 10, 8, 2),
                                       R(19, 10, 2, 2), R(3, 12, 18, 3), R(5, 15, 4, 2), R(15, 15, 4, 2), 0};
static const signed char ci_width_a[] = {12, 1, 19, 3, 19, 3, 21, 21, 21, 21, 19, 23, 19, 23, 24, 21, 24, 21, 23,
                                         3, 23, 3, 24, 1, 24, 0};
static const signed char ci_approach[] = {SMALL_CAR(0, 8), R(0, 20, 16, 2), 0};
static const signed char ci_approach_a[] = {3, 18, 22, 24, 16, 24, 22, 0};
static const signed char ci_departure[] = {SMALL_CAR(8, 8), R(8, 20, 16, 2), 0};
static const signed char ci_departure_a[] = {3, 0, 16, 6, 22, 0, 22, 0};
static const signed char ci_breakover[] = {SMALL_CAR(4, 4), 0};
static const signed char ci_breakover_a[] = {6, 0, 21, 12, 15, 24, 21, 24, 23, 12, 17, 0, 23, 0};
static const signed char ci_turning[] = {12, 5, 22, 5, 9, 9, 5, 15, 5, 19, 9, 19, 14, 17, 14, 17, 10, 14, 7, 10, 7,
                                         7, 10, 7, 22, 3, 14, 14, 22, 14, 18, 18, 0};
static const signed char ci_tyre[] = {   /* a tyre ring in eight pieces, and the hub */
    4, 8, 2, 16, 2, 15, 6, 9, 6,        4, 16, 2, 22, 8, 18, 9, 15, 6,
    4, 22, 8, 22, 16, 18, 15, 18, 9,    4, 22, 16, 16, 22, 15, 18, 18, 15,
    4, 16, 22, 8, 22, 9, 18, 15, 18,    4, 8, 22, 2, 16, 6, 15, 9, 18,
    4, 2, 16, 2, 8, 6, 9, 6, 15,        4, 2, 8, 8, 2, 9, 6, 6, 9,
    R(10, 10, 4, 4), 0};
static const signed char ci_pressure[] = {   /* a dial in eight pieces on a short stem */
    4, 8, 2, 16, 2, 15, 4, 9, 4,        4, 16, 2, 21, 7, 19, 8, 15, 4,
    4, 21, 7, 21, 15, 19, 14, 19, 8,    4, 21, 15, 16, 20, 15, 18, 19, 14,
    4, 16, 20, 8, 20, 9, 18, 15, 18,    4, 8, 20, 3, 15, 5, 14, 9, 18,
    4, 3, 15, 3, 7, 5, 8, 5, 14,        4, 3, 7, 8, 2, 9, 4, 5, 8,
    R(11, 20, 2, 3), 0};
static const signed char ci_pressure_a[] = {4, 10, 12, 15, 7, 17, 9, 12, 14, 0};
static const signed char ci_drive[] = {R(3, 2, 4, 6), R(17, 2, 4, 6), R(7, 4, 10, 2), 0};   /* front axle */
static const signed char ci_drive_a[] = {R(3, 16, 4, 6), R(17, 16, 4, 6), R(7, 18, 10, 2),  /* driven rear axle */
                                         R(11, 8, 2, 10), R(10, 16, 4, 6), 0};
static const signed char ci_weight[] = {R(8, 2, 8, 2), R(8, 4, 2, 4), R(14, 4, 2, 4), R(8, 8, 8, 2),
                                        6, 6, 10, 18, 10, 21, 16, 21, 22, 3, 22, 3, 16, 0};

/* The car: 2016 Mitsubishi Montero Sport (third generation), 2.4 diesel, rear-wheel drive (4x2).
 * Figures from Mitsubishi's published specifications for this model (Australian Pajero Sport data
 * where the Philippine sheet was not available); tyre pressure and gross weight are worth checking
 * against the door-pillar sticker and the registration papers. */
#define PLATE_NUMBER "NDE 4391"

typedef struct {
    const signed char *icon, *accent;  /* accent: the part that is measured, or NULL */
    const char *label, *value, *unit, *sub;
    int plate;                         /* value drawn as a number plate */
} InfoTile;

static const InfoTile g_tiles[16] = {
    {ci_plate, NULL, "PLATE NUMBER", PLATE_NUMBER, NULL, NULL, 1},
    {ci_fuel, NULL, "FUEL TYPE", "Diesel", NULL, "Euro 4 grade", 0},
    {ci_tank, NULL, "FUEL TANK", "68", "L", "Full capacity", 0},
    {ci_engine, NULL, "ENGINE", "2.4", "L turbo", "181 PS" DOT "430 Nm", 0},
    {ci_wading, ci_wading_a, "WATER WADING", "700", "mm", "Maximum, at walking pace", 0},
    {ci_ground, ci_ground_a, "GROUND CLEARANCE", "218", "mm", "Unladen", 0},
    {ci_height, ci_height_a, "HEIGHT CLEARANCE", "1,805", "mm", "Overall height, unladen", 0},
    {ci_width, ci_width_a, "WIDTH", "1,815", "mm", "Body, without mirrors", 0},
    {ci_approach, ci_approach_a, "APPROACH ANGLE", "30" DEG, NULL, "Front overhang", 0},
    {ci_departure, ci_departure_a, "DEPARTURE ANGLE", "24.2" DEG, NULL, "Rear overhang", 0},
    {ci_breakover, ci_breakover_a, "BREAKOVER ANGLE", "23.1" DEG, NULL, "Crest between the axles", 0},
    {ci_turning, NULL, "TURNING CIRCLE", "11.2", "m", "Kerb to kerb", 0},
    {ci_tyre, NULL, "TYRE SIZE", "265/60 R18", NULL, "18 in alloy wheels", 0},
    {ci_pressure, ci_pressure_a, "TYRE PRESSURE", "29 / 32", "psi", "Cold, front / rear", 0},
    {ci_drive, ci_drive_a, "DRIVE", "4x2", NULL, "Rear wheels, 8-speed auto", 0},
    {ci_weight, NULL, "GROSS WEIGHT", "2,710", "kg", "Fully loaded maximum", 0},
};

/* 4 x 4 tiles below the header, 10 px from the screen edges, 8 px apart. */
#define TILE_W ((SCREEN_W - 2 * EDGE - 3 * GAP) / 4)   /* 189 */
#define TILE_H ((BOTTOM - TOP - 3 * GAP) / 4)          /* 99 */
#define BACK_END 90                                    /* the Back button: x 0 to here */

/* The number plate: letters spaced 2 px apart on a light (by night dark) plate with a 2 px border. */
static void draw_plate(const Canvas *c, const Theme *t, int x, int y, const char *s)
{
    char ch[2] = {0, 0};
    const char *q;
    int w = 0, h = 44;

    for (q = s; *q; q++) {
        ch[0] = *q;
        w += tw(c, FONT_TILE, ch) + (q[1] ? 2 : 0);
    }
    c->round_rect(c->ctx, x, y, w + 28, h, 6, t->plate, t->text);
    c->round_rect(c->ctx, x + 1, y + 1, w + 26, h - 2, 5, NO_FILL, t->text);
    x += 14;
    for (q = s; *q; q++) {
        ch[0] = *q;
        text(c, x, y + 32, FONT_TILE, t->text, ch);
        x += tw(c, FONT_TILE, ch) + 2;
    }
}

static void draw_tile(const Canvas *c, const Theme *t, const InfoTile *it, int x, int y)
{
    glass(c, t, x, y, TILE_W, TILE_H, RADIUS);
    icon_shape(c, it->icon, x + 12, y + 9, t->muted);
    if (it->accent)
        icon_shape(c, it->accent, x + 12, y + 9, t->accent);
    label(c, x + 44, y + 25, t->muted, it->label);
    if (it->plate) {
        draw_plate(c, t, x + 12, y + TILE_H - 10 - 44, it->value);
        return;
    }
    value_unit(c, t, x + 12, y + 66, FONT_TILE, it->value, FONT_UNIT, it->unit, 3);
    fitted(c, x + 12, y + 86, FONT_SMALL, t->muted, TILE_W - 24, it->sub);
}

/* Header: Back, the car's make, model and year; the time on the right. */
static void draw_carinfo_header(const Canvas *c, const Theme *t, const DashModel *m)
{
    char buf[16];
    int x;

    header_bar(c, t);
    icon_shape(c, g_back, 12, 10, t->text);
    text(c, 36, 25, FONT_HEADER, t->text, "Back");
    x = 36 + tw(c, FONT_HEADER, "Back") + 12;
    c->fill_rect(c->ctx, x, 10, 1, 20, t->line);
    x += 14;
    text(c, x, 30, FONT_CLOCK, t->text, "Mitsubishi Montero Sport");
    x += tw(c, FONT_CLOCK, "Mitsubishi Montero Sport") + 10;
    text(c, x, 30, FONT_HEADER, t->muted, "2016" DOT "2.4 diesel 4x2");

    if (m->clock_valid)
        fmt_time12(buf, sizeof buf, m->hour, m->minute);
    else
        strcpy(buf, "--:--");
    x = SCREEN_W - 16 - tw(c, FONT_HEADER, buf);
    text(c, x, 25, FONT_HEADER, t->muted, buf);
    icon_shape(c, g_clock, x - 26, 10, t->muted);
}

void render_carinfo(const Canvas *c, const DashModel *m)
{
    const Theme *t = m->night ? &g_night : &g_day;
    int i;

    c->fill_rect(c->ctx, 0, 0, SCREEN_W, SCREEN_H, t->bg);
    draw_carinfo_header(c, t, m);
    for (i = 0; i < 16; i++)
        draw_tile(c, t, &g_tiles[i], EDGE + (i % 4) * (TILE_W + GAP), TOP + (i / 4) * (TILE_H + GAP));
}

int render_carinfo_back_hit(int x, int y)
{
    return x >= 0 && x < BACK_END && y >= 0 && y < HEADER_H + 4;
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
    c->round_rect(c->ctx, TZ_MINUS_X, TZ_Y, 56, 56, 10, t->button, t->border);
    c->round_rect(c->ctx, TZ_PLUS_X, TZ_Y, 56, 56, 10, t->button, t->border);
    c->fill_rect(c->ctx, TZ_MINUS_X + 18, mid - 1, 20, 3, t->text);
    c->fill_rect(c->ctx, TZ_PLUS_X + 18, mid - 1, 20, 3, t->text);
    c->fill_rect(c->ctx, TZ_PLUS_X + 27, mid - 10, 3, 20, t->text);
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

    c->round_rect(c->ctx, MENU_X, MENU_Y, MENU_W, MENU_H, 12, t->solid, t->border);
    text(c, MENU_X + 20, MENU_Y + 40, FONT_MENU, t->text, "Dashboard menu");
    snprintf(buf, sizeof buf, "Montero dashboard %s", version);
    text(c, MENU_X + 20, MENU_Y + 64, FONT_SMALL, t->muted, buf);
    draw_tz_row(c, t, m);
    for (i = 0; i < n; i++) {
        int back = items[i] == MENU_BACK;
        const char *name = names[items[i]];
        menu_button(i, &b);
        c->round_rect(c->ctx, b.x, b.y, b.w, b.h, 10, back ? t->accent : t->button, back ? t->accent : t->border);
        text(c, b.x + (b.w - tw(c, FONT_MENU, name)) / 2, b.y + 35, FONT_MENU, back ? t->on_accent : t->text, name);
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
    return x >= 0 && x < 120 && y >= 0 && y < HEADER_H; /* "12:45 PM" is about 100 px wide */
}
