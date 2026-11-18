#include "vmap.h"

#include <stdlib.h>
#include <string.h>

/* zlib inflating from stb_image (pngdec.c), already in the dashboard for the PNG tiles */
int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

#define VM_PI 3.14159265358979323846
#define FLAG_ROUND 1
#define FLAG_UPPER 2
#define NO_TEXT 0xFFFFFFFFu
#define HIGHEST_ZOOM 19.5
#define GRID 8                  /* label overlap cells, pixels */
#define MAX_CANDIDATES 2048
#define MAX_LABEL_GLYPHS 64
#define MAX_TILE_BYTES (8u << 20)

static unsigned le16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static unsigned now_us(const VMap *m)
{
    return m->clock ? m->clock() : 0;
}

/* ---------------------------------------------------------------- maths without a library */

static double sin_rad(double x)
{
    double x2;
    while (x > VM_PI)
        x -= 2 * VM_PI;
    while (x < -VM_PI)
        x += 2 * VM_PI;
    if (x > VM_PI / 2)
        x = VM_PI - x;
    else if (x < -VM_PI / 2)
        x = -VM_PI - x;
    x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}

void vmap_sincos(double degrees, double *s, double *c)
{
    double r = degrees * VM_PI / 180.0;
    *s = sin_rad(r);
    *c = sin_rad(r + VM_PI / 2);
}

static double exp2_of(double x)
{
    double scale = 1.0, y, term, sum;
    int n = 0, k;
    while (x < 0) {
        x += 1;
        n--;
    }
    while (x >= 1) {
        x -= 1;
        n++;
    }
    y = x * 0.69314718055994530942;
    sum = term = 1.0;
    for (k = 1; k < 14; k++) {
        term *= y / k;
        sum += term;
    }
    for (; n > 0; n--)
        scale *= 2;
    for (; n < 0; n++)
        scale *= 0.5;
    return sum * scale;
}

double vmap_exp2(double x)
{
    return exp2_of(x);
}

static double ln_of(double x)
{
    double s, s2, term, sum = 0;
    int e = 0, k;
    if (x <= 0)
        return -1e9;
    while (x > 1.41421356237) {
        x *= 0.5;
        e++;
    }
    while (x < 0.70710678118) {
        x *= 2;
        e--;
    }
    s = (x - 1) / (x + 1);
    s2 = s * s;
    term = s;
    for (k = 1; k < 30; k += 2) {
        sum += term / k;
        term *= s2;
    }
    return 2 * sum + e * 0.69314718055994530942;
}

void vmap_mercator(double lat, double lon, double *mx, double *my)
{
    double r, s, c;
    if (lat > 85.0511)
        lat = 85.0511;
    if (lat < -85.0511)
        lat = -85.0511;
    r = (45.0 + lat / 2.0);
    vmap_sincos(r, &s, &c);
    *mx = (lon + 180.0) / 360.0;
    *my = (1.0 - ln_of(s / c) / VM_PI) / 2.0;
}

void vmap_middle(const VMap *m, double *mx, double *my)
{
    double x0, y0, x1, y1;
    vmap_mercator(m->north, m->west, &x0, &y0);
    vmap_mercator(m->south, m->east, &x1, &y1);
    *mx = (x0 + x1) / 2;
    *my = (y0 + y1) / 2;
}

void vmap_zoom_range(const VMap *m, double *lowest, double *highest)
{
    int i, low = 99 * 8;
    for (i = 0; i < m->nlevels; i++)
        if (m->levels[i].first8 < low)
            low = m->levels[i].first8;
    *lowest = low / 8.0;
    *highest = HIGHEST_ZOOM;
}

/* ---------------------------------------------------------------- file */

int vmap_open(VMap *m, void *file, vmap_read_fn read, int cache_tiles)
{
    unsigned char h[64];
    unsigned lay_off, lay_n, lev_off, lev_n, end, pos, i;

    memset(m, 0, sizeof *m);
    if (read(file, 0, h, 64) != 64 || memcmp(h, "MTVM", 4) != 0 || le16(h + 4) != 1)
        return 0;
    lay_off = le32(h + 8);
    lay_n = le32(h + 12);
    lev_off = le32(h + 16);
    lev_n = le32(h + 20);
    if (lay_n == 0 || lay_n > VM_MAX_LAYERS || lev_n == 0 || lev_n > VM_MAX_LEVELS)
        return 0;
    m->land[0] = le32(h + 24);
    m->land[1] = le32(h + 28);
    m->west = (int)le32(h + 32) / 1e6;
    m->south = (int)le32(h + 36) / 1e6;
    m->east = (int)le32(h + 40) / 1e6;
    m->north = (int)le32(h + 44) / 1e6;
    m->zoom_start8 = (int)le16(h + 48);
    m->zoom_count = (int)le16(h + 50);
    if (m->zoom_count < 2 || m->zoom_count > 400)
        return 0;

    /* everything before the first tile: rules, levels and tile indexes */
    end = lev_off + 24 * lev_n;
    for (i = 0; i < lev_n; i++) {
        unsigned char rec[24];
        unsigned stop;
        if (read(file, lev_off + 24 * i, rec, 24) != 24)
            return 0;
        stop = le32(rec + 20) + 12u * le16(rec + 2) * le16(rec + 4);
        if (stop > end)
            end = stop;
    }
    if (end > (4u << 20))
        return 0;
    m->head = (unsigned char *)malloc(end);
    if (m->head == NULL || read(file, 0, m->head, end) != (int)end) {
        vmap_close(m);
        return 0;
    }
    m->head_size = end;

    pos = lay_off;
    for (i = 0; i < lay_n; i++) {
        VmLayer *L = &m->layers[i];
        const unsigned char *p = m->head + pos;
        int k;
        if (pos + 46 + 4u * m->zoom_count > end) {
            vmap_close(m);
            return 0;
        }
        L->kind = p[0];
        L->flags = p[1];
        L->ndash = p[2] > 4 ? 4 : p[2];
        L->anchor = p[3];
        L->first8 = (int)le16(p + 4);
        L->end8 = (int)le16(p + 6);
        L->colour[0] = le32(p + 8);
        L->colour[1] = le32(p + 12);
        L->colour2[0] = le32(p + 16);
        L->colour2[1] = le32(p + 20);
        L->opacity = (int)le16(p + 24);
        L->offset_x = (short)le16(p + 26);
        L->offset_y = (short)le16(p + 28);
        for (k = 0; k < 4; k++)
            L->dashes[k] = (int)le16(p + 30 + 2 * k);
        L->spacing = (int)le16(p + 38);
        L->max_angle = (int)le16(p + 40);
        L->max_width = (int)le16(p + 42);
        L->icon = (int)le16(p + 44);
        L->size = (const unsigned short *)(p + 46);
        L->size2 = (const unsigned short *)(p + 46 + 2 * m->zoom_count);
        pos += 46 + 4 * m->zoom_count;
        pos = (pos + 3) & ~3u;
    }
    m->nlayers = (int)lay_n;
    for (i = 0; i < lev_n; i++) {
        const unsigned char *p = m->head + lev_off + 24 * i;
        VmLevel *lv = &m->levels[i];
        lv->zoom = p[0];
        lv->cols = (int)le16(p + 2);
        lv->rows = (int)le16(p + 4);
        lv->x0 = le32(p + 8);
        lv->y0 = le32(p + 12);
        lv->first8 = (int)le16(p + 16);
        lv->end8 = (int)le16(p + 18);
        lv->index = le32(p + 20);
        if (lv->zoom > 20) {
            vmap_close(m);
            return 0;
        }
    }
    m->nlevels = (int)lev_n;

    if (cache_tiles < 4)
        cache_tiles = 4;
    m->cache = (VmTile *)malloc((size_t)cache_tiles * sizeof(VmTile));
    if (m->cache == NULL) {
        vmap_close(m);
        return 0;
    }
    for (i = 0; i < (unsigned)cache_tiles; i++) {
        m->cache[i].used = 0;
        m->cache[i].data = NULL;
    }
    m->cache_size = cache_tiles;
    m->file = file;
    m->read = read;
    return 1;
}

void vmap_close(VMap *m)
{
    int i;
    if (m->cache)
        for (i = 0; i < m->cache_size; i++)
            free(m->cache[i].data);
    free(m->cache);
    free(m->head);
    free(m->packed);
    free(m->pts);
    free(m->cum);
    free(m->dir);
    free(m->kept);
    free(m->grid);
    free(m->mask);
    if (m->glyphs) {
        int k;
        for (k = 0; k < VM_GLYPH_SLOTS; k++)
            free(m->glyphs[k].bits);
        free(m->glyphs);
    }
    for (i = 0; i < VICON_COUNT; i++)
        free(m->markers[i]);
    if (m->raster.acc)
        vr_free(&m->raster);
    memset(m, 0, sizeof *m);
}

void vmap_set_font(VMap *m, VFont *f)
{
    m->font = f;
}

/* Checks a tile's sections against its length, so a damaged file cannot make us read past it. */
static int parse_tile(VmTile *t, unsigned size, int nlayers)
{
    const unsigned char *d = t->data;
    unsigned nf, nd, np, nparts, ntext, nindex, i;
    unsigned long long pos;
    if (size < 20)
        return 0;
    nf = le16(d);
    nd = le16(d + 2);
    np = le32(d + 4);
    nparts = le32(d + 8);
    ntext = le32(d + 12);
    nindex = le32(d + 16);
    pos = 20;
    t->features = (const VmFeature *)(d + pos);
    pos += 28ull * nf;
    if (pos > size)
        return 0;
    t->parts = (const unsigned *)(d + pos);
    pos += 4ull * nparts;
    if (pos > size)
        return 0;
    t->draws = (const VmDraw *)(d + pos);
    pos += 8ull * nd;
    if (pos > size)
        return 0;
    t->points = (const short *)(d + pos);
    pos += 4ull * np;
    if (pos > size)
        return 0;
    t->indexes = (const unsigned short *)(d + pos);
    pos += 2ull * nindex;
    pos = (pos + 3) & ~3ull;
    if (pos + ntext > size)
        return 0;
    t->text = (const char *)(d + pos);
    t->text_size = ntext;
    if (ntext > 0 && t->text[ntext - 1] != 0)
        return 0;
    for (i = 0; i < nf; i++) {
        const VmFeature *f = &t->features[i];
        unsigned k, count = 0;
        if ((unsigned long long)f->first_part + f->nparts > nparts)
            return 0;
        for (k = 0; k < f->nparts; k++)
            count += t->parts[f->first_part + k];
        if ((unsigned long long)f->first_point + count > np)
            return 0;
        if (f->text != NO_TEXT && f->text >= ntext)
            return 0;
    }
    for (i = 0; i < nd; i++) {
        const VmDraw *dr = &t->draws[i];
        unsigned k;
        if (dr->layer >= nlayers || (unsigned long long)dr->first + dr->count > nindex)
            return 0;
        for (k = 0; k < dr->count; k++)
            if (t->indexes[dr->first + k] >= nf)
                return 0;
    }
    t->nfeatures = (int)nf;
    t->ndraws = (int)nd;
    return 1;
}

static const VmTile *get_tile(VMap *m, int level, unsigned x, unsigned y, VMapStats *st)
{
    const VmLevel *lv = &m->levels[level];
    VmTile *slot = NULL;
    const unsigned char *entry;
    unsigned off, len, raw;
    int i;

    if (x < lv->x0 || y < lv->y0 || x - lv->x0 >= (unsigned)lv->cols || y - lv->y0 >= (unsigned)lv->rows)
        return NULL;
    for (i = 0; i < m->cache_size; i++) {
        VmTile *t = &m->cache[i];
        if (t->used && t->level == level && t->x == x && t->y == y) {
            t->last_use = ++m->use_clock;
            return t->nfeatures ? t : NULL;
        }
    }
    for (i = 0; i < m->cache_size; i++) {
        VmTile *t = &m->cache[i];
        if (!t->used) {
            slot = t;
            break;
        }
        if (slot == NULL || t->last_use < slot->last_use)
            slot = t;
    }
    free(slot->data);
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    slot->level = level;
    slot->x = x;
    slot->y = y;
    slot->last_use = ++m->use_clock;
    entry = m->head + lv->index + 12u * ((y - lv->y0) * (unsigned)lv->cols + (x - lv->x0));
    if (entry + 12 > m->head + m->head_size)
        return NULL;
    off = le32(entry);
    len = le32(entry + 4);
    raw = le32(entry + 8);
    if (len == 0)
        return NULL;
    if (len > MAX_TILE_BYTES || raw > MAX_TILE_BYTES || raw < 20) {
        st->load_errors++;
        return NULL;
    }
    if (len > m->packed_cap) {
        free(m->packed);
        m->packed = (unsigned char *)malloc(len);
        m->packed_cap = m->packed ? len : 0;
        if (m->packed == NULL) {
            st->load_errors++;
            return NULL;
        }
    }
    slot->data = (unsigned char *)malloc(raw);
    if (slot->data == NULL || m->read(m->file, off, m->packed, len) != (int)len ||
        stbi_zlib_decode_buffer((char *)slot->data, (int)raw, (const char *)m->packed, (int)len) != (int)raw ||
        !parse_tile(slot, raw, m->nlayers)) {
        free(slot->data);
        slot->data = NULL;
        slot->nfeatures = 0;
        st->load_errors++;
        return NULL;
    }
    st->tiles_loaded++;
    return slot->nfeatures ? slot : NULL;
}

/* ---------------------------------------------------------------- drawing */

typedef struct {
    const VmTile *t;
    int a, b, c, d;           /* 1/256 pixel per tile unit, x 65536 */
    int tx, ty;               /* 1/256 pixel */
    int cursor;               /* next draw list to look at */
} Vis;

static void tpoint(const Vis *v, int lx, int ly, int *x, int *y)
{
    *x = v->tx + (int)(((long long)v->a * lx + (long long)v->b * ly) >> 16);
    *y = v->ty + (int)(((long long)v->c * lx + (long long)v->d * ly) >> 16);
}

/* A size table at zoom z256 (zoom x 256), linear between its 1/8 steps: pixels x 64. */
static int table_at(const VMap *m, const unsigned short *t, int z256)
{
    int idx = z256 * 8 - m->zoom_start8 * 256, i, f;
    if (idx <= 0)
        return t[0];
    i = idx >> 8;
    f = idx & 255;
    if (i >= m->zoom_count - 1)
        return t[m->zoom_count - 1];
    return (t[i] * (256 - f) + t[i + 1] * f) >> 8;
}

static const VmDraw *draw_list(Vis *v, int layer)
{
    const VmTile *t = v->t;
    while (v->cursor < t->ndraws && t->draws[v->cursor].layer < layer)
        v->cursor++;
    if (v->cursor < t->ndraws && t->draws[v->cursor].layer == layer)
        return &t->draws[v->cursor];
    return NULL;
}

/* 1 when the feature's box, mapped to the screen and widened by pad (1/256 pixel), meets it. */
static int on_screen(const Vis *v, const VmFeature *f, int pad, int w, int h)
{
    int xs[4], ys[4], k, x0, x1, y0, y1;
    tpoint(v, f->box[0], f->box[1], &xs[0], &ys[0]);
    tpoint(v, f->box[2], f->box[1], &xs[1], &ys[1]);
    tpoint(v, f->box[0], f->box[3], &xs[2], &ys[2]);
    tpoint(v, f->box[2], f->box[3], &xs[3], &ys[3]);
    x0 = x1 = xs[0];
    y0 = y1 = ys[0];
    for (k = 1; k < 4; k++) {
        if (xs[k] < x0) x0 = xs[k];
        if (xs[k] > x1) x1 = xs[k];
        if (ys[k] < y0) y0 = ys[k];
        if (ys[k] > y1) y1 = ys[k];
    }
    return x1 + pad >= 0 && y1 + pad >= 0 && x0 - pad <= (w << VR_SHIFT) && y0 - pad <= (h << VR_SHIFT);
}

static int grow_points(VMap *m, int n)
{
    int cap;
    if (n <= m->pts_cap)
        return 1;
    cap = m->pts_cap ? m->pts_cap : 1024;
    while (cap < n)
        cap *= 2;
    free(m->pts);
    free(m->cum);
    free(m->dir);
    m->pts = (int *)malloc((size_t)cap * 2 * sizeof(int));
    m->cum = (int *)malloc((size_t)cap * sizeof(int));
    m->dir = (int *)malloc((size_t)cap * 2 * sizeof(int));
    if (m->pts == NULL || m->cum == NULL || m->dir == NULL) {
        free(m->pts);
        free(m->cum);
        free(m->dir);
        m->pts = m->cum = m->dir = NULL;
        m->pts_cap = 0;
        return 0;
    }
    m->pts_cap = cap;
    return 1;
}

/* One part's points on the screen, dropping points closer than `near` (1/256 pixel) to the last
 * one kept (the end point always stays). Returns the count, in m->pts. */
static int screen_points(VMap *m, const Vis *v, const short *p, int n, int near)
{
    int i, k = 0;
    if (!grow_points(m, n))
        return 0;
    for (i = 0; i < n; i++) {
        int x, y;
        tpoint(v, p[2 * i], p[2 * i + 1], &x, &y);
        if (k > 0 && i < n - 1) {
            int dx = x - m->pts[2 * k - 2], dy = y - m->pts[2 * k - 1];
            if (dx < near && dx > -near && dy < near && dy > -near)
                continue;
        }
        m->pts[2 * k] = x;
        m->pts[2 * k + 1] = y;
        k++;
    }
    return k;
}

/* Draws one layer of every visible tile in one filling pass (two for dots: outline, then fill). */
static void draw_layer(VMap *m, VSurface *s, Vis *vis, int nvis, int li, int z256, const VMapView *view,
                       VMapStats *st)
{
    const VmLayer *L = &m->layers[li];
    VRaster *r = &m->raster;
    int size = table_at(m, L->size, z256) * 4;      /* 1/256 pixel */
    int size2 = table_at(m, L->size2, z256) * 4;
    int style = view->style ? 1 : 0, pass, passes = 1, i;
    VrPaint paint;

    if (L->kind == VM_LINE && size < VR_ONE / 8)
        return;
    if (L->kind == VM_DOT) {
        if (size <= 0)
            return;
        passes = size2 > 0 ? 2 : 1;
    }
    for (pass = 0; pass < passes; pass++) {
        int outline = passes == 2 && pass == 0;
        int dashes[4], k;
        unsigned rgb = outline ? L->colour2[style] : L->colour[style];
        vr_paint_init(&paint, s, rgb, L->opacity, view->dim);
        vr_set_clip(r, s->w, s->h);
        for (k = 0; k < L->ndash; k++)
            dashes[k] = (int)(((long long)L->dashes[k] * size) >> 6);
        for (i = 0; i < nvis; i++) {
            Vis *v = &vis[i];
            int j;
            const VmDraw *dl = draw_list(v, li); /* the fill pass of a dot layer finds the same list */
            if (dl == NULL)
                continue;
            for (j = 0; j < dl->count; j++) {
                const VmFeature *f = &v->t->features[v->t->indexes[dl->first + j]];
                const short *p = v->t->points + 2 * f->first_point;
                int part, pad = (L->kind == VM_AREA ? 0 : size + size2) + VR_ONE;
                if (!on_screen(v, f, pad, s->w, s->h))
                    continue;
                st->features++;
                for (part = 0; part < f->nparts; part++) {
                    int n = (int)v->t->parts[f->first_part + part];
                    if (L->kind == VM_AREA) {
                        int q = screen_points(m, v, p, n, VR_ONE / 4), e;
                        if (q >= 3) {
                            vr_move_to(r, m->pts[0], m->pts[1]);
                            for (e = 1; e < q; e++)
                                vr_line_to(r, m->pts[2 * e], m->pts[2 * e + 1]);
                            vr_close(r);
                        }
                    } else if (L->kind == VM_LINE) {
                        int q = screen_points(m, v, p, n, size < 3 * VR_ONE ? VR_ONE : VR_ONE / 2);
                        if (q >= 2)
                            vr_stroke(r, m->pts, q, size, (L->flags & FLAG_ROUND) != 0, 0, dashes, L->ndash);
                    } else {
                        int e;
                        for (e = 0; e < n; e++) {
                            int x, y;
                            tpoint(v, p[2 * e], p[2 * e + 1], &x, &y);
                            vr_disc(r, x, y, outline ? size + size2 : size);
                        }
                    }
                    p += 2 * n;
                }
            }
        }
        vr_sweep(r, vr_paint_span, &paint);
    }
}

/* ---------------------------------------------------------------- labels */

typedef struct {
    const Vis *vis;
    const VmFeature *f;
} Cand;

static Cand g_cand[MAX_CANDIDATES];

static unsigned text_char(const char **p, int upper)
{
    unsigned c = vf_next_char(p);
    if (upper && c >= 'a' && c <= 'z')
        c -= 32;
    return c;
}

/* Width of text up to end (or a line break), font units. */
static int text_units(VFont *f, const char *s, const char *end, int upper)
{
    int w = 0;
    while (*s && s != end && *s != '\n')
        w += vf_advance(f, vf_glyph(f, text_char(&s, upper)));
    return w;
}

static int ensure_masks(VMap *m, int bytes)
{
    if (bytes <= m->mask_cap)
        return 1;
    free(m->mask);
    m->mask = (unsigned char *)malloc((size_t)bytes * 3);
    if (m->mask == NULL) {
        m->mask_cap = 0;
        return 0;
    }
    m->halo = m->mask + bytes;
    m->mask_cap = bytes;
    return 1;
}

static int grid_free(const VMap *m, int x0, int y0, int x1, int y1)
{
    int gx, gy;
    for (gy = y0 / GRID; gy <= (y1 - 1) / GRID && gy < m->grid_h; gy++)
        for (gx = x0 / GRID; gx <= (x1 - 1) / GRID && gx < m->grid_w; gx++)
            if (gx >= 0 && gy >= 0 && m->grid[gy * m->grid_w + gx])
                return 0;
    return 1;
}

static void grid_mark(VMap *m, int x0, int y0, int x1, int y1)
{
    int gx, gy;
    for (gy = y0 / GRID; gy <= (y1 - 1) / GRID && gy < m->grid_h; gy++)
        for (gx = x0 / GRID; gx <= (x1 - 1) / GRID && gx < m->grid_w; gx++)
            if (gx >= 0 && gy >= 0)
                m->grid[gy * m->grid_w + gx] = 1;
}

static int seen_near(const VMap *m, const char *text, int x, int y, int distance)
{
    int i;
    for (i = 0; i < m->nplaced; i++) {
        int dx = m->placed[i].x - x, dy = m->placed[i].y - y;
        if (dx < distance && dx > -distance && dy < distance && dy > -distance &&
            strcmp(m->placed[i].text, text) == 0)
            return 1;
    }
    return 0;
}

static void remember(VMap *m, const char *text, int x, int y)
{
    if (m->nplaced < VM_MAX_PLACED) {
        m->placed[m->nplaced].text = text;
        m->placed[m->nplaced].x = x;
        m->placed[m->nplaced].y = y;
        m->nplaced++;
    }
}

/* Max filter of radius rad: the halo's shape. */
static void dilate(const unsigned char *src, unsigned char *dst, unsigned char *tmp, int w, int h, int rad)
{
    int x, y, k;
    for (y = 0; y < h; y++) {
        const unsigned char *s = src + y * w;
        unsigned char *t = tmp + y * w;
        for (x = 0; x < w; x++) {
            int best = 0, lo = x - rad < 0 ? 0 : x - rad, hi = x + rad >= w ? w - 1 : x + rad;
            for (k = lo; k <= hi; k++)
                if (s[k] > best)
                    best = s[k];
            t[x] = (unsigned char)best;
        }
    }
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) {
            int best = 0, lo = y - rad < 0 ? 0 : y - rad, hi = y + rad >= h ? h - 1 : y + rad;
            for (k = lo; k <= hi; k++)
                if (tmp[k * w + x] > best)
                    best = tmp[k * w + x];
            dst[y * w + x] = (unsigned char)best;
        }
    }
}

/* ---------------------------------------------------------------- letter pictures
 *
 * Each letter is filled once per size (1/4 pixel steps), angle (VM_ANGLES steps), halo width and
 * half-pixel position, with its halo grown from it, and kept; drawing a label then only mixes
 * kept pictures into the screen. Labels move and turn with every frame while a finger drags or
 * turns the map, so this saves filling every outline again. */

typedef struct {
    unsigned glyph;
    int x, y;                 /* origin on the baseline, 1/256 pixel */
    int angle;                /* VM_ANGLES steps, 0 = upright */
} GlyphAt;

static void ready_angles(VMap *m)
{
    int k;
    if (m->angles_ready)
        return;
    for (k = 0; k < VM_ANGLES; k++) {
        double sn, cs;
        vmap_sincos(k * 360.0 / VM_ANGLES, &sn, &cs);
        m->angles[k][0] = (short)(cs * 16383.5);
        m->angles[k][1] = (short)(sn * 16383.5);
    }
    m->angles_ready = 1;
}

/* The step nearest to direction (ux, uy) x 16384 on the screen (y down). */
static int angle_of(VMap *m, int ux, int uy)
{
    int k, best = 0, best_dot = -0x7FFFFFFF;
    ready_angles(m);
    for (k = 0; k < VM_ANGLES; k++) {
        int d = m->angles[k][0] * ux + m->angles[k][1] * uy;
        if (d > best_dot) {
            best_dot = d;
            best = k;
        }
    }
    return best;
}

static const VmGlyphPic *glyph_pic(VMap *m, unsigned g, int size_q, int angle, int halo, int subx, int suby,
                                   VMapStats *st)
{
    VFont *font = m->font;
    unsigned key = (g & 0xFFFFu) | (unsigned)(size_q & 0xFF) << 16 | (unsigned)(angle & 0x7F) << 24 | (unsigned)subx << 31;
    unsigned key2 = (unsigned)(halo & 0xF) | (unsigned)suby << 4 | 1u << 5;
    VmGlyphPic *p;
    const VfGlyph *o;
    int k, a, b, i, minx = 0x7FFFFFFF, miny = 0x7FFFFFFF, maxx = -0x7FFFFFFF, maxy = -0x7FFFFFFF, x0, y0, w, h;
    unsigned char *bits;
    VrMask mk;

    if (m->glyphs == NULL) {
        m->glyphs = (VmGlyphPic *)malloc(VM_GLYPH_SLOTS * sizeof(VmGlyphPic));
        if (m->glyphs == NULL)
            return NULL;
        for (i = 0; i < VM_GLYPH_SLOTS; i++) { /* a loop: malloc plus memset would become calloc */
            m->glyphs[i].key = m->glyphs[i].key2 = 0;
            m->glyphs[i].stamp = 0;
            m->glyphs[i].bits = NULL;
        }
    }
    /* groups of four: a letter can sit in any of its group's slots, so two letters used in the same
     * frame do not keep throwing each other out */
    {
        VmGlyphPic *set = &m->glyphs[(((key * 2654435761u) ^ (key2 * 40503u)) >> 18 & (VM_GLYPH_SLOTS / 4 - 1)) * 4];
        m->glyph_clock++;
        p = &set[0];
        for (i = 0; i < 4; i++) {
            if (set[i].key == key && set[i].key2 == key2) {
                set[i].stamp = m->glyph_clock;
                return &set[i];
            }
            if (set[i].stamp < p->stamp)
                p = &set[i];
        }
        p->stamp = m->glyph_clock;
    }
    free(p->bits);
    p->bits = NULL;
    p->w = p->h = 0;
    p->key = key;
    p->key2 = key2;
    if (st)
        st->glyphs_new++;
    o = vf_outline(font, g);
    if (o == NULL)
        return p; /* a space: nothing to draw */
    ready_angles(m);
    k = (int)(((long long)size_q << 22) / font->units); /* (size_q * 64) << 16 */
    a = (int)(((long long)k * m->angles[angle][0]) >> 14);
    b = (int)(((long long)k * m->angles[angle][1]) >> 14);
    for (i = 0; i < o->npoints; i++) {
        int fx = o->points[i].x, fy = o->points[i].y;
        int x = (int)(((long long)a * fx + (long long)b * fy) >> 16), y = (int)(((long long)b * fx - (long long)a * fy) >> 16);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    x0 = (minx >> VR_SHIFT) - halo - 2;
    y0 = (miny >> VR_SHIFT) - halo - 2;
    w = (maxx >> VR_SHIFT) + halo + 3 - x0;
    h = (maxy >> VR_SHIFT) + halo + 3 - y0;
    if (w <= 0 || h <= 0 || w > m->raster.max_w || h > m->raster.max_h || !ensure_masks(m, w * h))
        return p;
    bits = (unsigned char *)malloc((size_t)w * h * 2);
    if (bits == NULL)
        return p;
    for (i = 0; i < w * h * 2; i++)
        bits[i] = 0;
    vr_set_clip(&m->raster, w, h);
    vf_add(&m->raster, font, g, a, b, b, -a, (-x0 << VR_SHIFT) + subx * (VR_ONE / 2), (-y0 << VR_SHIFT) + suby * (VR_ONE / 2));
    mk.mask = bits;
    mk.w = w;
    vr_sweep(&m->raster, vr_mask_span, &mk);
    if (halo > 0)
        dilate(bits, bits + w * h, m->mask, w, h, halo);
    p->x = (short)x0;
    p->y = (short)y0;
    p->w = (short)w;
    p->h = (short)h;
    p->bits = bits;
    return p;
}

/* Keeps a placed letter for the frames that follow (see VM_DRAW_LABELS_KEPT). (sx, sy): its
 * anchor on the screen, 1/256 pixel. */
static void keep_glyph(VMap *m, const GlyphAt *g, int sx, int sy, int upright, int size_q, int halo, unsigned rgb,
                       unsigned halo_rgb)
{
    VmKept *k;
    long long ex = sx - ((long long)m->kept_view.cx << VR_SHIFT), ey = sy - ((long long)m->kept_view.cy << VR_SHIFT);
    if (m->nkept == m->kept_cap) {
        int cap = m->kept_cap ? m->kept_cap * 2 : 512;
        VmKept *more = (VmKept *)malloc((size_t)cap * sizeof(VmKept));
        if (more == NULL)
            return;
        if (m->kept)
            memcpy(more, m->kept, (size_t)m->nkept * sizeof(VmKept));
        free(m->kept);
        m->kept = more;
        m->kept_cap = cap;
    }
    k = &m->kept[m->nkept++];
    /* back to north up: the inverse of the screen turn */
    k->dx = (int)((m->kept_c16 * ex + m->kept_s16 * ey) >> 14);
    k->dy = (int)((-m->kept_s16 * ex + m->kept_c16 * ey) >> 14);
    k->ox = g->x - sx;
    k->oy = g->y - sy;
    k->glyph = (unsigned short)g->glyph;
    k->angle = (unsigned char)((g->angle + (int)(m->kept_view.bearing * VM_ANGLES / 360.0 + 0.5)) & (VM_ANGLES - 1));
    k->upright = (unsigned char)upright;
    k->size_q = (unsigned char)size_q;
    k->halo = (unsigned char)halo;
    k->rgb = rgb;
    k->halo_rgb = halo_rgb;
}

/* Mixes letters into the surface: every halo first, then the letters, so no halo covers a letter
 * next to it. size: text height, 1/256 pixel. (ax, ay): the label's anchor (upright labels) on the
 * screen, 1/256 pixel, for keeping it. */
static void draw_glyphs(VMap *m, VSurface *s, const GlyphAt *gl, int n, int size, int halo, unsigned rgb,
                        unsigned halo_rgb, int dim, VMapStats *st, int upright, int ax, int ay)
{
    int size_q = (size + 32) >> 6, pass, i;
    if (size_q > 255)
        size_q = 255;
    if (m->keeping)
        for (i = 0; i < n; i++)
            keep_glyph(m, &gl[i], upright ? ax : gl[i].x, upright ? ay : gl[i].y, upright, size_q, halo,
                       dim ? (rgb >> 1) & 0x7F7F7F : rgb, dim ? (halo_rgb >> 1) & 0x7F7F7F : halo_rgb);
    for (pass = halo > 0 ? 0 : 1; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            int X = gl[i].x, Y = gl[i].y, px = X >> VR_SHIFT, py = Y >> VR_SHIFT, fx = X & (VR_ONE - 1),
                fy = Y & (VR_ONE - 1), sx = 0, sy = 0;
            const VmGlyphPic *p;
            if (fx >= 3 * VR_ONE / 4)
                px++;
            else if (fx >= VR_ONE / 4)
                sx = 1;
            if (fy >= 3 * VR_ONE / 4)
                py++;
            else if (fy >= VR_ONE / 4)
                sy = 1;
            p = glyph_pic(m, gl[i].glyph, size_q, gl[i].angle, halo, sx, sy, st);
            if (p == NULL || p->bits == NULL)
                continue;
            vr_blit_mask(s, px + p->x, py + p->y, pass == 0 ? p->bits + p->w * p->h : p->bits, p->w, p->h,
                         pass == 0 ? halo_rgb : rgb, dim);
        }
    }
}

/* ---------------------------------------------------------------- place markers */

/* The three masks of a marker (ring, badge, pictogram), made on first use: discs filled with
 * smooth edges, the pictogram's pixels exactly as drawn (vicons.c). */
static const unsigned char *marker_masks(VMap *m, int icon)
{
    const int n = VM_MARKER_SIZE, half = VM_MARKER_SIZE / 2;
    const char *bits = vicon_bitmap(icon);
    unsigned char *masks;
    VrMask mk;
    int i, r;
    if (bits == NULL)
        return NULL;
    if (m->markers[icon])
        return m->markers[icon];
    masks = (unsigned char *)malloc((size_t)3 * n * n);
    if (masks == NULL || m->raster.acc == NULL) {
        free(masks);
        return NULL;
    }
    for (i = 0; i < 3 * n * n; i++)
        masks[i] = 0;
    for (r = 0; r < 2; r++) { /* the ring: a disc of 15 px; the badge: 13 px */
        vr_set_clip(&m->raster, n, n);
        vr_disc(&m->raster, half << VR_SHIFT, half << VR_SHIFT, (r == 0 ? half : half - 2) << VR_SHIFT);
        mk.mask = masks + r * n * n;
        mk.w = n;
        vr_sweep(&m->raster, vr_mask_span, &mk);
    }
    for (i = 0; i < VICON_SIZE * VICON_SIZE; i++)
        if (bits[i] == '#')
            masks[2 * n * n + (half - VICON_SIZE / 2 + i / VICON_SIZE) * n + half - VICON_SIZE / 2 + i % VICON_SIZE] = 255;
    m->markers[icon] = masks;
    return masks;
}

/* A marker centred on pixel corner (px, py); colours already dimmed for the night when needed. */
static void draw_marker(VMap *m, VSurface *s, int px, int py, int icon, unsigned badge, unsigned ring, unsigned glyph)
{
    const unsigned char *masks = marker_masks(m, icon);
    const int n = VM_MARKER_SIZE;
    if (masks == NULL)
        return;
    px -= n / 2;
    py -= n / 2;
    vr_blit_mask(s, px, py, masks, n, n, ring, 0);
    vr_blit_mask(s, px, py, masks + n * n, n, n, badge, 0);
    vr_blit_mask(s, px, py, masks + 2 * n * n, n, n, glyph, 0);
}

static void keep_marker(VMap *m, int px, int py, int icon, unsigned badge, unsigned ring, int dim)
{
    GlyphAt g;
    g.glyph = (unsigned)icon;
    g.x = px << VR_SHIFT;
    g.y = py << VR_SHIFT;
    g.angle = 0;
    keep_glyph(m, &g, g.x, g.y, 2, 0, 0, badge, ring);
    m->kept[m->nkept - 1].angle = (unsigned char)dim; /* markers keep the night dimming here */
}

typedef struct {
    const char *start, *end;
    int w;                    /* font units */
} TextLine;

/* Breaks text into at most 4 lines: at line breaks, and between words past max_w (font units). */
static int layout_lines(VFont *f, const char *text, int max_w, int upper, TextLine *lines)
{
    int n = 0, space = vf_advance(f, vf_glyph(f, ' '));
    const char *p = text;
    lines[0].start = p;
    lines[0].end = p;
    lines[0].w = 0;
    while (*p) {
        const char *word = p, *q;
        int w;
        while (*p && *p != ' ' && *p != '\n')
            p++;
        q = word;
        w = text_units(f, q, p, upper);
        if (lines[n].end != lines[n].start && lines[n].w + space + w > max_w && n < 3) {
            n++;
            lines[n].start = lines[n].end = word;
            lines[n].w = 0;
        }
        lines[n].w += (lines[n].end != lines[n].start ? space : 0) + w;
        lines[n].end = p;
        if (*p == '\n' && n < 3) {
            p++;
            n++;
            lines[n].start = lines[n].end = p;
            lines[n].w = 0;
            continue;
        }
        while (*p == ' ')
            p++;
    }
    return n + 1;
}

static int place_point(VMap *m, VSurface *s, const VMapView *view, const VmLayer *L, const Vis *v,
                       const VmFeature *f, int size, int halo, VMapStats *st)
{
    VFont *font = m->font;
    const char *text = v->t->text + f->text;
    const short *p = v->t->points + 2 * f->first_point;
    TextLine lines[4];
    int n, i, x, y, lh, bw = 0, bh, left, top, x0, y0, x1, y1, upper = (L->flags & FLAG_UPPER) != 0;
    int asc = font->ascent, desc = font->descent;

    tpoint(v, p[0], p[1], &x, &y);
    if (x < 0 || y < 0 || x >= (s->w << VR_SHIFT) || y >= (s->h << VR_SHIFT))
        return 0;
    if (seen_near(m, text, x >> VR_SHIFT, y >> VR_SHIFT, 3 * (size >> VR_SHIFT)))
        return 0;
    n = layout_lines(font, text, (int)((long long)L->max_width * font->units / 16), upper, lines);
    for (i = 0; i < n; i++) {
        int lw = (int)((long long)lines[i].w * size / font->units);
        if (lw > bw)
            bw = lw;
    }
    lh = size * 6 / 5;
    bh = n * lh;
    if (L->anchor == VM_ANCHOR_LEFT) {
        left = x + L->offset_x * size / 64;
        top = y - bh / 2 + L->offset_y * size / 64;
    } else if (L->anchor == VM_ANCHOR_TOP) {
        left = x - bw / 2 + L->offset_x * size / 64;
        top = y + L->offset_y * size / 64;
    } else {
        left = x - bw / 2 + L->offset_x * size / 64;
        top = y - bh / 2 + L->offset_y * size / 64;
    }
    x0 = (left >> VR_SHIFT) - halo - 1;
    y0 = (top >> VR_SHIFT) - halo - 1;
    x1 = ((left + bw) >> VR_SHIFT) + halo + 2;
    y1 = ((top + bh) >> VR_SHIFT) + halo + 2;
    if (x0 < 0 || y0 < 0 || x1 > s->w || y1 > s->h || !grid_free(m, x0, y0, x1, y1))
        return 0;
    grid_mark(m, x0, y0, x1, y1);
    remember(m, text, x >> VR_SHIFT, y >> VR_SHIFT);
    {
        static GlyphAt gl[256];
        int count = 0;
        for (i = 0; i < n; i++) {
            int lw = (int)((long long)lines[i].w * size / font->units);
            int pen = L->anchor == VM_ANCHOR_LEFT ? left : left + (bw - lw) / 2;
            int base = top + i * lh + (lh - size) / 2 + (int)((long long)size * asc / (asc - desc));
            const char *c = lines[i].start;
            while (*c && c < lines[i].end && count < 256) {
                unsigned g = vf_glyph(font, text_char(&c, upper));
                gl[count].glyph = g;
                gl[count].x = pen;
                gl[count].y = base;
                gl[count].angle = 0;
                count++;
                pen += (int)((long long)vf_advance(font, g) * size / font->units);
            }
        }
        draw_glyphs(m, s, gl, count, size, halo, L->colour[view->style ? 1 : 0], L->colour2[view->style ? 1 : 0],
                    view->dim, st, 1, x, y);
    }
    return 1;
}

static int place_marker(VMap *m, VSurface *s, const VMapView *view, const VmLayer *L, const Vis *v,
                        const VmFeature *f, int size, int halo, VMapStats *st)
{
    const short *p = v->t->points + 2 * f->first_point;
    const int r = VM_MARKER_SIZE / 2;
    int x, y, px, py, style = view->style ? 1 : 0, named = 0, n = 0, i, bw = 0, lh = 0, bh, left = 0, top = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    unsigned badge = L->colour[style], ring = m->land[style], white = 0xFFFFFF;
    VFont *font = m->font;
    const char *text = f->text != NO_TEXT ? v->t->text + f->text : NULL;
    TextLine lines[4];

    tpoint(v, p[0], p[1], &x, &y);
    px = (x + VR_ONE / 2) >> VR_SHIFT; /* on a whole pixel: the pictogram stays sharp */
    py = (y + VR_ONE / 2) >> VR_SHIFT;
    if (px - r < 0 || py - r < 0 || px + r > s->w || py + r > s->h || !grid_free(m, px - r, py - r, px + r, py + r))
        return 0;
    /* the name to the right, when there is room for it (checked before the marker takes its own) */
    if (size >= 4 * VR_ONE && text && font) {
        n = layout_lines(font, text, (int)((long long)L->max_width * font->units / 16), 0, lines);
        for (i = 0; i < n; i++) {
            int lw = (int)((long long)lines[i].w * size / font->units);
            if (lw > bw)
                bw = lw;
        }
        lh = size * 6 / 5;
        bh = n * lh;
        left = (px + r + 3) << VR_SHIFT;
        top = (py << VR_SHIFT) - bh / 2;
        x0 = (left >> VR_SHIFT) - halo - 1;
        y0 = (top >> VR_SHIFT) - halo - 1;
        x1 = ((left + bw) >> VR_SHIFT) + halo + 2;
        y1 = ((top + bh) >> VR_SHIFT) + halo + 2;
        named = x1 <= s->w && y0 >= 0 && y1 <= s->h && grid_free(m, x0, y0, x1, y1) &&
                !seen_near(m, text, px, py, 6 * (size >> VR_SHIFT));
    }
    grid_mark(m, px - r, py - r, px + r, py + r);
    if (view->dim) {
        badge = (badge >> 1) & 0x7F7F7F;
        ring = (ring >> 1) & 0x7F7F7F;
        white = 0x7F7F7F;
    }
    draw_marker(m, s, px, py, L->icon, badge, ring, white);
    if (m->keeping)
        keep_marker(m, px, py, L->icon, badge, ring, view->dim);
    st->markers++;
    if (named) {
        static GlyphAt gl[128];
        int count = 0, asc = font->ascent, desc = font->descent;
        grid_mark(m, x0, y0, x1, y1);
        remember(m, text, px, py);
        for (i = 0; i < n; i++) {
            int pen = left, base = top + i * lh + (lh - size) / 2 + (int)((long long)size * asc / (asc - desc));
            const char *c = lines[i].start;
            while (*c && c < lines[i].end && count < 128) {
                unsigned g = vf_glyph(font, text_char(&c, 0));
                gl[count].glyph = g;
                gl[count].x = pen;
                gl[count].y = base;
                gl[count].angle = 0;
                count++;
                pen += (int)((long long)vf_advance(font, g) * size / font->units);
            }
        }
        draw_glyphs(m, s, gl, count, size, halo, L->colour2[style], m->land[style], view->dim, st, 1, px << VR_SHIFT,
                    py << VR_SHIFT);
    }
    return 1;
}

typedef struct {
    unsigned glyph;
    int ox, oy, ux, uy;       /* baseline start (1/256 pixel), direction x 16384 */
    int bx0, by0, bx1, by1;   /* box, pixels */
} Placed;

/* Point and direction at distance d (1/256 pixel) along m->pts (n points, lengths in m->cum). */
static void along(const VMap *m, int n, int d, int *x, int *y, int *ux, int *uy)
{
    int j = 0, len, dx, dy;
    while (j < n - 2 && m->cum[j + 1] < d)
        j++;
    len = m->cum[j + 1] - m->cum[j];
    dx = m->pts[2 * j + 2] - m->pts[2 * j];
    dy = m->pts[2 * j + 3] - m->pts[2 * j + 1];
    *x = m->pts[2 * j] + (len ? (int)((long long)dx * (d - m->cum[j]) / len) : 0);
    *y = m->pts[2 * j + 1] + (len ? (int)((long long)dy * (d - m->cum[j]) / len) : 0);
    *ux = m->dir[2 * j]; /* worked out once per segment in place_line */
    *uy = m->dir[2 * j + 1];
}

static int try_along(VMap *m, VSurface *s, const VMapView *view, const VmLayer *L, const char *text, int n,
                     int centre, int tw, int size, int halo, int min_dot, VMapStats *st)
{
    static Placed gl[MAX_LABEL_GLYPHS];
    VFont *font = m->font;
    const char *c = text;
    int count = 0, pen = 0, reversed, x, y, ux, uy, pux = 0, puy = 0, i;
    int half = (size >> VR_SHIFT) * 6 / 10 + halo + 1, upper = (L->flags & FLAG_UPPER) != 0;

    along(m, n, centre, &x, &y, &ux, &uy);
    reversed = ux < 0 || (ux == 0 && uy > 0);
    while (*c) {
        unsigned g = vf_glyph(font, text_char(&c, upper));
        int adv = (int)((long long)vf_advance(font, g) * size / font->units);
        int sc = reversed ? centre + tw / 2 - pen - adv / 2 : centre - tw / 2 + pen + adv / 2;
        Placed *q;
        if (count == MAX_LABEL_GLYPHS)
            return 0;
        along(m, n, sc, &x, &y, &ux, &uy);
        if (reversed) {
            ux = -ux;
            uy = -uy;
        }
        if (count > 0 && ((pux * ux + puy * uy) >> 14) < min_dot)
            return 0;
        q = &gl[count++];
        q->glyph = g;
        /* baseline start: back half the letter along the road, down by about half a capital */
        q->ox = x - (int)(((long long)ux * adv / 2) >> 14) + (int)(((long long)-uy * size * 35 / 100) >> 14);
        q->oy = y - (int)(((long long)uy * adv / 2) >> 14) + (int)(((long long)ux * size * 35 / 100) >> 14);
        q->ux = ux;
        q->uy = uy;
        q->bx0 = (x >> VR_SHIFT) - half;
        q->by0 = (y >> VR_SHIFT) - half;
        q->bx1 = (x >> VR_SHIFT) + half;
        q->by1 = (y >> VR_SHIFT) + half;
        if (q->bx0 < 0 || q->by0 < 0 || q->bx1 > s->w || q->by1 > s->h || !grid_free(m, q->bx0, q->by0, q->bx1, q->by1))
            return 0;
        pux = ux;
        puy = uy;
        pen += adv;
    }
    if (count == 0)
        return 0;
    {
        static GlyphAt at[MAX_LABEL_GLYPHS];
        for (i = 0; i < count; i++) {
            grid_mark(m, gl[i].bx0, gl[i].by0, gl[i].bx1, gl[i].by1);
            at[i].glyph = gl[i].glyph;
            at[i].x = gl[i].ox;
            at[i].y = gl[i].oy;
            at[i].angle = angle_of(m, gl[i].ux, gl[i].uy);
        }
        draw_glyphs(m, s, at, count, size, halo, L->colour[view->style ? 1 : 0], L->colour2[view->style ? 1 : 0],
                    view->dim, st, 0, 0, 0);
    }
    return 1;
}

static int place_line(VMap *m, VSurface *s, const VMapView *view, const VmLayer *L, const Vis *v,
                      const VmFeature *f, int size, int halo, VMapStats *st)
{
    VFont *font = m->font;
    const char *text = v->t->text + f->text;
    const short *p = v->t->points + 2 * f->first_point;
    int part, placed = 0, upper = (L->flags & FLAG_UPPER) != 0;
    int tw = (int)((long long)text_units(font, text, NULL, upper) * size / font->units);
    int spacing = (int)((long long)L->spacing * size / 16);
    double sn, cs;
    int min_dot;

    vmap_sincos(L->max_angle, &sn, &cs);
    min_dot = (int)(cs * 16384);
    if (spacing < tw + size)
        spacing = tw + size;
    for (part = 0; part < f->nparts; part++) {
        int n = (int)v->t->parts[f->first_part + part], q, i, total, c;
        q = screen_points(m, v, p, n, VR_ONE);
        p += 2 * n;
        if (q < 2)
            continue;
        m->cum[0] = 0;
        for (i = 1; i < q; i++) {
            int dx = m->pts[2 * i] - m->pts[2 * i - 2], dy = m->pts[2 * i + 1] - m->pts[2 * i - 1], sh = 0, l;
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, mm = ax > ay ? ax : ay, sdx, sdy;
            while ((mm >> sh) > 16383)
                sh++;
            sdx = dx >> sh;
            sdy = dy >> sh;
            l = (int)vr_isqrt((unsigned)(sdx * sdx + sdy * sdy));
            m->cum[i] = m->cum[i - 1] + (l << sh);
            m->dir[2 * i - 2] = l ? (sdx << 14) / l : 16384;
            m->dir[2 * i - 1] = l ? (sdy << 14) / l : 0;
        }
        total = m->cum[q - 1];
        if (total < tw + size)
            continue;
        c = total < spacing ? total / 2 : spacing / 2;
        for (; c + tw / 2 <= total; c += spacing) {
            int x, y, ux, uy;
            if (c - tw / 2 < 0)
                continue;
            along(m, q, c, &x, &y, &ux, &uy);
            if (x < 0 || y < 0 || x >= (s->w << VR_SHIFT) || y >= (s->h << VR_SHIFT))
                continue;
            st->labels_tried++;
            if (seen_near(m, text, x >> VR_SHIFT, y >> VR_SHIFT, (spacing >> VR_SHIFT) / 2))
                continue;
            if (try_along(m, s, view, L, text, q, c, tw, size, halo, min_dot, st)) {
                remember(m, text, x >> VR_SHIFT, y >> VR_SHIFT);
                placed++;
            }
        }
    }
    return placed;
}

static void draw_labels(VMap *m, VSurface *s, Vis *vis, int nvis, int z256, int z8, const VMapView *view,
                        VMapStats *st)
{
    int li, need = ((s->w + GRID - 1) / GRID) * ((s->h + GRID - 1) / GRID);
    if (m->grid == NULL || m->grid_w * m->grid_h < need) {
        free(m->grid);
        m->grid = (unsigned char *)malloc((size_t)need);
        if (m->grid == NULL)
            return;
    }
    m->grid_w = (s->w + GRID - 1) / GRID;
    m->grid_h = (s->h + GRID - 1) / GRID;
    memset(m->grid, 0, (size_t)need);
    m->nplaced = 0;
    /* the last label layers (cities, towns) are the most important: they go first */
    for (li = m->nlayers - 1; li >= 0; li--) {
        const VmLayer *L = &m->layers[li];
        int size, halo, n = 0, i, j;
        if ((L->kind != VM_LABEL_POINT && L->kind != VM_LABEL_LINE && L->kind != VM_MARKER) || z8 < L->first8 ||
            z8 >= L->end8)
            continue;
        size = table_at(m, L->size, z256) * 4;
        halo = (table_at(m, L->size2, z256) + 32) >> 6;
        if (size < 4 * VR_ONE && L->kind != VM_MARKER)
            continue;
        for (i = 0; i < nvis; i++) {
            Vis *v = &vis[i];
            const VmDraw *dl;
            v->cursor = 0;
            dl = draw_list(v, li);
            if (dl == NULL)
                continue;
            for (j = 0; j < dl->count && n < MAX_CANDIDATES; j++) {
                const VmFeature *f = &v->t->features[v->t->indexes[dl->first + j]];
                if ((f->text == NO_TEXT && L->kind != VM_MARKER) || f->nparts == 0)
                    continue;
                g_cand[n].vis = v;
                g_cand[n].f = f;
                n++;
            }
        }
        /* by rank (lower first), keeping the file's order otherwise */
        for (i = 1; i < n; i++) {
            Cand c = g_cand[i];
            for (j = i; j > 0 && g_cand[j - 1].f->rank > c.f->rank; j--)
                g_cand[j] = g_cand[j - 1];
            g_cand[j] = c;
        }
        for (i = 0; i < n; i++) {
            if (L->kind == VM_LABEL_POINT) {
                st->labels_tried++;
                st->labels_placed += place_point(m, s, view, L, g_cand[i].vis, g_cand[i].f, size, halo, st);
            } else if (L->kind == VM_MARKER) {
                place_marker(m, s, view, L, g_cand[i].vis, g_cand[i].f, size, halo, st);
            } else {
                st->labels_placed += place_line(m, s, view, L, g_cand[i].vis, g_cand[i].f, size, halo, st);
            }
        }
    }
}

/* ---------------------------------------------------------------- kept labels */

static int round_16384(double v)
{
    return v < 0 ? -(int)(-v * 16384 + 0.5) : (int)(v * 16384 + 0.5);
}

static int angle_diff(double a, double b)
{
    double d = a - b;
    while (d > 180)
        d -= 360;
    while (d < -180)
        d += 360;
    return (int)(d < 0 ? -d : d);
}

/* Draws the kept placement for this view when it is close enough to the one it was made for.
 * Returns 0 when it is not (the labels must be placed afresh). */
static int draw_kept(VMap *m, VSurface *s, const VMapView *v, VMapStats *st)
{
    const VMapView *k = &m->kept_view;
    double scale, ddx, ddy, sn, cs;
    int c16, s16, i, pass, steps, mx, my, z256 = (int)(v->zoom * 256 + 0.5);
    if (!m->kept_ok || z256 != (int)(k->zoom * 256 + 0.5) || v->style != k->style || v->dim != k->dim ||
        s->w != m->kept_w || s->h != m->kept_h || angle_diff(v->bearing, k->bearing) > VM_KEEP_DEGREES)
        return 0;
    scale = 256.0 * vmap_exp2(v->zoom) * VR_ONE;
    ddx = (k->mx - v->mx) * scale; /* the placement's middle from the view's middle, 1/256 pixel */
    ddy = (k->my - v->my) * scale;
    if (ddx * ddx + ddy * ddy > (double)VM_KEEP_PIXELS * VM_KEEP_PIXELS * VR_ONE * VR_ONE)
        return 0;
    vmap_sincos(-v->bearing, &sn, &cs);
    c16 = round_16384(cs);
    s16 = round_16384(sn);
    mx = (int)ddx;
    my = (int)ddy;
    steps = (int)(v->bearing * VM_ANGLES / 360.0 + 0.5);
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < m->nkept; i++) {
            const VmKept *g = &m->kept[i];
            long long wx = g->dx + mx, wy = g->dy + my;
            int ax = (v->cx << VR_SHIFT) + (int)((c16 * wx - s16 * wy) >> 14);
            int ay = (v->cy << VR_SHIFT) + (int)((s16 * wx + c16 * wy) >> 14);
            int X = ax + g->ox, Y = ay + g->oy, px, py, fx, fy, sx = 0, sy = 0;
            const VmGlyphPic *p;
            if (g->upright == 2) { /* a place marker, under the letters */
                if (pass == 0 && X > -32 * VR_ONE && Y > -32 * VR_ONE && X < (s->w + 32) * VR_ONE && Y < (s->h + 32) * VR_ONE)
                    draw_marker(m, s, (X + VR_ONE / 2) >> VR_SHIFT, (Y + VR_ONE / 2) >> VR_SHIFT, g->glyph, g->rgb,
                                g->halo_rgb, g->angle ? 0x7F7F7F : 0xFFFFFF);
                continue;
            }
            if (pass == 0 && g->halo == 0)
                continue;
            if (X < -64 * VR_ONE || Y < -64 * VR_ONE || X > (s->w + 64) * VR_ONE || Y > (s->h + 64) * VR_ONE)
                continue;
            px = X >> VR_SHIFT;
            py = Y >> VR_SHIFT;
            fx = X & (VR_ONE - 1);
            fy = Y & (VR_ONE - 1);
            if (fx >= 3 * VR_ONE / 4)
                px++;
            else if (fx >= VR_ONE / 4)
                sx = 1;
            if (fy >= 3 * VR_ONE / 4)
                py++;
            else if (fy >= VR_ONE / 4)
                sy = 1;
            p = glyph_pic(m, g->glyph, g->size_q, g->upright ? 0 : (g->angle - steps) & (VM_ANGLES - 1), g->halo, sx, sy,
                          st);
            if (p == NULL || p->bits == NULL)
                continue;
            vr_blit_mask(s, px + p->x, py + p->y, pass == 0 ? p->bits + p->w * p->h : p->bits, p->w, p->h,
                         pass == 0 ? g->halo_rgb : g->rgb, 0);
        }
    }
    st->labels_kept = 1;
    return 1;
}

/* ---------------------------------------------------------------- text and boxes over the map */

static int raster_for(VMap *m, VSurface *s)
{
    if (m->raster.max_w >= s->w && m->raster.max_h >= s->h)
        return 1;
    if (m->raster.acc)
        vr_free(&m->raster);
    return vr_init(&m->raster, s->w, s->h);
}

int vmap_text_width(VMap *m, const char *text, int size)
{
    if (m->font == NULL)
        return 0;
    return (int)((long long)text_units(m->font, text, NULL, 0) * size / m->font->units);
}

int vmap_text(VMap *m, VSurface *s, const char *text, int x, int y, int size, unsigned rgb, unsigned halo_rgb,
              int halo)
{
    static GlyphAt gl[128];
    VFont *font = m->font;
    const char *c = text;
    int count = 0, pen = x << VR_SHIFT, size256 = size << VR_SHIFT;
    if (font == NULL || !raster_for(m, s))
        return 0;
    while (*c && count < 128) {
        unsigned g = vf_glyph(font, vf_next_char(&c));
        gl[count].glyph = g;
        gl[count].x = pen;
        gl[count].y = y << VR_SHIFT;
        gl[count].angle = 0;
        count++;
        pen += (int)((long long)vf_advance(font, g) * size256 / font->units);
    }
    draw_glyphs(m, s, gl, count, size256, halo, rgb, halo_rgb, 0, NULL, 0, 0, 0);
    return (pen >> VR_SHIFT) - x;
}

void vmap_box(VMap *m, VSurface *s, int x, int y, int w, int h, int radius, unsigned rgb, int opacity)
{
    VRaster *r = &m->raster;
    VrPaint paint;
    int x0 = x << VR_SHIFT, y0 = y << VR_SHIFT, x1 = (x + w) << VR_SHIFT, y1 = (y + h) << VR_SHIFT, rr = radius << VR_SHIFT;
    if (!raster_for(m, s))
        return;
    vr_set_clip(r, s->w, s->h);
    /* the same turning direction as vr_disc, so the pieces add up */
    vr_move_to(r, x0 + rr, y0);
    vr_line_to(r, x0 + rr, y1);
    vr_line_to(r, x1 - rr, y1);
    vr_line_to(r, x1 - rr, y0);
    vr_close(r);
    vr_move_to(r, x0, y0 + rr);
    vr_line_to(r, x0, y1 - rr);
    vr_line_to(r, x1, y1 - rr);
    vr_line_to(r, x1, y0 + rr);
    vr_close(r);
    vr_disc(r, x0 + rr, y0 + rr, rr);
    vr_disc(r, x1 - rr, y0 + rr, rr);
    vr_disc(r, x0 + rr, y1 - rr, rr);
    vr_disc(r, x1 - rr, y1 - rr, rr);
    vr_paint_init(&paint, s, rgb, opacity, 0);
    vr_sweep(r, vr_paint_span, &paint);
}

/* ---------------------------------------------------------------- the view */

int vmap_render(VMap *m, VSurface *s, const VMapView *view, VMapStats *st)
{
    return vmap_draw(m, s, view, VM_DRAW_SHAPES | (view->labels ? VM_DRAW_LABELS : 0), st);
}

int vmap_draw(VMap *m, VSurface *s, const VMapView *view, int what, VMapStats *st)
{
    static Vis vis[VM_MAX_VISIBLE];
    VMapStats local;
    double zoom = view->zoom, lowest, highest, scale, cx, cy, ts, sn, cs, minx = 1e30, miny = 1e30, maxx = -1e30,
           maxy = -1e30;
    int z256, z8, level = -1, i, nvis = 0, li, k, asked = 0;
    long tx0, ty0, tx1, ty1, tx, ty;
    unsigned t0 = now_us(m), t1;
    const VmLevel *lv;

    if (st == NULL)
        st = &local;
    memset(st, 0, sizeof *st);
    if (!raster_for(m, s))
        return 0;
    if (what & VM_DRAW_SHAPES)
        vr_fill_surface(s, m->land[view->style ? 1 : 0], view->dim);
    vmap_zoom_range(m, &lowest, &highest);
    if (zoom < lowest)
        zoom = lowest;
    if (zoom > highest)
        zoom = highest;
    z256 = (int)(zoom * 256 + 0.5);
    z8 = z256 >> 5;
    for (i = 0; i < m->nlevels; i++)
        if (m->levels[i].first8 <= z8 && z8 < m->levels[i].end8)
            level = i;
    if (level < 0)
        return 0;
    lv = &m->levels[level];

    scale = 256.0 * exp2_of(zoom);
    cx = view->mx * scale;
    cy = view->my * scale;
    ts = 256.0 * exp2_of(zoom - lv->zoom);
    vmap_sincos(-view->bearing, &sn, &cs);
    for (k = 0; k < 4; k++) {
        double sx = (k & 1) ? s->w + 64 : -64, sy = (k & 2) ? s->h + 64 : -64;
        double dx = sx - view->cx, dy = sy - view->cy;
        double wx = (cx + cs * dx + sn * dy) / ts, wy = (cy - sn * dx + cs * dy) / ts;
        if (wx < minx) minx = wx;
        if (wx > maxx) maxx = wx;
        if (wy < miny) miny = wy;
        if (wy > maxy) maxy = wy;
    }
    tx0 = (long)minx;
    ty0 = (long)miny;
    tx1 = (long)maxx;
    ty1 = (long)maxy;
    if (tx0 < (long)lv->x0) tx0 = (long)lv->x0;
    if (ty0 < (long)lv->y0) ty0 = (long)lv->y0;
    if (tx1 >= (long)lv->x0 + lv->cols) tx1 = (long)lv->x0 + lv->cols - 1;
    if (ty1 >= (long)lv->y0 + lv->rows) ty1 = (long)lv->y0 + lv->rows - 1;
    for (ty = ty0; ty <= ty1; ty++) {
        /* every tile looked at stays in the cache until the frame is drawn: a load may only drop
         * a tile of an earlier frame, so stop one short of the cache size */
        for (tx = tx0; tx <= tx1 && nvis < VM_MAX_VISIBLE && asked < m->cache_size - 1; tx++) {
            const VmTile *t = get_tile(m, level, (unsigned)tx, (unsigned)ty, st);
            double k16 = ts * 4096.0, dx0 = tx * ts - cx, dy0 = ty * ts - cy;
            Vis *v;
            asked++;
            if (t == NULL)
                continue;
            v = &vis[nvis++];
            v->t = t;
            v->a = (int)(cs * k16);
            v->b = (int)(-sn * k16);
            v->c = (int)(sn * k16);
            v->d = (int)(cs * k16);
            v->tx = (int)((cs * dx0 - sn * dy0 + view->cx) * 256.0);
            v->ty = (int)((sn * dx0 + cs * dy0 + view->cy) * 256.0);
            v->cursor = 0;
        }
    }
    st->tiles_drawn = nvis;
    t1 = now_us(m);
    st->us_tiles = t1 - t0;
    if (nvis == 0) {
        st->us_total = now_us(m) - t0;
        return 0;
    }

    {
        unsigned long cells0 = m->raster.cells_total;
    for (li = 0; li < m->nlayers && (what & VM_DRAW_SHAPES); li++) {
        const VmLayer *L = &m->layers[li];
        unsigned t2;
        int before = st->features;
        if (L->kind > VM_DOT || z8 < L->first8 || z8 >= L->end8)
            continue;
        t2 = now_us(m);
        draw_layer(m, s, vis, nvis, li, z256, view, st);
        t2 = now_us(m) - t2;
        st->us_layer[li] = t2;
        st->layer_features[li] = st->features - before;
        if (L->kind == VM_AREA)
            st->us_areas += t2;
        else
            st->us_lines += t2;
    }
    if ((what & (VM_DRAW_LABELS | VM_DRAW_LABELS_KEPT)) && m->font) {
        unsigned t3 = now_us(m);
        if (!(what & VM_DRAW_LABELS_KEPT) || (what & VM_DRAW_LABELS) || !draw_kept(m, s, view, st)) {
            double sn, cs;
            /* place afresh, and keep the placement for the frames that follow */
            m->kept_view = *view;
            m->kept_w = s->w;
            m->kept_h = s->h;
            vmap_sincos(-view->bearing, &sn, &cs);
            m->kept_c16 = round_16384(cs);
            m->kept_s16 = round_16384(sn);
            m->nkept = 0;
            m->keeping = 1;
            draw_labels(m, s, vis, nvis, z256, z8, view, st);
            m->keeping = 0;
            m->kept_ok = 1;
        }
        st->us_labels = now_us(m) - t3;
    }
    st->cells = m->raster.cells_total - cells0;
    }
    st->us_total = now_us(m) - t0;
    return 1;
}
