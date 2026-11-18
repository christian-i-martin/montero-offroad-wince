#include "vtiles.h"

#include <stdlib.h>
#include <string.h>

#define LAND_TILE 2
#define HAVE_TILE 1
#define NO_TILE 0
#define BLOCK 16

/* RGB565 to 0x00RRGGBB by two small tables (the high and the low byte each give their own bits of
 * the result), the same as VR_565_TO_32 but in two lookups. */
static unsigned g_wide_hi[256], g_wide_lo[256];
#define WIDEN(c) (g_wide_hi[(c) >> 8] | g_wide_lo[(c)&0xFF])

static void make_wide_tables(void)
{
    unsigned b;
    for (b = 0; b < 256; b++) {
        g_wide_hi[b] = VR_565_TO_32(b << 8);
        g_wide_lo[b] = VR_565_TO_32(b);
    }
}

static unsigned now_us(const VTiles *t)
{
    return t->map->clock ? t->map->clock() : 0;
}

static int round_int(double v)
{
    return v < 0 ? -(int)(-v + 0.5) : (int)(v + 0.5);
}

static long floor_long(double v)
{
    long i = (long)v;
    return (v < 0 && (double)i != v) ? i - 1 : i;
}

static unsigned hash(int z256, int look, long tx, long ty)
{
    return ((unsigned)tx * 73856093u ^ (unsigned)ty * 19349663u ^ (unsigned)z256 * 83492791u ^ (unsigned)look) &
           (VT_BUCKETS - 1);
}

int vt_init(VTiles *t, VMap *m, int count)
{
    int i;
    memset(t, 0, sizeof *t);
    t->pics = (VtPic *)malloc((size_t)count * sizeof(VtPic));
    t->next = (int *)malloc((size_t)count * sizeof(int));
    if (t->pics == NULL || t->next == NULL) {
        vt_free(t);
        return 0;
    }
    for (i = 0; i < count; i++) {
        t->pics[i].used = 0;
        t->pics[i].px = NULL;
        t->next[i] = -1;
    }
    for (i = 0; i < VT_BUCKETS; i++)
        t->bucket[i] = -1;
    t->map = m;
    t->count = count;
    t->chunk_w = VT_CHUNK_W;
    t->chunk_h = VT_CHUNK_H;
    make_wide_tables();
    return 1;
}

void vt_free(VTiles *t)
{
    int i;
    if (t->pics)
        for (i = 0; i < t->allocated; i++)
            free(t->pics[i].px);
    free(t->pics);
    free(t->next);
    memset(t, 0, sizeof *t);
}

void vt_clear(VTiles *t)
{
    int i;
    for (i = 0; i < t->allocated; i++) {
        t->pics[i].used = 0;
        t->next[i] = -1;
    }
    for (i = 0; i < VT_BUCKETS; i++)
        t->bucket[i] = -1;
}

static VtPic *find(VTiles *t, int z256, int look, long tx, long ty)
{
    int i;
    for (i = t->bucket[hash(z256, look, tx, ty)]; i >= 0; i = t->next[i]) {
        VtPic *p = &t->pics[i];
        if (p->used && p->z256 == z256 && p->look == look && p->tx == tx && p->ty == ty)
            return p;
    }
    return NULL;
}

static void unlink_pic(VTiles *t, int k)
{
    VtPic *p = &t->pics[k];
    int *link = &t->bucket[hash(p->z256, p->look, p->tx, p->ty)];
    while (*link >= 0 && *link != k)
        link = &t->next[*link];
    if (*link == k)
        *link = t->next[k];
    t->next[k] = -1;
    p->used = 0;
}

/* A picture's slot: an unused one, a new one, or the least recently used one that the last frame
 * did not use. */
static VtPic *slot_for(VTiles *t, int z256, int look, long tx, long ty)
{
    int i, best = -1;
    unsigned h;
    for (i = 0; i < t->allocated; i++)
        if (!t->pics[i].used) {
            best = i;
            break;
        }
    if (best < 0 && t->allocated < t->count) {
        unsigned short *px = (unsigned short *)malloc(VT_STRIDE * VT_STRIDE * sizeof(unsigned short));
        if (px != NULL) {
            best = t->allocated++;
            t->pics[best].px = px;
            t->pics[best].used = 0;
        }
    }
    if (best < 0) {
        for (i = 0; i < t->allocated; i++) {
            const VtPic *p = &t->pics[i];
            if (p->last_use == t->clock)
                continue;
            if (best < 0 || p->last_use < t->pics[best].last_use)
                best = i;
        }
        if (best < 0)
            return NULL;
        unlink_pic(t, best);
    }
    t->pics[best].used = 1;
    t->pics[best].z256 = z256;
    t->pics[best].look = look;
    t->pics[best].tx = tx;
    t->pics[best].ty = ty;
    t->pics[best].last_use = t->clock;
    h = hash(z256, look, tx, ty);
    t->next[best] = t->bucket[h];
    t->bucket[h] = best;
    return &t->pics[best];
}

/* The pictures of the map file's area at a zoom: x0..x1, y0..y1. */
static void area_tiles(const VMap *m, double scale, long *x0, long *y0, long *x1, long *y1)
{
    double mx0, my0, mx1, my1;
    vmap_mercator(m->north, m->west, &mx0, &my0);
    vmap_mercator(m->south, m->east, &mx1, &my1);
    *x0 = floor_long(mx0 * scale / VT_SIZE);
    *y0 = floor_long(my0 * scale / VT_SIZE);
    *x1 = floor_long(mx1 * scale / VT_SIZE);
    *y1 = floor_long(my1 * scale / VT_SIZE);
}

/* ---------------------------------------------------------------- composing */

typedef struct {
    int z256, look, ok;
    long gx0, gy0;
    int gw, gh;
    const unsigned short *tile[VT_GRID * VT_GRID];
    unsigned char state[VT_GRID * VT_GRID];
    int u0, v0, dux, dvx, duy, dvy;   /* 16.16 picture pixels from (gx0, gy0) * 256 */
    double kz;                        /* picture pixels per screen pixel */
    double cx, cy, sn, cs;            /* the view's middle in world pixels at this zoom; turn */
    unsigned short land;
} Mosaic;

/* Sets up the pictures of zoom z256 that a view needs. extra: pictures beyond the screen's
 * bounding box (for vt_fill). Returns 0 when they do not fit the grid. */
static int mosaic(VTiles *t, Mosaic *M, const VMapView *v, int w, int h, int z256, int smooth, int extra, int mark)
{
    double z = z256 / 256.0, scale = 256.0 * vmap_exp2(z), half = smooth ? 0.5 : 0.0;
    double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30, ax, bx, ay, by, wx, wy;
    long ax0, ay0, ax1, ay1, gx1, gy1;
    int k, i, j;

    M->ok = 0;
    M->z256 = z256;
    M->look = (v->style ? 1 : 0) | (v->dim ? 2 : 0);
    M->land = vr_rgb565(t->map->land[v->style ? 1 : 0], v->dim);
    M->kz = vmap_exp2(z - v->zoom);
    M->cx = v->mx * scale;
    M->cy = v->my * scale;
    vmap_sincos(-v->bearing, &M->sn, &M->cs);
    /* world pixel of screen point (sx, sy): (cx + ax dx + bx dy, cy + ay dx + by dy), d = s - centre */
    ax = M->kz * M->cs;
    bx = M->kz * M->sn;
    ay = -M->kz * M->sn;
    by = M->kz * M->cs;
    for (k = 0; k < 4; k++) {
        double dx = ((k & 1) ? w : 0) - v->cx, dy = ((k & 2) ? h : 0) - v->cy;
        wx = M->cx + ax * dx + bx * dy;
        wy = M->cy + ay * dx + by * dy;
        if (wx < minx) minx = wx;
        if (wx > maxx) maxx = wx;
        if (wy < miny) miny = wy;
        if (wy > maxy) maxy = wy;
    }
    M->gx0 = floor_long((minx - 1) / VT_SIZE) - extra;
    M->gy0 = floor_long((miny - 1) / VT_SIZE) - extra;
    gx1 = floor_long((maxx + 1) / VT_SIZE) + extra;
    gy1 = floor_long((maxy + 1) / VT_SIZE) + extra;
    M->gw = (int)(gx1 - M->gx0 + 1);
    M->gh = (int)(gy1 - M->gy0 + 1);
    if (M->gw > VT_GRID || M->gh > VT_GRID || M->gw <= 0 || M->gh <= 0)
        return 0;
    wx = M->cx + ax * (0.5 - v->cx) + bx * (0.5 - v->cy) - half - M->gx0 * (double)VT_SIZE;
    wy = M->cy + ay * (0.5 - v->cx) + by * (0.5 - v->cy) - half - M->gy0 * (double)VT_SIZE;
    M->u0 = round_int(wx * 65536.0);
    M->v0 = round_int(wy * 65536.0);
    M->dux = round_int(ax * 65536.0);
    M->dvx = round_int(ay * 65536.0);
    M->duy = round_int(bx * 65536.0);
    M->dvy = round_int(by * 65536.0);
    area_tiles(t->map, scale, &ax0, &ay0, &ax1, &ay1);
    for (j = 0; j < M->gh; j++) {
        for (i = 0; i < M->gw; i++) {
            long tx = M->gx0 + i, ty = M->gy0 + j;
            int n = j * VT_GRID + i;
            VtPic *p;
            M->tile[n] = NULL;
            if (tx < ax0 || tx > ax1 || ty < ay0 || ty > ay1) {
                M->state[n] = LAND_TILE;
                continue;
            }
            p = find(t, z256, M->look, tx, ty);
            M->state[n] = p ? HAVE_TILE : NO_TILE;
            if (p) {
                M->tile[n] = p->px;
                if (mark)
                    p->last_use = t->clock;
            }
        }
    }
    M->ok = 1;
    return 1;
}

/* Screen position of a picture's middle, and whether it shows on a w x h screen. */
static int on_screen(const Mosaic *M, const VMapView *v, long tx, long ty, int w, int h, double *dist2)
{
    double wx = (tx + 0.5) * VT_SIZE - M->cx, wy = (ty + 0.5) * VT_SIZE - M->cy, r = 182.0 / M->kz;
    /* inverse of the mapping in mosaic(): screen = centre + R (W - C) / kz */
    double sx = v->cx + (M->cs * wx - M->sn * wy) / M->kz, sy = v->cy + (M->sn * wx + M->cs * wy) / M->kz;
    double dx = sx - v->cx, dy = sy - v->cy;
    *dist2 = dx * dx + dy * dy;
    return sx > -r && sy > -r && sx < w + r && sy < h + r;
}

static int spread_mix(unsigned a, unsigned b, int f)
{
    return (int)((a + (((b - a) * (unsigned)f) >> 5)) & 0x07E0F81Fu);
}

static unsigned spread(unsigned short c)
{
    return ((unsigned)c | (unsigned)c << 16) & 0x07E0F81Fu;
}

/* The picture under 16.16 position (u, v) of the mosaic: its state, and in *p its pixels. */
static int cell(const Mosaic *M, int u, int v, const unsigned short **p)
{
    int tx, ty, n;
    *p = NULL;
    if (u < 0 || v < 0)
        return NO_TILE;
    tx = u >> 24;
    ty = v >> 24;
    if (tx >= M->gw || ty >= M->gh)
        return NO_TILE;
    n = ty * VT_GRID + tx;
    *p = M->tile[n];
    return M->state[n];
}

/* One sample of the mosaic at (u, v): 1 and *c, or 0 when its picture is not drawn yet. Smooth
 * samples mix the 2 x 2 pixels from (u, v) on; the border row and column of each picture (copied
 * from its neighbours) hold the right and lower ones. */
static int sample(const Mosaic *M, int u, int v, int smooth, unsigned short *c)
{
    const unsigned short *p;
    int state = cell(M, u, v, &p);
    if (state == LAND_TILE) {
        *c = M->land;
        return 1;
    }
    if (state != HAVE_TILE)
        return 0;
    p += ((v >> 16) & 0xFF) * VT_STRIDE + ((u >> 16) & 0xFF);
    if (!smooth) {
        *c = p[0];
    } else {
        int fu = (u >> 11) & 31, fv = (v >> 11) & 31;
        unsigned top = (unsigned)spread_mix(spread(p[0]), spread(p[1]), fu);
        unsigned bottom = (unsigned)spread_mix(spread(p[VT_STRIDE]), spread(p[VT_STRIDE + 1]), fu);
        top = (unsigned)spread_mix(top, bottom, fv);
        *c = (unsigned short)(top | top >> 16);
    }
    return 1;
}

/* A run of n screen pixels inside one picture, into a 16-bit (d16) or 32-bit (d32) row. */
static void run(const unsigned short *src, int u, int v, int du, int dv, int n, int smooth, unsigned short *d16,
                unsigned *d32)
{
    int k;
    if (!smooth) {
        if (d32) {
            for (k = 0; k < n; k++, u += du, v += dv) {
                unsigned c = src[((v >> 16) & 0xFF) * VT_STRIDE + ((u >> 16) & 0xFF)];
                d32[k] = WIDEN(c);
            }
        } else {
            for (k = 0; k < n; k++, u += du, v += dv)
                d16[k] = src[((v >> 16) & 0xFF) * VT_STRIDE + ((u >> 16) & 0xFF)];
        }
        return;
    }
    for (k = 0; k < n; k++, u += du, v += dv) {
        const unsigned short *p = src + ((v >> 16) & 0xFF) * VT_STRIDE + ((u >> 16) & 0xFF);
        int fu = (u >> 11) & 31, fv = (v >> 11) & 31;
        unsigned top = (unsigned)spread_mix(spread(p[0]), spread(p[1]), fu);
        unsigned bottom = (unsigned)spread_mix(spread(p[VT_STRIDE]), spread(p[VT_STRIDE + 1]), fu);
        unsigned short c;
        top = (unsigned)spread_mix(top, bottom, fv);
        c = (unsigned short)(top | top >> 16);
        if (d32)
            d32[k] = WIDEN(c);
        else
            d16[k] = c;
    }
}

/* How many steps of d from c stay in the same picture (c >> 24), at most limit. */
static int steps_in(int c, int d, int limit)
{
    long long k;
    if (d == 0)
        return limit;
    if (d > 0)
        k = ((((long long)(c >> 24) + 1) << 24) - c + d - 1) / d;
    else
        k = (c - ((long long)(c >> 24) << 24)) / -(long long)d + 1;
    return k < limit ? (int)k : limit;
}

static Mosaic g_main_m, g_back_m;

/* Screen pixels x0 to x1 - 1 of row y, in runs of pixels that come from one picture. */
static void compose_span(const Mosaic *M, const Mosaic *F, int have_back, VSurface *s, int y, int x0, int x1, int smooth)
{
    unsigned short *d16 = s->px32 ? NULL : s->px + (long)y * s->stride;
    unsigned *d32 = s->px32 ? s->px32 + (long)y * s->stride : NULL;
    int u = M->u0 + y * M->duy + x0 * M->dux, vv = M->v0 + y * M->dvy + x0 * M->dvx, x = x0;
    while (x < x1) {
        int n = steps_in(u, M->dux, x1 - x), k;
        const unsigned short *p;
        int state;
        n = steps_in(vv, M->dvx, n);
        state = cell(M, u, vv, &p);
        if (state == HAVE_TILE) {
            if (M->dux == 65536 && M->dvx == 0 && !smooth) {
                /* straight across: whole rows of the picture */
                const unsigned short *src = p + ((vv >> 16) & 0xFF) * VT_STRIDE + ((u >> 16) & 0xFF);
                if (d32)
                    for (k = 0; k < n; k++)
                        d32[x + k] = WIDEN(src[k]);
                else
                    memcpy(d16 + x, src, (size_t)n * 2);
            } else {
                run(p, u, vv, M->dux, M->dvx, n, smooth, d16 ? d16 + x : NULL, d32 ? d32 + x : NULL);
            }
        } else {
            for (k = 0; k < n; k++) {
                unsigned short c = M->land;
                if (state == NO_TILE && have_back) {
                    int fx = x + k;
                    if (!sample(F, F->u0 + fx * F->dux + y * F->duy, F->v0 + fx * F->dvx + y * F->dvy, smooth, &c))
                        c = M->land;
                }
                if (d32)
                    d32[x + k] = WIDEN(c);
                else
                    d16[x + k] = c;
            }
        }
        x += n;
        u += n * M->dux;
        vv += n * M->dvx;
    }
}

int vt_compose(VTiles *t, VSurface *s, const VMapView *v, int smooth)
{
    Mosaic *M = &g_main_m, *F = &g_back_m;
    int z256 = round_int(v->zoom * 256.0), have_back = 0, missing = 0, i, j, x0, y0, cw, ch;
    double lowest, highest;

    vmap_zoom_range(t->map, &lowest, &highest);
    if (z256 < round_int(lowest * 256.0))
        z256 = round_int(lowest * 256.0);
    if (z256 > round_int(highest * 256.0))
        z256 = round_int(highest * 256.0);
    if (z256 != t->cur_z256) {
        if (t->cur_z256)
            t->prev_z256 = t->cur_z256;
        t->cur_z256 = z256;
    }
    t->clock++;
    /* neither turned nor scaled: copy the pixels exactly (mixing neighbours would only blur) */
    if (z256 == v->zoom * 256.0 && (v->bearing == 0.0 || v->bearing == 360.0))
        smooth = 0;
    if (!mosaic(t, M, v, s->w, s->h, z256, smooth, 0, 1)) {
        vr_fill_surface(s, t->map->land[v->style ? 1 : 0], v->dim);
        t->missing = 0;
        return 0;
    }
    if (t->prev_z256 && t->prev_z256 != z256)
        have_back = mosaic(t, F, v, s->w, s->h, t->prev_z256, smooth, 0, 1);
    for (j = 0; j < M->gh; j++)
        for (i = 0; i < M->gw; i++) {
            double d2;
            if (M->state[j * VT_GRID + i] == NO_TILE &&
                on_screen(M, v, M->gx0 + i, M->gy0 + j, s->w, s->h, &d2))
                missing++;
        }
    t->missing = missing;

    /* Straight across, whole rows; turned or scaled, in chunks of chunk_w x chunk_h pixels: each
     * screen row then crosses only a few rows of a picture, which stay in the processor's cache for
     * the next row (a whole turned row crosses hundreds). */
    cw = s->w;
    ch = 1;
    if (!(M->dux == 65536 && M->dvx == 0) && t->chunk_w > 0 && t->chunk_h > 0) {
        cw = t->chunk_w;
        ch = t->chunk_h;
    }
    for (y0 = 0; y0 < s->h; y0 += ch) {
        int y1 = y0 + ch < s->h ? y0 + ch : s->h;
        for (x0 = 0; x0 < s->w; x0 += cw) {
            int x1 = x0 + cw < s->w ? x0 + cw : s->w, y;
            for (y = y0; y < y1; y++)
                compose_span(M, F, have_back, s, y, x0, x1, smooth);
        }
    }
    return missing;
}

/* ---------------------------------------------------------------- drawing pictures */

static int draw_pic(VTiles *t, const VMapView *v, int z256, int look, long tx, long ty)
{
    VtPic *p;
    VSurface ps;
    VMapView pv;
    double scale = 256.0 * vmap_exp2(z256 / 256.0);
    unsigned start = now_us(t);
    p = slot_for(t, z256, look, tx, ty);
    if (p == NULL)
        return 0;
    ps.px = p->px;
    ps.px32 = NULL;
    ps.w = ps.h = ps.stride = VT_STRIDE; /* the picture and one more row and column: its border */
    memset(&pv, 0, sizeof pv);
    pv.mx = (tx * (double)VT_SIZE + VT_SIZE / 2) / scale;
    pv.my = (ty * (double)VT_SIZE + VT_SIZE / 2) / scale;
    pv.zoom = z256 / 256.0;
    pv.style = v->style;
    pv.dim = v->dim;
    pv.cx = pv.cy = VT_SIZE / 2;
    vmap_draw(t->map, &ps, &pv, VM_DRAW_SHAPES, NULL);
    t->drawn++;
    t->us_last = now_us(t) - start;
    t->us_drawing += t->us_last;
    return 1;
}

int vt_fill(VTiles *t, const VMapView *v, int w, int h, int ring, double zoom_step)
{
    static Mosaic M;
    VMapView zv = *v;
    double lowest, highest, best_d = 1e30;
    int z256, i, j, found = 0, best_visible = 0;
    long best_x = 0, best_y = 0;

    vmap_zoom_range(t->map, &lowest, &highest);
    zv.zoom = v->zoom + zoom_step;
    if (zv.zoom < lowest || zv.zoom > highest)
        return 0;
    z256 = round_int(zv.zoom * 256.0);
    if (!mosaic(t, &M, &zv, w, h, z256, 0, ring, 0) && !mosaic(t, &M, &zv, w, h, z256, 0, 0, 0))
        return 0;
    for (j = 0; j < M.gh; j++) {
        for (i = 0; i < M.gw; i++) {
            double d2;
            int visible;
            if (M.state[j * VT_GRID + i] != NO_TILE)
                continue;
            visible = on_screen(&M, &zv, M.gx0 + i, M.gy0 + j, w, h, &d2);
            if (!visible && ring == 0)
                continue;
            if (!found || (visible && !best_visible) || (visible == best_visible && d2 < best_d)) {
                found = 1;
                best_visible = visible;
                best_d = d2;
                best_x = M.gx0 + i;
                best_y = M.gy0 + j;
            }
        }
    }
    if (!found)
        return 0;
    return draw_pic(t, &zv, z256, M.look, best_x, best_y);
}
