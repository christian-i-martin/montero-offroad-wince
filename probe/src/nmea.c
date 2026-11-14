#include "nmea.h"

#include <string.h>

#define MAX_FIELDS 20

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Fixed-point parse of "123.45" into value * 10^decimals, rounding toward zero. */
static int parse_fixed(const char *s, int decimals, int *out)
{
    int sign = 1, value = 0, digits = 0, frac = -1;
    if (*s == '-') { sign = -1; s++; }
    for (; *s; s++) {
        if (*s == '.') { if (frac >= 0) return 0; frac = 0; continue; }
        if (*s < '0' || *s > '9') return 0;
        if (frac >= 0) { if (frac >= decimals) continue; frac++; }
        value = value * 10 + (*s - '0');
        digits++;
    }
    if (!digits) return 0;
    for (frac = frac < 0 ? 0 : frac; frac < decimals; frac++)
        value *= 10;
    *out = sign * value;
    return 1;
}

static int has_token(const char *list, const char *token, size_t n)
{
    while (*list) {
        const char *end = strchr(list, ' ');
        size_t l = end ? (size_t)(end - list) : strlen(list);
        if (l == n && strncmp(list, token, n) == 0)
            return 1;
        if (!end)
            break;
        list = end + 1;
    }
    return 0;
}

static int two_digits(const char *s)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9')
        return -1;
    return (s[0] - '0') * 10 + (s[1] - '0');
}

/* "4807.0380" + "N" (degrees and minutes) -> micro-degrees, south and west negative. */
static int parse_coordinate(const char *s, const char *hemisphere, int *out)
{
    int value, degrees, minutes_e4;
    if (!parse_fixed(s, 4, &value) || value < 0 || (hemisphere[0] != 'N' && hemisphere[0] != 'S' &&
                                                    hemisphere[0] != 'E' && hemisphere[0] != 'W'))
        return 0;
    degrees = value / 1000000;
    minutes_e4 = value % 1000000;
    if (minutes_e4 >= 600000 || degrees > 180)
        return 0;
    value = degrees * 1000000 + (int)((minutes_e4 * 100LL) / 60);
    *out = (hemisphere[0] == 'S' || hemisphere[0] == 'W') ? -value : value;
    return 1;
}

static void remember_type(NmeaState *st, const char *type)
{
    size_t used = strlen(st->seen), len = strlen(type);
    if (len == 0 || len > 9 || has_token(st->seen, type, len) || used + len + 2 > sizeof st->seen)
        return;
    if (used)
        st->seen[used++] = ' ';
    memcpy(st->seen + used, type, len + 1);
}

int nmea_parse_sentence(NmeaState *st, const char *sentence)
{
    char buf[128];
    char *fields[MAX_FIELDS];
    int count = 0, value;
    size_t len = strlen(sentence);
    char *star, *p;

    if (len < 7 || len >= sizeof buf || sentence[0] != '$')
        return 0;
    memcpy(buf, sentence + 1, len); /* includes the terminating NUL */
    star = strrchr(buf, '*');
    if (star) {
        unsigned sum = 0;
        int hi = hex_value(star[1]), lo = star[1] ? hex_value(star[2]) : -1;
        for (p = buf; p < star; p++)
            sum ^= (unsigned char)*p;
        if (hi < 0 || lo < 0 || (unsigned)(hi * 16 + lo) != sum) {
            st->bad_checksums++;
            return 0;
        }
        *star = '\0';
    }
    for (p = buf; count < MAX_FIELDS; ) {
        fields[count++] = p;
        p = strchr(p, ',');
        if (!p) break;
        *p++ = '\0';
    }
    st->sentences++;
    remember_type(st, fields[0]);
    if (strlen(fields[0]) != 5)
        return 0;

    if (strcmp(fields[0] + 2, "RMC") == 0 && count >= 9) {
        int h = two_digits(fields[1]), m = h < 0 ? -1 : two_digits(fields[1] + 2),
            s = m < 0 ? -1 : two_digits(fields[1] + 4), lat, lon;
        st->have_rmc = 1;
        st->rmc_count++;
        st->valid = fields[2][0] == 'A';
        if (parse_fixed(fields[7], 1, &value))     /* knots * 10 */
            st->speed_dkmh = (value * 1852 + 500) / 1000;
        if (parse_fixed(fields[8], 1, &value))
            st->course_ddeg = value % 3600;
        if (s >= 0 && h < 24 && m < 60 && s < 61) {
            st->have_time = 1;
            st->utc_seconds = h * 3600 + m * 60 + (s > 59 ? 59 : s);
        }
        if (count >= 10 && strlen(fields[9]) == 6) {
            int d = two_digits(fields[9]), mo = two_digits(fields[9] + 2), y = two_digits(fields[9] + 4);
            if (d >= 1 && d <= 31 && mo >= 1 && mo <= 12 && y >= 0) {
                st->have_date = 1;
                st->day = d;
                st->month = mo;
                st->year = y < 80 ? 2000 + y : 1900 + y;
            }
        }
        if (st->valid && parse_coordinate(fields[3], fields[4], &lat) && parse_coordinate(fields[5], fields[6], &lon) &&
            lat >= -90000000 && lat <= 90000000) {
            st->have_position = 1;
            st->lat_e6 = lat;
            st->lon_e6 = lon;
        }
        return 1;
    }
    if (strcmp(fields[0] + 2, "GGA") == 0 && count >= 10) {
        st->have_gga = 1;
        st->fix_quality = fields[6][0] ? fields[6][0] - '0' : 0;
        if (parse_fixed(fields[7], 0, &value)) st->satellites = value;
        if (parse_fixed(fields[9], 1, &value)) st->altitude_dm = value;
        return 1;
    }
    return 0;
}

void nmea_feed(NmeaState *st, char *line, int *used, int capacity, const char *data, int length)
{
    int i;
    for (i = 0; i < length; i++) {
        char c = data[i];
        if (c == '$')
            *used = 0;
        if (c == '\r' || c == '\n') {
            if (*used > 0) {
                line[*used] = '\0';
                nmea_parse_sentence(st, line);
            }
            *used = 0;
        } else if (*used < capacity - 1) {
            line[(*used)++] = c;
        } else {
            *used = 0; /* overlong line: drop it */
        }
    }
}
