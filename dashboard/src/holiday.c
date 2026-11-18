#include "holiday.h"

#include "sun.h"

#include <math.h>

#define BOX_E5 300000L        /* towns further than 3 degrees are skipped (build_holidays.py) */
#define COS_SCALE 4096
#define NEAR_LIMIT 3.0        /* radii */
#define M_PER_E5 1.1132

/* Splits a line in place at tabs; returns the number of fields (at most max). */
static int fields(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (; *line && n < max; line++)
        if (*line == '\t') {
            *line = 0;
            f[n++] = line + 1;
        }
    return n;
}

static int number(const char *s, long *out)
{
    long v = 0, sign = 1;
    int digits = 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    for (; *s >= '0' && *s <= '9'; s++, digits++)
        v = v * 10 + (*s - '0');
    *out = v * sign;
    return digits > 0 && *s == 0;
}

/* "YYYY-MM-DD" as days since 1970. */
static int date(const char *s, long *days)
{
    int i, y = 0, m = 0, d = 0;
    for (i = 0; i < 10; i++)
        if (s[i] == 0 || (i == 4 || i == 7 ? s[i] != '-' : s[i] < '0' || s[i] > '9'))
            return 0;
    if (s[10] != 0)
        return 0;
    for (i = 0; i < 4; i++)
        y = y * 10 + (s[i] - '0');
    m = (s[5] - '0') * 10 + (s[6] - '0');
    d = (s[8] - '0') * 10 + (s[9] - '0');
    if (m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    *days = sun_days_from_civil(y, m, d);
    return 1;
}

static int parse_line(HolidayList *h, char *line)
{
    char *f[8];
    long a, b, c, d;
    int n = fields(line, f, 8);

    if (line[0] == 0 || line[0] == '#')
        return 1;
    if (f[0][0] == 'V' && f[0][1] == 0 && n == 3)
        return date(f[1], &h->first_day) && date(f[2], &h->last_day);
    if (f[0][0] == 'P' && f[0][1] == 0 && n == 3) {
        if (!number(f[1], &a) || a != h->province_count || h->province_count >= HOLIDAY_MAX_PROVINCES)
            return 0;
        h->province[h->province_count++] = f[2];
        return 1;
    }
    if (f[0][0] == 'T' && f[0][1] == 0 && n == 6) {
        HolidayTown *t = &h->town[h->town_count];
        if (h->town_count >= HOLIDAY_MAX_TOWNS || !number(f[1], &a) || a < 0 || a >= h->province_count ||
            !number(f[2], &b) || !number(f[3], &c) || !number(f[4], &d) || d <= 0)
            return 0;
        t->province = (int)a;
        t->lat_e5 = (int)b;
        t->lon_e5 = (int)c;
        t->radius_m = (int)d;
        t->name = f[5];
        h->town_count++;
        return 1;
    }
    if (f[0][0] == 'H' && f[0][1] == 0 && (n == 5 || n == 6)) {
        HolidayEntry *e = &h->entry[h->count];
        if (h->count >= HOLIDAY_MAX_ENTRIES || !date(f[1], &e->first) || !date(f[2], &e->last) || e->last < e->first ||
            (f[3][0] != HOLIDAY_REGULAR && f[3][0] != HOLIDAY_SPECIAL && f[3][0] != HOLIDAY_WORKING) || f[3][1] != 0 ||
            f[4][0] == 0)
            return 0;
        e->kind = f[3][0];
        e->name = f[4];
        e->places = n == 6 ? f[5] : "";
        h->count++;
        return 1;
    }
    return 0;
}

int holiday_parse(HolidayList *h, char *text, int size)
{
    int i, start = 0;
    h->first_day = 1;
    h->last_day = 0;
    h->province_count = h->town_count = h->count = h->bad_lines = 0;
    for (i = 0; i <= size; i++) {
        if (i < size && text[i] != '\n' && text[i] != '\r' && text[i] != 0)
            continue;
        if (i < size)
            text[i] = 0;
        if (i > start && !parse_line(h, text + start))
            h->bad_lines++;
        start = i + 1;
    }
    return h->count;
}

int holiday_covers(const HolidayList *h, int year, int month, int day)
{
    long d = sun_days_from_civil(year, month, day);
    return d >= h->first_day && d <= h->last_day;
}

int holiday_locate(const HolidayList *h, int lat_e5, int lon_e5)
{
    int i, best = -1, k = (int)floor(cos(lat_e5 / 1e5 * 3.14159265358979323846 / 180.0) * COS_SCALE + 0.5);
    double best_ratio = 0.0, best_d2 = 0.0;

    for (i = 0; i < h->town_count; i++) {
        const HolidayTown *t = &h->town[i];
        long dlat = (long)lat_e5 - t->lat_e5, dlon = (long)lon_e5 - t->lon_e5;
        double dx, dy, d2, ratio;
        if (dlat > BOX_E5 || dlat < -BOX_E5 || dlon > BOX_E5 || dlon < -BOX_E5)
            continue;
        dx = (double)dlon * k;
        dy = (double)dlat * COS_SCALE;
        d2 = dx * dx + dy * dy;
        ratio = d2 / ((double)t->radius_m * (double)t->radius_m);
        if (best < 0 || ratio < best_ratio) {
            best = i;
            best_ratio = ratio;
            best_d2 = d2;
        }
    }
    if (best >= 0) {
        double limit = NEAR_LIMIT * h->town[best].radius_m / M_PER_E5 * COS_SCALE;
        if (best_d2 > limit * limit)
            return -1;
    }
    return best;
}

/* 1 when the place list holds token kind ('P' or 'T') with number n. */
static int has_place(const char *places, int kind, int n)
{
    while (*places) {
        long v = 0;
        int k, digits = 0;
        while (*places == ' ')
            places++;
        k = *places;
        if (k == 0)
            break;
        places++;
        for (; *places >= '0' && *places <= '9'; places++, digits++)
            v = v * 10 + (*places - '0');
        if (digits && k == kind && v == n)
            return 1;
        while (*places && *places != ' ')
            places++;
    }
    return 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void copy(char *dst, const char *src, int size)
{
    int i;
    for (i = 0; i < size - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

int holiday_today(const HolidayList *h, int year, int month, int day, int town, HolidayRow *rows, int max)
{
    long d = sun_days_from_civil(year, month, day);
    int level[HOLIDAY_MAX_ENTRIES / 8], found[HOLIDAY_MAX_ENTRIES / 8], n = 0, i, j, lv, out = 0;
    int province = town >= 0 && town < h->town_count ? h->town[town].province : -1;

    if (province < 0)
        town = -1;
    /* the holidays of the day, and the level each one applies at: 0 country, 1 province, 2 town */
    for (i = 0; i < h->count && n < (int)(sizeof found / sizeof found[0]); i++) {
        const HolidayEntry *e = &h->entry[i];
        if (d < e->first || d > e->last)
            continue;
        if (e->places[0] == 0)
            lv = 0;
        else if (town >= 0 && has_place(e->places, 'T', town))
            lv = 2;
        else if (province >= 0 && has_place(e->places, 'P', province))
            lv = 1;
        else
            continue;
        found[n] = i;
        level[n++] = lv;
    }
    /* each name once, at its most specific level; country first, then province, then town */
    for (lv = 0; lv <= 2; lv++)
        for (i = 0; i < n && out < max; i++) {
            const HolidayEntry *e = &h->entry[found[i]];
            int keep = level[i] == lv;
            for (j = 0; j < n && keep; j++)
                if (j != i && same(h->entry[found[j]].name, e->name) &&
                    (level[j] > lv || (level[j] == lv && j < i)))
                    keep = 0;
            if (!keep)
                continue;
            copy(rows[out].name, e->name, HOLIDAY_NAME_MAX);
            rows[out].kind = e->kind;
            rows[out].local = lv > 0;
            copy(rows[out].place, lv == 2 ? h->town[town].name : lv == 1 ? h->province[province] : "", HOLIDAY_NAME_MAX);
            out++;
        }
    return out;
}
