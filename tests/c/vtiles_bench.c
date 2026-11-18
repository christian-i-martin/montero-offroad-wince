/* Times and checks the map pictures (dashboard/src/vtiles.c) on the build machine.
 *
 *   vtiles_bench FONT.ttf MAP.vmap OUT_PREFIX VIEW...
 *
 * VIEW is zoom,bearing,style (as in vmap_preview). For each view: a full draw (vmap_render), then
 * from an empty cache every picture on the screen, then frames from the pictures (nearest and
 * smooth) with labels on top. Writes OUT_PREFIX-K-full.rgb and OUT_PREFIX-K-pictures.rgb (smooth)
 * and prints one line of figures per view, including how many pixels differ between the two
 * pictures (more than 2 steps in any colour). */
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vfont.h"
#include "vmap.h"
#include "vtiles.h"

#define W 800
#define H 480

static unsigned now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000000u + t.tv_nsec / 1000);
}

static int file_read(void *file, unsigned offset, void *buffer, unsigned size)
{
    FILE *f = (FILE *)file;
    if (fseek(f, (long)offset, SEEK_SET) != 0)
        return -1;
    return (int)fread(buffer, 1, size, f);
}

static void write_rgb(const char *prefix, int k, const char *what, const unsigned short *px)
{
    static unsigned char rgb[W * H * 3];
    char path[1024];
    FILE *out;
    int i;
    for (i = 0; i < W * H; i++) {
        unsigned c = px[i];
        rgb[3 * i] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
        rgb[3 * i + 1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
        rgb[3 * i + 2] = (unsigned char)((c & 0x1F) * 255 / 31);
    }
    snprintf(path, sizeof path, "%s-%d-%s.rgb", prefix, k, what);
    out = fopen(path, "wb");
    if (out) {
        fwrite(rgb, 1, sizeof rgb, out);
        fclose(out);
    }
}

static int channel_diff(unsigned short a, unsigned short b)
{
    int dr = (int)((a >> 11) & 31) - (int)((b >> 11) & 31), dg = ((int)((a >> 5) & 63) - (int)((b >> 5) & 63)) / 2,
        db = (int)(a & 31) - (int)(b & 31), m;
    m = dr < 0 ? -dr : dr;
    if ((dg < 0 ? -dg : dg) > m)
        m = dg < 0 ? -dg : dg;
    if ((db < 0 ? -db : db) > m)
        m = db < 0 ? -db : db;
    return m;
}

int main(int argc, char **argv)
{
    static VFont font;
    static unsigned short full[W * H], pics[W * H];
    VMap map;
    VTiles tiles;
    VSurface sf = {full, W, H, W, NULL}, sp = {pics, W, H, W, NULL};
    FILE *file, *ff;
    unsigned char *ttf;
    long n;
    int a;

    if (argc < 5) {
        fprintf(stderr, "usage: vtiles_bench FONT MAP OUT_PREFIX VIEW...\n");
        return 2;
    }
    ff = fopen(argv[1], "rb");
    file = fopen(argv[2], "rb");
    if (ff == NULL || file == NULL)
        return 1;
    fseek(ff, 0, SEEK_END);
    n = ftell(ff);
    fseek(ff, 0, SEEK_SET);
    ttf = (unsigned char *)malloc((size_t)n);
    if (fread(ttf, 1, (size_t)n, ff) != (size_t)n || !vf_load(&font, ttf, (unsigned)n) ||
        !vmap_open(&map, file, file_read, 128) || !vt_init(&tiles, &map, 480))
        return 1;
    map.clock = now_us;
    vmap_set_font(&map, &font);
    for (a = 4; a < argc; a++) {
        VMapView v;
        VMapStats st;
        unsigned t0, full_us, fill_us, fast_us, smooth_us, labels_cold, labels_warm;
        int filled = 0, missing, differ = 0, i, style = 1;
        double zoom = 16, bearing = 0;
        sscanf(argv[a], "%lf,%lf,%d", &zoom, &bearing, &style);
        memset(&v, 0, sizeof v);
        vmap_middle(&map, &v.mx, &v.my);
        v.zoom = zoom;
        v.bearing = bearing;
        v.style = style;
        v.labels = 1;
        v.cx = W / 2;
        v.cy = H / 2 + 20;
        { /* the middle on a whole world pixel, so screen and picture pixels line up */
            double scale = 256.0 * vmap_exp2(zoom);
            v.mx = (double)(long)(v.mx * scale + 0.5) / scale;
            v.my = (double)(long)(v.my * scale + 0.5) / scale;
        }

        vmap_render(&map, &sf, &v, &st); /* reads the tiles */
        t0 = now_us();
        vmap_render(&map, &sf, &v, &st);
        full_us = now_us() - t0;

        vt_clear(&tiles);
        t0 = now_us();
        while (vt_fill(&tiles, &v, W, H, 0, 0.0))
            filled++;
        fill_us = now_us() - t0;
        t0 = now_us();
        missing = vt_compose(&tiles, &sp, &v, 0);
        fast_us = now_us() - t0;
        t0 = now_us();
        vt_compose(&tiles, &sp, &v, 1);
        smooth_us = now_us() - t0;
        if (map.glyphs) { /* labels from an empty letter cache, then a kept one */
            for (i = 0; i < VM_GLYPH_SLOTS; i++) {
                free(map.glyphs[i].bits);
                map.glyphs[i].bits = NULL;
                map.glyphs[i].key = map.glyphs[i].key2 = 0;
            }
        }
        t0 = now_us();
        vmap_draw(&map, &sp, &v, VM_DRAW_LABELS, &st);
        labels_cold = now_us() - t0;
        vt_compose(&tiles, &sp, &v, 1);
        t0 = now_us();
        vmap_draw(&map, &sp, &v, VM_DRAW_LABELS, &st);
        labels_warm = now_us() - t0;
        {
            static unsigned px32[W * H];
            VSurface s32 = {NULL, W, H, W, px32};
            unsigned c32, kept;
            vt_compose(&tiles, &sp, &v, 1);
            t0 = now_us();
            vmap_draw(&map, &sp, &v, VM_DRAW_LABELS_KEPT, &st);
            kept = now_us() - t0;
            unsigned rows;
            t0 = now_us();
            vt_compose(&tiles, &s32, &v, 1);
            c32 = now_us() - t0;
            tiles.chunk_w = tiles.chunk_h = 0;
            t0 = now_us();
            vt_compose(&tiles, &s32, &v, 1);
            rows = now_us() - t0;
            tiles.chunk_w = VT_CHUNK_W;
            tiles.chunk_h = VT_CHUNK_H;
            printf("view %d: labels kept %u us (kept %d); smooth frame into a 32-bit screen %u us in chunks, %u us "
                   "row by row\n",
                   a - 4, kept, st.labels_kept, c32, rows);
        }
        for (i = 0; i < W * H; i++)
            if (channel_diff(full[i], pics[i]) > 2)
                differ++;
        printf("view %d zoom %.2f bearing %.0f: full %u us; %d pictures in %u us (%u each), %d missing; frame "
               "fast %u us, smooth %u us; labels %u us, then %u us; %.2f%% pixels differ\n",
               a - 4, zoom, bearing, full_us, filled, fill_us, filled ? fill_us / (unsigned)filled : 0, missing, fast_us,
               smooth_us, labels_cold, labels_warm, 100.0 * differ / (W * H));
        write_rgb(argv[3], a - 4, "full", full);
        write_rgb(argv[3], a - 4, "pictures", pics);
    }
    vt_free(&tiles);
    vmap_close(&map);
    vf_free(&font);
    free(ttf);
    fclose(file);
    fclose(ff);
    return 0;
}
