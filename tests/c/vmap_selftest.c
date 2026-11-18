/* Checks dashboard/src/vraster.c and vmap.c on the build machine (tests/test_vmap.py).
 *
 *   vmap_selftest                         the filling and line tests only
 *   vmap_selftest TEST.vmap FONT.ttf      also draws the test map that test_vmap.py writes
 *
 * Prints one line per failure and "ok N" at the end; the exit code is the failure count. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfont.h"
#include "vmap.h"
#include "vraster.h"
#include "vtiles.h"

static int g_failures, g_checks;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            printf("FAIL line %d: ", __LINE__);                                                                       \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            g_failures++;                                                                                             \
        }                                                                                                             \
    } while (0)

#define W 32
#define H 32
static int g_cov[H][W];

static void record_span(void *ctx, int y, int x, int len, int coverage)
{
    int i;
    for (i = 0; i < len; i++)
        g_cov[y][x + i] += coverage;
}

static void sweep(VRaster *r)
{
    memset(g_cov, 0, sizeof g_cov);
    vr_sweep(r, record_span, NULL);
}

static void box(VRaster *r, double x0, double y0, double x1, double y1)
{
    vr_move_to(r, (int)(x0 * VR_ONE), (int)(y0 * VR_ONE));
    vr_line_to(r, (int)(x1 * VR_ONE), (int)(y0 * VR_ONE));
    vr_line_to(r, (int)(x1 * VR_ONE), (int)(y1 * VR_ONE));
    vr_line_to(r, (int)(x0 * VR_ONE), (int)(y1 * VR_ONE));
    vr_close(r);
}

static void test_filling(void)
{
    VRaster r;
    int x, y, ok = 1;
    CHECK(vr_init(&r, W, H), "vr_init");
    vr_set_clip(&r, W, H);

    box(&r, 2, 2, 6, 6);
    sweep(&r);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (g_cov[y][x] != ((x >= 2 && x < 6 && y >= 2 && y < 6) ? 256 : 0))
                ok = 0;
    CHECK(ok, "a 4 x 4 square covers exactly its 16 pixels");

    box(&r, 2.5, 2, 6, 6.25);
    sweep(&r);
    CHECK(g_cov[3][2] == 128, "half a pixel: %d", g_cov[3][2]);
    CHECK(g_cov[6][3] == 64, "a quarter row: %d", g_cov[6][3]);
    CHECK(g_cov[6][2] == 32, "half of a quarter: %d", g_cov[6][2]);

    box(&r, -10, 1, 3, 4);
    sweep(&r);
    CHECK(g_cov[2][0] == 256 && g_cov[2][2] == 256 && g_cov[2][3] == 0, "cut at the left edge: %d %d %d", g_cov[2][0],
          g_cov[2][2], g_cov[2][3]);

    box(&r, 28, -5, 40, 3);
    sweep(&r);
    CHECK(g_cov[0][28] == 256 && g_cov[2][31] == 256 && g_cov[3][31] == 0, "cut at the right and top edges");

    /* a hole: the inner square runs the other way */
    box(&r, 2, 2, 10, 10);
    vr_move_to(&r, 4 * VR_ONE, 4 * VR_ONE);
    vr_line_to(&r, 4 * VR_ONE, 8 * VR_ONE);
    vr_line_to(&r, 8 * VR_ONE, 8 * VR_ONE);
    vr_line_to(&r, 8 * VR_ONE, 4 * VR_ONE);
    vr_close(&r);
    sweep(&r);
    CHECK(g_cov[3][3] == 256 && g_cov[5][5] == 0, "a hole stays empty: %d %d", g_cov[3][3], g_cov[5][5]);

    /* overlapping shapes running the same way are simply covered */
    box(&r, 2, 2, 6, 6);
    box(&r, 4, 4, 8, 8);
    sweep(&r);
    CHECK(g_cov[4][4] == 256, "overlap is covered once: %d", g_cov[4][4]);

    /* a turned square: the area adds up to the shape's area (32 pixels) */
    vr_move_to(&r, 10 * VR_ONE, 2 * VR_ONE);
    vr_line_to(&r, 14 * VR_ONE, 6 * VR_ONE);
    vr_line_to(&r, 10 * VR_ONE, 10 * VR_ONE);
    vr_line_to(&r, 6 * VR_ONE, 6 * VR_ONE);
    vr_close(&r);
    sweep(&r);
    {
        long sum = 0;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                sum += g_cov[y][x];
        CHECK(sum == 32 * 256, "a diamond of 32 square pixels: %ld / 256", sum);
    }
    CHECK(vr_isqrt(0) == 0 && vr_isqrt(15) == 3 && vr_isqrt(16) == 4 && vr_isqrt(2147395600u) == 46340, "vr_isqrt");
    vr_free(&r);
}

static void test_lines(void)
{
    VRaster r;
    int xy[6], dashes[2];
    vr_init(&r, W, H);
    vr_set_clip(&r, W, H);

    /* right, then up: the wedge fills the outer corner, the overlap does not cancel */
    xy[0] = 2 * VR_ONE, xy[1] = 8 * VR_ONE;
    xy[2] = 8 * VR_ONE, xy[3] = 8 * VR_ONE;
    xy[4] = 8 * VR_ONE, xy[5] = 2 * VR_ONE;
    vr_stroke(&r, xy, 3, 4 * VR_ONE, 0, 0, NULL, 0);
    sweep(&r);
    CHECK(g_cov[7][7] == 256, "inside both segments: %d", g_cov[7][7]);
    CHECK(g_cov[8][8] == 256, "the outer corner of the bend: %d", g_cov[8][8]);
    CHECK(g_cov[8][4] == 256 && g_cov[10][4] == 0 && g_cov[5][9] == 256, "the line's width");
    CHECK(g_cov[8][1] == 0, "square-cut end: %d", g_cov[8][1]);

    xy[0] = 4 * VR_ONE; /* a round end of radius 2 reaches x = 2 */
    vr_stroke(&r, xy, 3, 4 * VR_ONE, 1, 0, NULL, 0);
    sweep(&r);
    CHECK(g_cov[8][2] > 200 && g_cov[8][1] == 0 && g_cov[10][2] == 0, "round end: %d %d %d", g_cov[8][2], g_cov[8][1],
          g_cov[10][2]);

    /* dashes 4 on, 4 off along y = 16 */
    xy[0] = 0, xy[1] = 16 * VR_ONE + VR_ONE / 2;
    xy[2] = 30 * VR_ONE, xy[3] = 16 * VR_ONE + VR_ONE / 2;
    dashes[0] = dashes[1] = 4 * VR_ONE;
    vr_stroke(&r, xy, 2, 2 * VR_ONE, 0, 0, dashes, 2);
    sweep(&r);
    CHECK(g_cov[16][1] == 256 && g_cov[16][5] == 0 && g_cov[16][9] == 256 && g_cov[16][13] == 0, "dashes: %d %d %d %d",
          g_cov[16][1], g_cov[16][5], g_cov[16][9], g_cov[16][13]);

    vr_disc(&r, 16 * VR_ONE, 24 * VR_ONE, 3 * VR_ONE);
    sweep(&r);
    CHECK(g_cov[24][16] == 256 && g_cov[24][20] == 0 && g_cov[20][16] == 0, "disc");
    vr_free(&r);
}

static void test_colours(void)
{
    unsigned short px[4] = {0, 0, 0, 0};
    VSurface s = {px, 4, 1, 4, NULL};
    VrPaint p;
    unsigned char mask[4] = {255, 128, 0, 0};
    CHECK(vr_rgb565(0xFFFFFF, 0) == 0xFFFF && vr_rgb565(0xFF0000, 0) == 0xF800 && vr_rgb565(0xFFFFFF, 1) == 0x7BEF,
          "rgb565");
    vr_paint_init(&p, &s, 0xFFFFFF, 256, 0);
    vr_paint_span(&p, 0, 0, 1, 256);
    vr_paint_span(&p, 0, 1, 1, 128);
    CHECK(px[0] == 0xFFFF, "full coverage paints the colour: %04x", px[0]);
    CHECK(((px[1] >> 11) & 0x1F) == 16 || ((px[1] >> 11) & 0x1F) == 15, "half coverage mixes: %04x", px[1]);
    vr_paint_init(&p, &s, 0xFFFFFF, 64, 0);
    vr_paint_span(&p, 0, 2, 1, 256);
    CHECK(((px[2] >> 11) & 0x1F) >= 7 && ((px[2] >> 11) & 0x1F) <= 8, "a quarter opacity: %04x", px[2]);
    px[0] = px[1] = px[2] = 0;
    vr_blit_mask(&s, 0, 0, mask, 4, 1, 0x00FF00, 0);
    CHECK(px[0] == 0x07E0 && (px[1] >> 5) > 25 && (px[1] >> 5) < 38 && px[2] == 0, "mask: %04x %04x", px[0], px[1]);
}

static int file_read(void *file, unsigned offset, void *buffer, unsigned size)
{
    FILE *f = (FILE *)file;
    if (fseek(f, (long)offset, SEEK_SET) != 0)
        return -1;
    return (int)fread(buffer, 1, size, f);
}

static unsigned rgb_at(const unsigned short *px, int x, int y)
{
    unsigned c = px[y * 800 + x];
    return ((c >> 11) & 0x1F) << 19 | ((c >> 5) & 0x3F) << 10 | (c & 0x1F) << 3;
}

/* The test map (tests/test_vmap.py): one zoom 14 tile, 13696 x 7533; a red square over the
 * middle half of the tile, a blue line across its middle from west to east, a label "Test" at the
 * middle, land black (dark) or green (light). */
static void test_map(const char *path, const char *font_path)
{
    static unsigned short px[800 * 480];
    static VFont font;
    VSurface s = {px, 800, 480, 800, NULL};
    VMap m;
    VMapView v;
    VMapStats st;
    FILE *f = fopen(path, "rb"), *ff = fopen(font_path, "rb");
    unsigned char *ttf;
    long n;
    int drawn;
    double lo, hi;

    CHECK(f != NULL && ff != NULL, "test files");
    if (f == NULL || ff == NULL)
        return;
    fseek(ff, 0, SEEK_END);
    n = ftell(ff);
    fseek(ff, 0, SEEK_SET);
    ttf = (unsigned char *)malloc((size_t)n);
    CHECK(fread(ttf, 1, (size_t)n, ff) == (size_t)n && vf_load(&font, ttf, (unsigned)n), "font");
    CHECK(vf_glyph(&font, 'A') != 0 && vf_glyph(&font, 0xF1) != 0, "letters A and n with tilde");
    CHECK(vf_outline(&font, vf_glyph(&font, 0xF1)) != NULL, "a composite letter has an outline");
    CHECK(vmap_open(&m, f, file_read, 8), "vmap_open");
    vmap_set_font(&m, &font);
    vmap_zoom_range(&m, &lo, &hi);
    CHECK(lo == 14.0 && hi == 19.5, "zoom range %g to %g", lo, hi);

    memset(&v, 0, sizeof v);
    v.mx = (13696 + 0.5) / 16384.0;
    v.my = (7533 + 0.5) / 16384.0;
    v.zoom = 14;
    v.cx = 400;
    v.cy = 240;
    v.labels = 1;
    drawn = vmap_render(&m, &s, &v, &st);
    CHECK(drawn == 1 && st.tiles_drawn == 1 && st.tiles_loaded == 1, "drawn %d, tiles %d", drawn, st.tiles_drawn);
    /* the tile spans x 272 to 528 and y 112 to 368; the square 336 to 464 and 176 to 304 */
    CHECK(rgb_at(px, 350, 200) == 0xC82828, "square at (350, 200): %06x", rgb_at(px, 350, 200));
    CHECK(rgb_at(px, 300, 200) == 0, "land at (300, 200): %06x", rgb_at(px, 300, 200));
    CHECK((rgb_at(px, 350, 240) & 0xFF) > 0xC0 && (rgb_at(px, 350, 240) >> 16) < 0x40, "line at (350, 240): %06x",
          rgb_at(px, 350, 240));
    CHECK(rgb_at(px, 600, 240) == 0, "nothing outside the tile: %06x", rgb_at(px, 600, 240));
    CHECK(st.labels_placed == 1, "the label: %d placed", st.labels_placed);
    /* the parking marker at tile point (1536, 3584): screen (368, 336); its badge is blue, the P white */
    CHECK(st.markers == 1, "one place marker: %d", st.markers);
    CHECK(rgb_at(px, 368 - 8, 336 - 8) == 0x186CD8, "marker badge: %06x", rgb_at(px, 368 - 8, 336 - 8));
    CHECK(rgb_at(px, 368 - 4, 336 + 4) == 0xF8FCF8, "marker pictogram: %06x", rgb_at(px, 368 - 4, 336 + 4));

    v.labels = 0;
    v.style = 1;
    v.bearing = 90; /* east up: the west-east line now runs up the screen */
    vmap_render(&m, &s, &v, &st);
    CHECK((rgb_at(px, 400, 200) & 0xFF) > 0xC0, "turned line at (400, 200): %06x", rgb_at(px, 400, 200));
    CHECK((rgb_at(px, 350, 240) >> 16) > 0xC0 && (rgb_at(px, 350, 240) & 0xFF) < 0x40, "turned square at (350, 240): %06x",
          rgb_at(px, 350, 240));
    CHECK(rgb_at(px, 300, 200) == 0x00C000, "light land: %06x", rgb_at(px, 300, 200));

    v.bearing = 0;
    v.zoom = 15; /* twice as close: the square spans 272 to 528 */
    vmap_render(&m, &s, &v, &st);
    CHECK((rgb_at(px, 280, 120) >> 16) > 0xC0, "zoom 15 square at (280, 120): %06x", rgb_at(px, 280, 120));
    v.zoom = 14.5; /* 1.41 times: the square spans about 309 to 491 */
    vmap_render(&m, &s, &v, &st);
    CHECK((rgb_at(px, 315, 150) >> 16) > 0xC0 && ((rgb_at(px, 300, 140) >> 8) & 0xFF) > 0xB0, "zoom 14.5: %06x %06x",
          rgb_at(px, 315, 150), rgb_at(px, 300, 140));

    v.mx = 0.1; /* far away */
    CHECK(vmap_render(&m, &s, &v, &st) == 0, "outside the map");
    vmap_close(&m);
    vf_free(&font);
    free(ttf);
    fclose(f);
    fclose(ff);
}

/* 32-bit pictures and kept labels on the test map. */
static void test_32bit_and_kept(const char *path, const char *font_path)
{
    static unsigned short px16[800 * 480];
    static unsigned px32[800 * 480], again[800 * 480];
    static VFont font;
    VSurface s16 = {px16, 800, 480, 800, NULL}, s32 = {NULL, 800, 480, 800, px32};
    VMap m;
    VTiles t;
    VMapView v;
    VMapStats st;
    FILE *f = fopen(path, "rb"), *ff = fopen(font_path, "rb");
    unsigned char *ttf;
    long n;
    int i, same = 0, close = 0;

    if (f == NULL || ff == NULL)
        return;
    fseek(ff, 0, SEEK_END);
    n = ftell(ff);
    fseek(ff, 0, SEEK_SET);
    ttf = (unsigned char *)malloc((size_t)n);
    if (fread(ttf, 1, (size_t)n, ff) != (size_t)n || !vf_load(&font, ttf, (unsigned)n) || !vmap_open(&m, f, file_read, 8))
        return;
    vmap_set_font(&m, &font);
    memset(&v, 0, sizeof v);
    v.mx = (13696 + 0.5) / 16384.0;
    v.my = (7533 + 0.5) / 16384.0;
    v.zoom = 14;
    v.cx = 400;
    v.cy = 240;
    vmap_draw(&m, &s32, &v, VM_DRAW_SHAPES, NULL);
    CHECK(px32[200 * 800 + 350] == 0xC82828, "32-bit square: %06x", px32[200 * 800 + 350]);
    CHECK((px32[240 * 800 + 350] & 0xFFFFFF) == 0x2040F0, "32-bit line: %06x", px32[240 * 800 + 350]);
    CHECK(px32[200 * 800 + 300] == 0, "32-bit land: %06x", px32[200 * 800 + 300]);
    vmap_draw(&m, &s16, &v, VM_DRAW_SHAPES, NULL);
    for (i = 0; i < 800 * 480; i++) {
        unsigned a = px32[i], b = VR_565_TO_32(px16[i]);
        int d = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
        close += d < 9 && d > -9;
    }
    CHECK(close == 800 * 480, "16 and 32-bit agree within the 16-bit steps: %d", close);

    CHECK(vt_init(&t, &m, 32), "pictures");
    v.bearing = 30;
    while (vt_fill(&t, &v, 800, 480, 0, 0.0))
        ;
    vt_compose(&t, &s16, &v, 1);
    vt_compose(&t, &s32, &v, 1);
    for (i = 0; i < 800 * 480; i++)
        same += px32[i] == VR_565_TO_32(px16[i]);
    CHECK(same == 800 * 480, "a 32-bit frame from pictures is the 16-bit one widened: %d", same);
    /* chunks or whole rows give the same pixels */
    {
        int k;
        for (k = 0; k < 3; k++) {
            v.bearing = 17 + 101 * k;
            v.zoom = 14 + 0.3 * k;
            while (vt_fill(&t, &v, 800, 480, 0, 0.0))
                ;
            t.chunk_w = t.chunk_h = 0;
            vt_compose(&t, &s32, &v, 1);
            memcpy(again, px32, sizeof again);
            t.chunk_w = 32 + 32 * k;
            t.chunk_h = 16 + 16 * (k & 1);
            vt_compose(&t, &s32, &v, 1);
            for (same = 0, i = 0; i < 800 * 480; i++)
                same += px32[i] == again[i];
            CHECK(same == 800 * 480, "chunked frame %d: %d of %d pixels as row by row", k, same, 800 * 480);
        }
        v.zoom = 14;
        v.bearing = 30;
    }
    vt_free(&t);

    /* labels: placed afresh, then kept for a frame from the same view and a small move */
    v.bearing = 0;
    vmap_draw(&m, &s32, &v, VM_DRAW_SHAPES | VM_DRAW_LABELS, &st);
    CHECK(st.labels_placed == 1 && st.labels_kept == 0, "fresh: %d placed, kept %d", st.labels_placed, st.labels_kept);
    memcpy(again, px32, sizeof again);
    vmap_draw(&m, &s32, &v, VM_DRAW_SHAPES | VM_DRAW_LABELS_KEPT, &st);
    CHECK(st.labels_kept == 1, "the same view keeps the labels");
    for (same = 0, i = 0; i < 800 * 480; i++)
        same += px32[i] == again[i];
    CHECK(same == 800 * 480, "kept labels land where they were placed: %d of %d", same, 800 * 480);
    v.mx += 50.0 / (256.0 * 16384.0); /* 50 pixels at zoom 14 */
    v.bearing = 10;
    vmap_draw(&m, &s32, &v, VM_DRAW_SHAPES | VM_DRAW_LABELS_KEPT, &st);
    CHECK(st.labels_kept == 1, "a small move keeps the labels");
    v.mx += 200.0 / (256.0 * 16384.0);
    vmap_draw(&m, &s32, &v, VM_DRAW_SHAPES | VM_DRAW_LABELS_KEPT, &st);
    CHECK(st.labels_kept == 0 && st.labels_placed == 1, "a long move places afresh: kept %d", st.labels_kept);
    v.bearing = 40;
    vmap_draw(&m, &s32, &v, VM_DRAW_SHAPES | VM_DRAW_LABELS_KEPT, &st);
    CHECK(st.labels_kept == 0, "a turn of 30 degrees places afresh");
    vmap_close(&m);
    vf_free(&font);
    free(ttf);
    fclose(f);
    fclose(ff);
}

/* Map pictures (vtiles.c) on the same test map: north up they equal a full draw pixel for pixel,
 * turned they put things in the same places, a new zoom borrows the last zoom's pictures, and a
 * cache smaller than the screen keeps what the frame uses. */
static void test_pictures(const char *path)
{
    static unsigned short full[800 * 480], pics[800 * 480];
    VSurface sf = {full, 800, 480, 800, NULL}, sp = {pics, 800, 480, 800, NULL};
    VMap m;
    VTiles t;
    VMapView v;
    FILE *f = fopen(path, "rb");
    int i, same = 0, filled = 0, missing;

    CHECK(f != NULL && vmap_open(&m, f, file_read, 8) && vt_init(&t, &m, 64), "open for pictures");
    if (f == NULL)
        return;
    memset(&v, 0, sizeof v);
    v.mx = (13696 + 0.5) / 16384.0;
    v.my = (7533 + 0.5) / 16384.0;
    v.zoom = 14;
    v.cx = 400;
    v.cy = 240;
    vmap_draw(&m, &sf, &v, VM_DRAW_SHAPES, NULL);
    missing = vt_compose(&t, &sp, &v, 1);
    CHECK(missing > 0 && rgb_at(pics, 350, 200) == 0, "nothing drawn yet: %d missing, %06x", missing,
          rgb_at(pics, 350, 200));
    while (vt_fill(&t, &v, 800, 480, 0, 0.0))
        filled++;
    CHECK(filled >= 12 && filled <= 15, "pictures on the screen (4 to 5 across, 3 down): %d", filled);
    CHECK(vt_compose(&t, &sp, &v, 1) == 0, "none missing after filling");
    for (i = 0; i < 800 * 480; i++)
        same += full[i] == pics[i];
    CHECK(same == 800 * 480, "north up: %d of %d pixels as in the full draw", same, 800 * 480);

    v.bearing = 90;
    while (vt_fill(&t, &v, 800, 480, 0, 0.0))
        filled++;
    vt_compose(&t, &sp, &v, 1);
    CHECK((rgb_at(pics, 400, 200) & 0xFF) > 0xC0, "turned line at (400, 200): %06x", rgb_at(pics, 400, 200));
    CHECK((rgb_at(pics, 350, 240) >> 16) > 0xC0 && (rgb_at(pics, 350, 240) & 0xFF) < 0x40, "turned square: %06x",
          rgb_at(pics, 350, 240));
    vt_compose(&t, &sp, &v, 0);
    CHECK((rgb_at(pics, 400, 200) & 0xFF) > 0xC0, "turned line, nearest pixels: %06x", rgb_at(pics, 400, 200));

    v.bearing = 0;
    v.zoom = 14.5; /* no pictures at 14.5 yet: those of 14 stand in, scaled */
    missing = vt_compose(&t, &sp, &v, 1);
    CHECK(missing > 0, "zoom 14.5 pictures missing: %d", missing);
    CHECK((rgb_at(pics, 315, 150) >> 16) > 0xC0, "zoom 14 stands in at (315, 150): %06x", rgb_at(pics, 315, 150));
    CHECK(vt_fill(&t, &v, 800, 480, 0, 0.0) == 1, "draws a zoom 14.5 picture");

    v.mx = 0.1; /* outside the map: land, nothing missing */
    v.zoom = 14;
    CHECK(vt_compose(&t, &sp, &v, 1) == 0 && rgb_at(pics, 10, 10) == 0, "outside the map is land");
    CHECK(vt_fill(&t, &v, 800, 480, 2, 0.0) == 0, "nothing to draw outside the map");
    vt_free(&t);

    /* four pictures kept, more on the screen: the frame keeps its own */
    CHECK(vt_init(&t, &m, 4), "small cache");
    v.mx = (13696 + 0.5) / 16384.0;
    v.zoom = 16; /* the 1024 px tile: the screen needs about 12 pictures */
    for (i = 0; i < 30; i++) {
        vt_compose(&t, &sp, &v, 0);
        vt_fill(&t, &v, 800, 480, 0, 0.0);
    }
    CHECK(t.allocated == 4, "never more than 4 pictures: %d", t.allocated);
    vt_free(&t);
    vmap_close(&m);
    fclose(f);
}

int main(int argc, char **argv)
{
    test_filling();
    test_lines();
    test_colours();
    if (argc >= 3) {
        test_map(argv[1], argv[2]);
        test_pictures(argv[1]);
        test_32bit_and_kept(argv[1], argv[2]);
    }
    printf("ok %d of %d\n", g_checks - g_failures, g_checks);
    return g_failures;
}
