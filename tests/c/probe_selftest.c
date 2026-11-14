/* Self-test for the probe's portable parts: bus ring reader and NMEA parser.
 * Prints one line per failed check and exits non-zero if any failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "busring.h"
#include "nmea.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- ring helpers: build records exactly as SkCom.dll's writer does */
#define SIZE 0x400
static unsigned char shm[SIZE];

static void wr32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void ring_init(void)
{
    memset(shm, 0xEE, sizeof shm);
    wr32(shm, SIZE); wr32(shm + 4, 0x10); wr32(shm + 8, 0x10); wr32(shm + 12, SIZE);
}

static unsigned ring_write(const unsigned char *msg, unsigned len, int complete)
{
    unsigned w = shm[8] | shm[9] << 8 | shm[10] << 16 | (unsigned)shm[11] << 24, sum = 0, i;
    if (w + len + 8 > SIZE) w = 0x10;
    wr32(shm + 8, w + len + 8);
    if (!complete) return w; /* reserved, not yet copied */
    memcpy(shm + w, "msg", 4);
    shm[w + 4] = len & 0xFF; shm[w + 5] = len >> 8;
    memcpy(shm + w + 6, msg, len);
    for (i = 0; i < len; i++) sum = (sum + msg[i]) & 0xFFFF;
    shm[w + 6 + len] = sum & 0xFF; shm[w + 7 + len] = sum >> 8;
    return w;
}

static unsigned char rmsg[64];
static unsigned make_rmsg(int what, int sender, unsigned extra)
{
    wr32(rmsg, 0x47534D52); wr32(rmsg + 4, what); wr32(rmsg + 8, 0x14); wr32(rmsg + 12, 0); wr32(rmsg + 16, sender);
    memset(rmsg + 20, 0xAB, extra);
    return 20 + extra;
}

static int seen_whats[64], seen_count;
static void collect(void *ctx, unsigned off, const unsigned char *m, int len)
{
    BusHeader h;
    (void)ctx; (void)off;
    if (busring_message_header(m, len, &h) && seen_count < 64) seen_whats[seen_count++] = h.what;
}

static void test_ring(void)
{
    BusRing r;
    int i, n;

    ring_init();
    CHECK(busring_header_ok(shm, SIZE));
    memset(&r, 0, sizeof r);
    seen_count = 0;
    n = busring_poll(&r, shm, SIZE, 100, collect, NULL); /* cursor 0: starts at write, nothing new */
    CHECK(n == 0);
    ring_write(rmsg, make_rmsg(0x4A, 0, 4), 1);
    ring_write(rmsg, make_rmsg(0x6D1, 0x14, 0), 1);
    n = busring_poll(&r, shm, SIZE, 100, collect, NULL);
    CHECK(n == 2 && seen_whats[0] == 0x4A && seen_whats[1] == 0x6D1);

    /* backlog read from the start of the lap */
    memset(&r, 0, sizeof r); r.cursor = 0x10; seen_count = 0;
    CHECK(busring_poll(&r, shm, SIZE, 100, collect, NULL) == 2);

    /* a reserved but not yet copied record makes the reader wait, then it continues */
    seen_count = 0;
    {
        unsigned len = make_rmsg(0x77, 0, 0), w = ring_write(rmsg, len, 0);
        CHECK(busring_poll(&r, shm, SIZE, 100, collect, NULL) == 0);
        wr32(shm + 8, w); ring_write(rmsg, len, 1); /* writer finishes the same record */
        CHECK(busring_poll(&r, shm, SIZE, 100, collect, NULL) == 1 && seen_whats[0] == 0x77);
    }

    /* wrap-around: fill the ring until the writer restarts at 0x10, reader keeps up */
    seen_count = 0;
    for (i = 0; i < 40; i++) {
        ring_write(rmsg, make_rmsg(0x100 + i, 0, 12), 1);
        if (i % 7 == 6) busring_poll(&r, shm, SIZE, 100, collect, NULL);
    }
    busring_poll(&r, shm, SIZE, 100, collect, NULL);
    CHECK(seen_count == 40);
    for (i = 0; i < seen_count && i < 40; i++) CHECK(seen_whats[i] == 0x100 + i);
    CHECK(r.resyncs == 0);

    /* garbage behind the write offset: the reader waits, then resyncs instead of looping */
    memset(shm + r.cursor, 0, 8);
    wr32(shm + 8, r.cursor + 40);
    for (i = 0; i < BUSRING_STALL_LIMIT; i++)
        CHECK(busring_poll(&r, shm, SIZE, 100, collect, NULL) == 0);
    CHECK(r.resyncs == 0);
    busring_poll(&r, shm, SIZE, 100, collect, NULL);
    CHECK(r.resyncs == 1);

    /* invalid header */
    wr32(shm + 4, 0);
    CHECK(busring_poll(&r, shm, SIZE, 100, collect, NULL) == -1);
    {
        BusHeader h;
        CHECK(!busring_message_header((const unsigned char *)"RMSX0000000000000000", 20, &h));
        CHECK(!busring_message_header(rmsg, 19, &h));
    }
}

static int parse_hex(const char *text, unsigned char *out, int max)
{
    int n = 0;
    unsigned v;
    while (*text && n < max) {
        if (*text == ' ') { text++; continue; }
        if (sscanf(text, "%2x", &v) != 1) return -1;
        out[n++] = (unsigned char)v;
        text += 2;
    }
    return n;
}

static void test_bundle(void)
{
    /* both messages copied from probe run 2 on the car */
    static const char *carinfo_mirror =
        "52 4d 53 47 b8 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 42 4e 44 4c 01 00 00 00 05 00 00 00 "
        "64 61 74 61 00 05 00 00 00 07 00 00 00 40 04 ce 05 07 8c f6 00 00 00 00";
    static const char *version_string =
        "52 4d 53 47 e5 06 00 00 03 04 00 00 02 00 00 00 08 00 00 00 42 4e 44 4c 02 00 00 00 07 00 00 00 "
        "46 6f 72 6d 61 74 00 01 00 00 00 04 00 00 00 07 00 00 00 53 74 72 69 6e 67 00 04 00 00 00 16 00 "
        "00 00 53 45 56 5f 56 20 53 41 39 53 44 30 2d 44 42 41 2d 39 35 33 53 00 00 00 00 00";
    unsigned char m[256];
    const unsigned char *data;
    BundleEntry e[8];
    BusHeader h;
    char text[200];
    int len, n, complete;

    len = parse_hex(carinfo_mirror, m, sizeof m);
    CHECK(len == 56);
    CHECK(busring_message_header(m, len, &h) && h.what == 0x4B8 && h.sender == 0);
    n = busring_bundle_bytes(m, len, "data", &data);
    CHECK(n == 7 && data[0] == 0x40 && data[5] == 0x8C);
    busring_bundle_format(m, len, text, sizeof text);
    CHECK(strcmp(text, "data=<40 04 ce 05 07 8c f6>") == 0);

    len = parse_hex(version_string, m, sizeof m);
    CHECK(len == 92);
    n = busring_bundle_entries(m, len, e, 8, &complete);
    CHECK(n == 2 && complete);
    CHECK(e[0].type == BUNDLE_INT && e[0].int_value == 4 && strcmp(e[0].key, "Format") == 0);
    CHECK(e[1].type == BUNDLE_STRING && e[1].data_length == 22);
    busring_bundle_format(m, len, text, sizeof text);
    CHECK(strcmp(text, "Format=4 String=\"SEV_V SA9SD0-DBA-953S\"") == 0);
    CHECK(busring_bundle_bytes(m, len, "data", &data) == -1);

    /* truncated: the first entry survives, the rest is reported as not parsed */
    n = busring_bundle_entries(m, 60, e, 8, &complete);
    CHECK(n == 1 && !complete);
    busring_bundle_format(m, 60, text, sizeof text);
    CHECK(strcmp(text, "Format=4 (rest not parsed)") == 0);

    /* a bare 20-byte message has no bundle */
    n = busring_bundle_entries(m, 20, e, 8, &complete);
    CHECK(n == 0);
    busring_bundle_format(m, 20, text, sizeof text);
    CHECK(text[0] == '\0');
}

static void test_nmea(void)
{
    NmeaState st;
    char line[128];
    int used = 0;
    const char *stream =
        "$PSRFTXT,hello*00\r\n"
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n"
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*00\r\n";

    memset(&st, 0, sizeof st);
    nmea_feed(&st, line, &used, sizeof line, stream, (int)strlen(stream));
    CHECK(st.have_rmc && st.valid);
    CHECK(st.speed_dkmh == 415);   /* 22.4 knots = 41.48 km/h */
    CHECK(st.course_ddeg == 844);
    CHECK(st.have_gga && st.fix_quality == 1 && st.satellites == 8);
    CHECK(st.altitude_dm == 5454);
    CHECK(st.bad_checksums == 2);  /* the PSRFTXT and the last RMC have wrong checksums */
    CHECK(strstr(st.seen, "GPRMC") && strstr(st.seen, "GPGGA"));
    CHECK(strstr(st.seen, "GPRMC GPRMC") == NULL);
    CHECK(st.rmc_count == 1);
    CHECK(st.have_time && st.utc_seconds == 12 * 3600 + 35 * 60 + 19);
    CHECK(st.have_date && st.year == 1994 && st.month == 3 && st.day == 23);
    CHECK(st.have_position && st.lat_e6 == 48117300 && st.lon_e6 == 11516666);

    memset(&st, 0, sizeof st);
    CHECK(nmea_parse_sentence(&st, "$GPRMC,235959.000,A,3351.000,S,07012.000,W,0.0,0.0,010126,,,A"));
    CHECK(st.utc_seconds == 86399 && st.year == 2026 && st.month == 1 && st.day == 1);
    CHECK(st.lat_e6 == -33850000 && st.lon_e6 == -70200000);

    memset(&st, 0, sizeof st);
    CHECK(nmea_parse_sentence(&st, "$GPRMC,,V,,,,,,,,,,N"));
    CHECK(st.have_rmc && !st.valid);
    CHECK(!nmea_parse_sentence(&st, "garbage"));
}

int main(void)
{
    test_ring();
    test_bundle();
    test_nmea();
    if (failures == 0) printf("all checks passed\n");
    return failures ? 1 : 0;
}
