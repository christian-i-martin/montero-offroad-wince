/* Renders dashboard scenes to SVG through the same render.c the head unit uses, with text widths
 * taken from the Barlow font files, so the layout can be checked on the build machine.
 * Usage: dashboard_preview FONT_DIR OUT_DIR [MAP_DIR [LAT LON]]
 * Writes driving.svg, parked-night.svg, no-gps.svg, startup.svg, menu.svg; the fonts are embedded
 * so any SVG viewer shows the real typeface. With MAP_DIR (a folder of *.pack files) the map is
 * composed by map.c exactly as on the head unit and embedded as a PNG picture; LAT LON move the
 * demo drive there (default 46 N 7.5 E, a made-up place). */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map.h"
#include "model.h"
#include "render.h"

/* ---------------------------------------------------------------- TrueType advance widths */

typedef struct {
    unsigned char *data;
    long size;
    unsigned units, metrics, hmtx, cmap4;
} Ttf;

static Ttf g_ttf[FACE_COUNT];
static const char *const g_file[FACE_COUNT] = {"Barlow-Medium.ttf", "Barlow-SemiBold.ttf", "BarlowCondensed-SemiBold.ttf"};
static const char *const g_family[FACE_COUNT] = {"Barlow Medium", "Barlow SemiBold", "Barlow Condensed SemiBold"};

static unsigned be16(const Ttf *f, unsigned o) { return o + 2 <= (unsigned)f->size ? (f->data[o] << 8) | f->data[o + 1] : 0; }
static unsigned be32(const Ttf *f, unsigned o) { return (be16(f, o) << 16) | be16(f, o + 2); }

static unsigned table(const Ttf *f, const char *tag)
{
    unsigned n = be16(f, 4), i;
    for (i = 0; i < n; i++)
        if (memcmp(f->data + 12 + 16 * i, tag, 4) == 0)
            return be32(f, 12 + 16 * i + 8);
    return 0;
}

static int load_ttf(Ttf *f, const char *path)
{
    FILE *in = fopen(path, "rb");
    unsigned cmap, n, i;
    if (!in)
        return 0;
    fseek(in, 0, SEEK_END);
    f->size = ftell(in);
    fseek(in, 0, SEEK_SET);
    f->data = malloc((size_t)f->size);
    if (fread(f->data, 1, (size_t)f->size, in) != (size_t)f->size) {
        fclose(in);
        return 0;
    }
    fclose(in);
    f->units = be16(f, table(f, "head") + 18);
    f->metrics = be16(f, table(f, "hhea") + 34);
    f->hmtx = table(f, "hmtx");
    cmap = table(f, "cmap");
    n = be16(f, cmap + 2);
    for (i = 0; i < n; i++)
        if (be16(f, cmap + 4 + 8 * i) == 3 && be16(f, cmap + 6 + 8 * i) == 1) {
            f->cmap4 = cmap + be32(f, cmap + 8 + 8 * i);
            break;
        }
    return f->units && f->hmtx && f->cmap4 && be16(f, f->cmap4) == 4;
}

static unsigned glyph(const Ttf *f, unsigned c)
{
    unsigned seg2 = be16(f, f->cmap4 + 6), ends = f->cmap4 + 14, starts = ends + seg2 + 2;
    unsigned deltas = starts + seg2, ranges = deltas + seg2, i;
    for (i = 0; i < seg2; i += 2) {
        unsigned end = be16(f, ends + i), start = be16(f, starts + i), delta = be16(f, deltas + i), range = be16(f, ranges + i);
        if (end < c)
            continue;
        if (start > c)
            return 0;
        if (range == 0)
            return (c + delta) & 0xFFFF;
        c = be16(f, ranges + i + range + 2 * (c - start));
        return c ? (c + delta) & 0xFFFF : 0;
    }
    return 0;
}

static unsigned advance(const Ttf *f, unsigned g)
{
    return be16(f, f->hmtx + 4 * (g < f->metrics ? g : f->metrics - 1));
}

static unsigned next_char(const unsigned char **p)
{
    unsigned c = *(*p)++;
    if (c >= 0xE0 && (*p)[0] && (*p)[1]) {
        c = ((c & 0x0F) << 12) | (((*p)[0] & 0x3F) << 6) | ((*p)[1] & 0x3F);
        *p += 2;
    } else if (c >= 0xC0 && (*p)[0]) {
        c = ((c & 0x1F) << 6) | ((*p)[0] & 0x3F);
        *p += 1;
    }
    return c;
}

/* ---------------------------------------------------------------- SVG canvas */

static FILE *g_out;

static void colour(char *buf, unsigned v) { sprintf(buf, "#%06x", v & 0xFFFFFF); }

static void svg_fill(void *ctx, int x, int y, int w, int h, unsigned rgb)
{
    char c[8];
    colour(c, rgb);
    fprintf(g_out, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"%s\"/>\n", x, y, w, h, c);
}

static void svg_round(void *ctx, int x, int y, int w, int h, int r, unsigned fill, unsigned border)
{
    char f[8], b[8];
    colour(f, fill);
    colour(b, border);
    fprintf(g_out, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%d\" height=\"%d\" rx=\"%d\" fill=\"%s\" stroke=\"%s\"/>\n",
            x + 0.5, y + 0.5, w - 1, h - 1, r, fill == NO_FILL ? "none" : f, b);
}

static void points(const DPoint *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        fprintf(g_out, "%d,%d ", p[i].x, p[i].y);
}

static void svg_polyline(void *ctx, const DPoint *p, int n, int width, unsigned rgb)
{
    char c[8];
    colour(c, rgb);
    fprintf(g_out, "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"%d\" stroke-linejoin=\"round\" "
                   "stroke-linecap=\"round\" points=\"", c, width);
    points(p, n);
    fprintf(g_out, "\"/>\n");
}

static void svg_polygon(void *ctx, const DPoint *p, int n, unsigned fill)
{
    char c[8];
    colour(c, fill);
    fprintf(g_out, "<polygon fill=\"%s\" points=\"", c);
    points(p, n);
    fprintf(g_out, "\"/>\n");
}

static void svg_circle(void *ctx, int cx, int cy, int r, unsigned fill, unsigned border, int bw)
{
    char f[8], b[8];
    colour(f, fill);
    colour(b, border);
    fprintf(g_out, "<circle cx=\"%d\" cy=\"%d\" r=\"%d\" fill=\"%s\" stroke=\"%s\" stroke-width=\"%d\"/>\n", cx, cy, r,
            fill == NO_FILL ? "none" : f, b, bw);
}

static int svg_width(void *ctx, int font, const char *s)
{
    const Ttf *f = &g_ttf[render_fonts[font].face];
    const unsigned char *p = (const unsigned char *)s;
    unsigned total = 0;
    while (*p)
        total += advance(f, glyph(f, next_char(&p)));
    return (int)floor((double)total * render_fonts[font].px / f->units + 0.5);
}

static void svg_text(void *ctx, int x, int baseline, int font, unsigned rgb, const char *s)
{
    char c[8];
    colour(c, rgb);
    fprintf(g_out, "<text x=\"%d\" y=\"%d\" font-family=\"%s\" font-size=\"%d\" fill=\"%s\">", x, baseline,
            g_family[render_fonts[font].face], render_fonts[font].px, c);
    for (; *s; s++) {
        if (*s == '&') fputs("&amp;", g_out);
        else if (*s == '<') fputs("&lt;", g_out);
        else if (*s == '>') fputs("&gt;", g_out);
        else fputc(*s, g_out);
    }
    fprintf(g_out, "</text>\n");
}

/* ---------------------------------------------------------------- map picture as PNG */

static MapSet g_map;
static double g_lat = 46.0, g_lon = 7.5;

static unsigned g_crc[256];

static unsigned crc32(unsigned crc, const unsigned char *p, size_t n)
{
    size_t i;
    if (!g_crc[1]) {
        unsigned k, c, j;
        for (k = 0; k < 256; k++) {
            for (c = k, j = 0; j < 8; j++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++)
        crc = g_crc[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static unsigned char *put32(unsigned char *o, unsigned v)
{
    o[0] = (unsigned char)(v >> 24); o[1] = (unsigned char)(v >> 16); o[2] = (unsigned char)(v >> 8); o[3] = (unsigned char)v;
    return o + 4;
}

/* An RGB PNG of the RGB565 pixels, with uncompressed ("stored") deflate blocks. */
static unsigned char *png_from_565(const unsigned short *px, int w, int h, long *size)
{
    long raw_n = (long)h * (1 + 3L * w), blocks = (raw_n + 65534) / 65535, i, at;
    unsigned char *raw = malloc((size_t)raw_n), *png = malloc((size_t)(raw_n + blocks * 5 + 128)), *o, *idat;
    unsigned a = 1, b = 0;
    int x, y;
    for (y = 0, at = 0; y < h; y++) {
        raw[at++] = 0;
        for (x = 0; x < w; x++) {
            unsigned c = px[y * w + x];
            raw[at++] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
            raw[at++] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
            raw[at++] = (unsigned char)((c & 31) * 255 / 31);
        }
    }
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    o = put32(png + 8, 13);
    memcpy(o, "IHDR", 4);
    put32(o + 4, (unsigned)w);
    put32(o + 8, (unsigned)h);
    o[12] = 8; o[13] = 2; o[14] = 0; o[15] = 0; o[16] = 0;
    put32(o + 17, crc32(0, o, 17));
    o += 21;
    idat = o + 4;
    memcpy(idat, "IDAT", 4);
    o = idat + 4;
    *o++ = 0x78; *o++ = 0x01;
    for (i = 0; i < raw_n; i += 65535) {
        long n = raw_n - i < 65535 ? raw_n - i : 65535;
        *o++ = i + n == raw_n ? 1 : 0;
        *o++ = (unsigned char)(n & 0xFF); *o++ = (unsigned char)(n >> 8);
        *o++ = (unsigned char)(~n & 0xFF); *o++ = (unsigned char)((~n >> 8) & 0xFF);
        memcpy(o, raw + i, (size_t)n);
        o += n;
    }
    for (i = 0; i < raw_n; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    o = put32(o, (b << 16) | a);
    put32(idat - 4, (unsigned)(o - idat - 4));
    o = put32(o, crc32(0, idat, (size_t)(o - idat)));
    o = put32(o, 0);
    memcpy(o, "IEND", 4);
    o = put32(o + 4, crc32(0, o, 4));
    free(raw);
    *size = o - png;
    return png;
}

static void base64(FILE *out, const unsigned char *d, long n);

static int svg_map(void *ctx, int x, int y, int w, int h, int zoom, double px, double py, int night)
{
    static unsigned short pixels[400 * 400];
    unsigned char *png;
    long size;
    int result;
    if (g_map.count == 0 || w > 400 || h > 400)
        return MAP_NO_FILES;
    result = map_compose(&g_map, pixels, w, h, w, zoom, px, py, night);
    png = png_from_565(pixels, w, h, &size);
    fprintf(g_out, "<image x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" href=\"data:image/png;base64,", x, y, w, h);
    base64(g_out, png, size);
    fprintf(g_out, "\"/>\n");
    free(png);
    return result;
}

static int file_read_at(void *file, unsigned offset, void *buffer, unsigned size)
{
    if (fseek((FILE *)file, (long)offset, SEEK_SET) != 0)
        return -1;
    return (int)fread(buffer, 1, size, (FILE *)file);
}

static int open_maps(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        FILE *f;
        if (n < 6 || strcmp(e->d_name + n - 5, ".pack") != 0)
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        f = fopen(path, "rb");
        if (f && !map_add_pack(&g_map, f, file_read_at)) {
            fprintf(stderr, "not a map pack: %s\n", path);
            fclose(f);
        }
    }
    closedir(d);
    return g_map.count;
}

/* The program sets the map view after dash_init: as dash.c does. */
static void set_map_view(DashModel *m)
{
    if (g_map.count == 0)
        return;
    m->map_min_zoom = g_map.min_zoom;
    m->map_max_zoom = g_map.max_zoom;
    if (m->map_zoom > g_map.max_zoom)
        m->map_zoom = g_map.max_zoom;
    if (m->map_zoom < g_map.min_zoom)
        m->map_zoom = g_map.min_zoom;
}

static const Canvas g_svg = {NULL, svg_fill, svg_round, svg_polyline, svg_polygon, svg_circle, svg_width, svg_text,
                             svg_map};

static void base64(FILE *out, const unsigned char *d, long n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    long i;
    for (i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        fputc(t[v >> 18 & 63], out);
        fputc(t[v >> 12 & 63], out);
        fputc(i + 1 < n ? t[v >> 6 & 63] : '=', out);
        fputc(i + 2 < n ? t[v & 63] : '=', out);
    }
}

static int write_svg(const char *dir, const char *name, const DashModel *m, int menu)
{
    char path[600];
    int i;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    g_out = fopen(path, "w");
    if (!g_out)
        return 0;
    fprintf(g_out, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n"
                   "<style>\n", SCREEN_W, SCREEN_H, SCREEN_W, SCREEN_H);
    for (i = 0; i < FACE_COUNT; i++) {
        fprintf(g_out, "@font-face { font-family: '%s'; src: url(data:font/ttf;base64,", g_family[i]);
        base64(g_out, g_ttf[i].data, g_ttf[i].size);
        fprintf(g_out, "); }\n");
    }
    fprintf(g_out, "</style>\n");
    render_dashboard(&g_svg, m);
    if (menu)
        render_menu(&g_svg, m, 1, "0.1.0");
    fprintf(g_out, "</svg>\n");
    fclose(g_out);
    return 1;
}

/* ---------------------------------------------------------------- scenes */

enum { SCENE_DRIVING, SCENE_PARKED_NIGHT, SCENE_NO_GPS };

static double terrain(double d) /* elevation in metres after d metres driven */
{
    return 900.0 + 400.0 * sin(d / 6000.0) + 120.0 * sin(d / 1700.0);
}

/* A demo drive (made-up place: 46 N, 7.5 E, 2026-07-15) from 08:10 to 14:32 local time (UTC+2). */
static void simulate(DashModel *m, int scene)
{
    DashInputs in;
    double dist = 0, course = 90;
    long t, total = 6 * 3600 + 22 * 60, local_s = 8 * 3600 + 10 * 60;
    unsigned seed = 12345;

    memset(&in, 0, sizeof in);
    dash_init(m);
    in.clock_valid = 1;
    in.year = 2026; in.month = 7; in.day = 15;
    in.system_utc_offset_min = 120;
    in.gps.have_rmc = in.gps.have_gga = in.gps.have_time = in.gps.have_position = 1;
    in.gps.fix_quality = 1;
    in.gps.lat_e6 = (int)floor(g_lat * 1e6 + 0.5);
    in.gps.lon_e6 = (int)floor(g_lon * 1e6 + 0.5);
    in.gps.have_date = 1;
    in.gps.year = 2026; in.gps.month = 7; in.gps.day = 15;
    in.car.have_compass = in.car.have_elevation = in.car.have_pressure = in.car.have_outside = in.car.have_battery = 1;
    if (scene == SCENE_PARKED_NIGHT)
        total += 6 * 3600 + 33 * 60; /* stay out until 21:05 */
    for (t = 0; t <= total; t++) {
        long s = local_s + t;
        int moving = (t / 300) % 5 != 4 && t < 6 * 3600 + 22 * 60; /* stop for 5 min every 25 min */
        double speed, e;
        if (scene == SCENE_DRIVING && t > total - 30)
            moving = 1;
        speed = moving ? 38.0 + 16.0 * sin(t / 400.0) : 0.0;
        seed = seed * 1103515245u + 12345u;
        if (t > total - 30 && scene == SCENE_DRIVING)
            speed = 42.0;
        dist += speed / 3.6;
        course = fmod(course + (moving ? 1.3 * sin(t / 230.0) : 0.0) + 360.0, 360.0);
        if (t > total - 30 && scene == SCENE_DRIVING)
            course = 127.0;
        e = terrain(dist);
        in.now_ms = (unsigned)(t * 1000 + 5000);
        in.hour = (int)(s / 3600 % 24); in.minute = (int)(s / 60 % 60); in.second = (int)(s % 60);
        in.gps.utc_seconds = (int)((s - 7200) % 86400);
        in.gps_fresh = 1;
        in.gps.valid = 1;
        in.gps.satellites = 9;
        in.gps.speed_dkmh = (int)(speed * 10.0);
        in.gps.course_ddeg = (int)(course * 10.0);
        in.gps.altitude_dm = (int)(e * 10.0) + (int)(seed >> 16) % 30 - 15;
        in.car_fresh = 1;
        in.car.compass = (int)floor(fmod(course + 22.5, 360.0) / 45.0) + 1;
        in.car.elevation_m = (int)(e - 80.0);
        in.car.pressure_hpa = (int)floor(1013.25 * pow(1.0 - 2.25577e-5 * e, 5.25588) - t / 18000.0 + 0.5);
        in.car.outside_c = (int)floor(27.0 - 0.0065 * (e - 640.0) - (scene == SCENE_PARKED_NIGHT && t > 30000 ? 6 : 0) + 0.5);
        in.car.battery_dv = moving ? 140 + (int)(seed >> 20) % 3 - 1 : (t % 1500 < 200 ? 139 : 124);
        in.lights_on = scene == SCENE_PARKED_NIGHT && t > total - 3600;
        if (scene == SCENE_NO_GPS && t > total - 120)
            in.gps_fresh = 0;
        dash_update(m, &in);
    }
    set_map_view(m);
}

int main(int argc, char **argv)
{
    static DashModel m;
    char path[600];
    int i;

    if (argc != 3 && argc != 4 && argc != 6) {
        fprintf(stderr, "usage: %s FONT_DIR OUT_DIR [MAP_DIR [LAT LON]]\n", argv[0]);
        return 2;
    }
    if (!map_init(&g_map, 16))
        return 1;
    if (argc >= 4 && !open_maps(argv[3])) {
        fprintf(stderr, "no map packs in %s\n", argv[3]);
        return 1;
    }
    if (argc == 6) {
        g_lat = atof(argv[4]);
        g_lon = atof(argv[5]);
    }
    for (i = 0; i < FACE_COUNT; i++) {
        snprintf(path, sizeof path, "%s/%s", argv[1], g_file[i]);
        if (!load_ttf(&g_ttf[i], path)) {
            fprintf(stderr, "cannot read %s\n", path);
            return 1;
        }
    }
    simulate(&m, SCENE_DRIVING);
    if (!write_svg(argv[2], "driving.svg", &m, 0) || !write_svg(argv[2], "menu.svg", &m, 1))
        return 1;
    simulate(&m, SCENE_PARKED_NIGHT);
    write_svg(argv[2], "parked-night.svg", &m, 0);
    simulate(&m, SCENE_NO_GPS);
    write_svg(argv[2], "no-gps.svg", &m, 0);
    dash_init(&m);
    set_map_view(&m);
    write_svg(argv[2], "startup.svg", &m, 0);
    printf("wrote 5 previews to %s\n", argv[2]);
    return 0;
}
