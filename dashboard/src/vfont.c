#include "vfont.h"

#include <stdlib.h>
#include <string.h>

static unsigned be16(const VFont *f, unsigned o)
{
    return o + 2 <= f->size ? (unsigned)f->data[o] << 8 | f->data[o + 1] : 0;
}

static int sbe16(const VFont *f, unsigned o)
{
    return (short)be16(f, o);
}

static unsigned be32(const VFont *f, unsigned o)
{
    return o + 4 <= f->size ? (unsigned)f->data[o] << 24 | (unsigned)f->data[o + 1] << 16 |
                                  (unsigned)f->data[o + 2] << 8 | f->data[o + 3]
                            : 0;
}

static unsigned table(const VFont *f, const char *tag)
{
    unsigned n = be16(f, 4), i;
    for (i = 0; i < n; i++) {
        unsigned rec = 12 + 16 * i;
        if (rec + 16 <= f->size && memcmp(f->data + rec, tag, 4) == 0)
            return be32(f, rec + 8);
    }
    return 0;
}

int vf_load(VFont *f, const unsigned char *data, unsigned size)
{
    unsigned head, maxp, hhea, cmap, n, i;
    memset(f, 0, sizeof *f);
    f->data = data;
    f->size = size;
    if (size < 12)
        return 0;
    head = table(f, "head");
    maxp = table(f, "maxp");
    hhea = table(f, "hhea");
    cmap = table(f, "cmap");
    f->hmtx = table(f, "hmtx");
    f->loca = table(f, "loca");
    f->glyf = table(f, "glyf");
    if (!head || !maxp || !hhea || !cmap || !f->hmtx || !f->loca || !f->glyf)
        return 0;
    f->units = (int)be16(f, head + 18);
    f->loca_long = be16(f, head + 50) != 0;
    f->glyph_count = be16(f, maxp + 4);
    f->ascent = sbe16(f, hhea + 4);
    f->descent = sbe16(f, hhea + 6);
    f->metrics = be16(f, hhea + 34);
    n = be16(f, cmap + 2);
    for (i = 0; i < n; i++) {
        unsigned rec = cmap + 4 + 8 * i, platform = be16(f, rec), encoding = be16(f, rec + 2);
        unsigned sub = cmap + be32(f, rec + 4);
        if (((platform == 3 && encoding == 1) || platform == 0) && be16(f, sub) == 4) {
            f->cmap4 = sub;
            break;
        }
    }
    for (i = 0; i < VF_MAP; i++)
        f->map[i] = 0xFFFF;
    return f->units > 0 && f->metrics > 0 && f->cmap4 != 0;
}

void vf_free(VFont *f)
{
    int i;
    for (i = 0; i < VF_CACHE; i++) {
        free(f->cache[i].points);
        free(f->cache[i].ends);
    }
    memset(f, 0, sizeof *f);
}

unsigned vf_next_char(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = s[0];
    int extra = 0, i;
    if (c == 0)
        return 0;
    if (c >= 0xF0) {
        c &= 0x07;
        extra = 3;
    } else if (c >= 0xE0) {
        c &= 0x0F;
        extra = 2;
    } else if (c >= 0xC0) {
        c &= 0x1F;
        extra = 1;
    }
    for (i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p = (const char *)(s + i);
            return 0xFFFD;
        }
        c = c << 6 | (s[i] & 0x3F);
    }
    *p = (const char *)(s + 1 + extra);
    return c;
}

static unsigned lookup(const VFont *f, unsigned cp);

unsigned vf_glyph(const VFont *f, unsigned cp)
{
    unsigned short *kept;
    if (cp >= VF_MAP)
        return lookup(f, cp);
    kept = (unsigned short *)&f->map[cp]; /* a cache: the font itself does not change */
    if (*kept == 0xFFFF)
        *kept = (unsigned short)lookup(f, cp);
    return *kept;
}

/* The cmap format 4 table: segments of code points, searched in order. */
static unsigned lookup(const VFont *f, unsigned cp)
{
    unsigned seg2 = be16(f, f->cmap4 + 6), ends = f->cmap4 + 14, starts = ends + seg2 + 2;
    unsigned deltas = starts + seg2, ranges = deltas + seg2, i;
    if (cp > 0xFFFF)
        return 0;
    for (i = 0; i < seg2; i += 2) {
        if (cp <= be16(f, ends + i)) {
            unsigned start = be16(f, starts + i), delta = be16(f, deltas + i), ro = be16(f, ranges + i), g;
            if (cp < start)
                return 0;
            if (ro == 0)
                return (cp + delta) & 0xFFFF;
            g = be16(f, ranges + i + ro + 2 * (cp - start));
            return g ? (g + delta) & 0xFFFF : 0;
        }
    }
    return 0;
}

int vf_advance(const VFont *f, unsigned glyph)
{
    return (int)be16(f, f->hmtx + 4 * (glyph < f->metrics ? glyph : f->metrics - 1));
}

/* ---------------------------------------------------------------- outlines */

typedef struct {
    VfPoint *points;
    unsigned short *ends;
    int np, nc, pcap, ccap, failed;
} Builder;

static int grow(void **buf, int *cap, int need, size_t item)
{
    void *more;
    int n = *cap ? *cap : 64;
    if (need <= *cap)
        return 1;
    while (n < need)
        n *= 2;
    more = malloc((size_t)n * item);
    if (more == NULL)
        return 0;
    if (*buf)
        memcpy(more, *buf, (size_t)*cap * item);
    free(*buf);
    *buf = more;
    *cap = n;
    return 1;
}

static unsigned glyph_offset(const VFont *f, unsigned g)
{
    return f->loca_long ? be32(f, f->loca + 4 * g) : be16(f, f->loca + 2 * g) * 2;
}

/* Appends glyph g's contours, each point mapped by (xx, xy, yx, yy) x 16384 plus (dx, dy). */
static void parse(VFont *f, unsigned g, int depth, Builder *b, int xx, int xy, int yx, int yy, int dx, int dy)
{
    unsigned start, end, p;
    int contours, i;
    if (g >= f->glyph_count || depth > 4)
        return;
    start = f->glyf + glyph_offset(f, g);
    end = f->glyf + glyph_offset(f, g + 1);
    if (end <= start || end > f->size)
        return;
    contours = sbe16(f, start);
    if (contours >= 0) {
        int count, base = b->np, x = 0, y = 0, k;
        unsigned flags_at, xs, ys, ins;
        unsigned char *flags;
        if (contours == 0)
            return;
        count = (int)be16(f, start + 10 + 2 * (contours - 1)) + 1;
        if (count <= 0 || count > 4000)
            return;
        if (!grow((void **)&b->points, &b->pcap, b->np + count, sizeof(VfPoint)) ||
            !grow((void **)&b->ends, &b->ccap, b->nc + contours, sizeof(unsigned short)) ||
            (flags = (unsigned char *)malloc((size_t)count)) == NULL) {
            b->failed = 1;
            return;
        }
        for (i = 0; i < contours; i++)
            b->ends[b->nc + i] = (unsigned short)(base + be16(f, start + 10 + 2 * i));
        ins = be16(f, start + 10 + 2 * contours);
        flags_at = start + 12 + 2 * contours + ins;
        p = flags_at;
        for (i = 0; i < count;) {
            unsigned char fl = p < f->size ? f->data[p++] : 0;
            flags[i++] = fl;
            if (fl & 8) {
                int rep = p < f->size ? f->data[p++] : 0;
                while (rep-- > 0 && i < count)
                    flags[i++] = fl;
            }
        }
        xs = p;
        for (i = 0; i < count; i++) {
            if (flags[i] & 2) {
                int v = xs < f->size ? f->data[xs++] : 0;
                x += (flags[i] & 16) ? v : -v;
            } else if (!(flags[i] & 16)) {
                x += sbe16(f, xs);
                xs += 2;
            }
            b->points[base + i].x = (short)x;
        }
        ys = xs;
        for (i = 0; i < count; i++) {
            if (flags[i] & 4) {
                int v = ys < f->size ? f->data[ys++] : 0;
                y += (flags[i] & 32) ? v : -v;
            } else if (!(flags[i] & 32)) {
                y += sbe16(f, ys);
                ys += 2;
            }
            b->points[base + i].y = (short)y;
            b->points[base + i].on = flags[i] & 1;
        }
        free(flags);
        for (k = 0; k < count; k++) {
            VfPoint *q = &b->points[base + k];
            int px = q->x, py = q->y;
            q->x = (short)(((px * xx + py * yx) >> 14) + dx);
            q->y = (short)(((px * xy + py * yy) >> 14) + dy);
        }
        b->np += count;
        b->nc += contours;
        return;
    }
    /* composite: other glyphs, each moved (and perhaps scaled) */
    p = start + 10;
    for (;;) {
        unsigned fl = be16(f, p), sub = be16(f, p + 2);
        int a1, a2, cxx = 16384, cxy = 0, cyx = 0, cyy = 16384;
        p += 4;
        if (fl & 1) {
            a1 = sbe16(f, p);
            a2 = sbe16(f, p + 2);
            p += 4;
        } else {
            a1 = (signed char)(p < f->size ? f->data[p] : 0);
            a2 = (signed char)(p + 1 < f->size ? f->data[p + 1] : 0);
            p += 2;
        }
        if (!(fl & 2))
            a1 = a2 = 0; /* placed by matching points: not used by our fonts */
        if (fl & 8) {
            cxx = cyy = sbe16(f, p);
            p += 2;
        } else if (fl & 0x40) {
            cxx = sbe16(f, p);
            cyy = sbe16(f, p + 2);
            p += 4;
        } else if (fl & 0x80) {
            cxx = sbe16(f, p);
            cxy = sbe16(f, p + 2);
            cyx = sbe16(f, p + 4);
            cyy = sbe16(f, p + 6);
            p += 8;
        }
        /* this component's map, then the parent's */
        parse(f, sub, depth + 1, b, (cxx * xx + cxy * yx) >> 14, (cxx * xy + cxy * yy) >> 14,
              (cyx * xx + cyy * yx) >> 14, (cyx * xy + cyy * yy) >> 14,
              ((a1 * xx + a2 * yx) >> 14) + dx, ((a1 * xy + a2 * yy) >> 14) + dy);
        if (!(fl & 0x20) || p >= end)
            break;
    }
}

const VfGlyph *vf_outline(VFont *f, unsigned glyph)
{
    VfGlyph *slot = &f->cache[glyph % VF_CACHE];
    Builder b;
    if (slot->used && slot->glyph == glyph)
        return slot->npoints ? slot : NULL;
    free(slot->points);
    free(slot->ends);
    memset(slot, 0, sizeof *slot);
    memset(&b, 0, sizeof b);
    parse(f, glyph, 0, &b, 16384, 0, 0, 16384, 0, 0);
    if (b.failed) {
        free(b.points);
        free(b.ends);
        return NULL;
    }
    slot->glyph = glyph;
    slot->used = 1;
    slot->npoints = b.np;
    slot->ncontours = b.nc;
    slot->points = b.points;
    slot->ends = b.ends;
    return b.np ? slot : NULL;
}

/* ---------------------------------------------------------------- filling */

typedef struct {
    VRaster *r;
    int a, b, c, d, tx, ty;
    int lx, ly;  /* last point emitted, 1/256 pixel */
} Pen;

static void map_point(const Pen *p, int fx, int fy, int *x, int *y)
{
    *x = p->tx + (int)(((long long)p->a * fx + (long long)p->b * fy) >> 16);
    *y = p->ty + (int)(((long long)p->c * fx + (long long)p->d * fy) >> 16);
}

static void quad_to(Pen *p, int cx, int cy, int x1, int y1)
{
    int x0 = p->lx, y0 = p->ly, ddx = x0 - 2 * cx + x1, ddy = y0 - 2 * cy + y1, dev, n, i;
    dev = (ddx < 0 ? -ddx : ddx) + (ddy < 0 ? -ddy : ddy);
    n = dev < 64 ? 1 : dev < 256 ? 2 : dev < 1024 ? 3 : dev < 4096 ? 5 : 8;
    for (i = 1; i <= n; i++) {
        long long u = n - i, t = i, nn = (long long)n * n;
        int x = (int)((x0 * u * u + 2 * cx * u * t + x1 * t * t) / nn);
        int y = (int)((y0 * u * u + 2 * cy * u * t + y1 * t * t) / nn);
        vr_line_to(p->r, x, y);
    }
    p->lx = x1;
    p->ly = y1;
}

void vf_add(VRaster *r, VFont *f, unsigned glyph, int a, int b, int c, int d, int tx, int ty)
{
    const VfGlyph *g = vf_outline(f, glyph);
    Pen pen;
    int start = 0, k;
    if (g == NULL)
        return;
    pen.r = r;
    pen.a = a;
    pen.b = b;
    pen.c = c;
    pen.d = d;
    pen.tx = tx;
    pen.ty = ty;
    for (k = 0; k < g->ncontours; k++) {
        int end = g->ends[k], n = end - start + 1, first = -1, i, sx, sy, cx = 0, cy = 0, have_ctrl = 0;
        if (n < 2 || end >= g->npoints) {
            start = end + 1;
            continue;
        }
        for (i = 0; i < n; i++)
            if (g->points[start + i].on) {
                first = i;
                break;
            }
        if (first < 0) { /* only control points: start halfway between the last and the first */
            map_point(&pen, (g->points[end].x + g->points[start].x) / 2, (g->points[end].y + g->points[start].y) / 2,
                      &sx, &sy);
            first = 0;
        } else {
            map_point(&pen, g->points[start + first].x, g->points[start + first].y, &sx, &sy);
            first++;
        }
        vr_move_to(r, sx, sy);
        pen.lx = sx;
        pen.ly = sy;
        for (i = 0; i < n; i++) {
            const VfPoint *q = &g->points[start + (first + i) % n];
            int x, y;
            map_point(&pen, q->x, q->y, &x, &y);
            if (q->on) {
                if (have_ctrl)
                    quad_to(&pen, cx, cy, x, y);
                else {
                    vr_line_to(r, x, y);
                    pen.lx = x;
                    pen.ly = y;
                }
                have_ctrl = 0;
            } else {
                if (have_ctrl)
                    quad_to(&pen, cx, cy, (cx + x) / 2, (cy + y) / 2);
                cx = x;
                cy = y;
                have_ctrl = 1;
            }
        }
        if (have_ctrl)
            quad_to(&pen, cx, cy, sx, sy);
        vr_close(r);
        start = end + 1;
    }
}
