/* Draws views of a vector map file through dashboard/src/vmap.c on the build machine and times
 * them: the previews and speed figures of the vector map pilot.
 *
 *   vmap_preview FONT.ttf MAP.vmap OUT_PREFIX [--repeat N] [--size WxH] [--at LAT,LON] VIEW...
 *
 * VIEW is zoom,bearing,style[,dim[,labels]] (style 0 dark, 1 light; labels 1 by default). Each
 * view is drawn N times (default 1) around the middle of the file's area; the last picture is
 * written as OUT_PREFIX-K.rgb (raw RGB, 3 bytes per pixel, top row first) and one line of
 * figures is printed per view. */
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vfont.h"
#include "vmap.h"

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

static unsigned char *slurp(const char *path, unsigned *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char *)malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (unsigned)n;
    return data;
}

int main(int argc, char **argv)
{
    static VFont font;
    VMap map;
    VSurface s;
    unsigned char *ttf;
    unsigned ttf_size;
    FILE *file;
    int repeat = 1, w = 800, h = 480, argi = 4, view_no = 0, at = 0;
    double at_lat = 0, at_lon = 0;
    unsigned short *px;
    unsigned char *rgb;

    if (argc < 5) {
        fprintf(stderr, "usage: vmap_preview FONT MAP OUT_PREFIX [--repeat N] [--size WxH] VIEW...\n");
        return 2;
    }
    ttf = slurp(argv[1], &ttf_size);
    if (ttf == NULL || !vf_load(&font, ttf, ttf_size)) {
        fprintf(stderr, "cannot load font %s\n", argv[1]);
        return 1;
    }
    file = fopen(argv[2], "rb");
    if (file == NULL || !vmap_open(&map, file, file_read, 24)) {
        fprintf(stderr, "cannot open vector map %s\n", argv[2]);
        return 1;
    }
    map.clock = now_us;
    vmap_set_font(&map, &font);
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--repeat") == 0 && argi + 1 < argc)
            repeat = atoi(argv[argi + 1]);
        else if (strcmp(argv[argi], "--size") == 0 && argi + 1 < argc)
            sscanf(argv[argi + 1], "%dx%d", &w, &h);
        else if (strcmp(argv[argi], "--at") == 0 && argi + 1 < argc)
            at = sscanf(argv[argi + 1], "%lf,%lf", &at_lat, &at_lon) == 2;
        argi += 2;
    }
    px = (unsigned short *)malloc((size_t)w * h * 2);
    rgb = (unsigned char *)malloc((size_t)w * h * 3);
    s.px = px;
    s.w = w;
    s.h = h;
    s.stride = w;
    s.px32 = NULL;
    for (; argi < argc; argi++, view_no++) {
        VMapView v;
        VMapStats st, best;
        double zoom = 16, bearing = 0;
        int style = 0, dim = 0, labels = 1, k, ok = 0;
        unsigned sum = 0;
        char path[1024];
        FILE *out;
        sscanf(argv[argi], "%lf,%lf,%d,%d,%d", &zoom, &bearing, &style, &dim, &labels);
        memset(&v, 0, sizeof v);
        if (at)
            vmap_mercator(at_lat, at_lon, &v.mx, &v.my);
        else
            vmap_middle(&map, &v.mx, &v.my);
        v.zoom = zoom;
        v.bearing = bearing;
        v.style = style;
        v.dim = dim;
        v.labels = labels;
        v.cx = w / 2;
        v.cy = h / 2;
        memset(&best, 0, sizeof best);
        for (k = 0; k < repeat; k++) {
            ok = vmap_render(&map, &s, &v, &st);
            sum += st.us_total;
            if (k == 0 || st.us_total < best.us_total)
                best = st;
        }
        printf("view %d zoom %.2f bearing %.0f style %d: %s, best %u us (tiles %u, areas %u, lines %u, labels %u), "
               "mean %u us, %d tiles, %d features, %d of %d labels\n",
               view_no, zoom, bearing, style, ok ? "drawn" : "outside", best.us_total, best.us_tiles, best.us_areas,
               best.us_lines, best.us_labels, sum / (unsigned)repeat, best.tiles_drawn, best.features,
               best.labels_placed, best.labels_tried);
        for (k = 0; k < w * h; k++) {
            unsigned c = px[k];
            rgb[3 * k] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
            rgb[3 * k + 1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
            rgb[3 * k + 2] = (unsigned char)((c & 0x1F) * 255 / 31);
        }
        if (getenv("VMAP_LAYERS")) {
            int li;
            printf("  cells %lu\n", best.cells);
            for (li = 0; li < map.nlayers; li++)
                if (best.us_layer[li] > 0)
                    printf("  layer %2d kind %d: %6u us, %5d features\n", li, map.layers[li].kind, best.us_layer[li],
                           best.layer_features[li]);
        }
        snprintf(path, sizeof path, "%s-%d.rgb", argv[3], view_no);
        out = fopen(path, "wb");
        if (out) {
            fwrite(rgb, 1, (size_t)w * h * 3, out);
            fclose(out);
        }
    }
    vmap_close(&map);
    vf_free(&font);
    fclose(file);
    free(px);
    free(rgb);
    free(ttf);
    return 0;
}
