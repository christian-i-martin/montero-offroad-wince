#include "vraster.h"

#include <stdlib.h>
#include <string.h>

/* cos and sin x 16384 of 64 angles round the circle */
static const short g_circle[64][2] = {
    {16384, 0}, {16305, 1606}, {16069, 3196}, {15679, 4756}, {15137, 6270}, {14449, 7723}, {13623, 9102},
    {12665, 10394}, {11585, 11585}, {10394, 12665}, {9102, 13623}, {7723, 14449}, {6270, 15137}, {4756, 15679},
    {3196, 16069}, {1606, 16305}, {0, 16384}, {-1606, 16305}, {-3196, 16069}, {-4756, 15679}, {-6270, 15137},
    {-7723, 14449}, {-9102, 13623}, {-10394, 12665}, {-11585, 11585}, {-12665, 10394}, {-13623, 9102},
    {-14449, 7723}, {-15137, 6270}, {-15679, 4756}, {-16069, 3196}, {-16305, 1606}, {-16384, 0},
    {-16305, -1606}, {-16069, -3196}, {-15679, -4756}, {-15137, -6270}, {-14449, -7723}, {-13623, -9102},
    {-12665, -10394}, {-11585, -11585}, {-10394, -12665}, {-9102, -13623}, {-7723, -14449}, {-6270, -15137},
    {-4756, -15679}, {-3196, -16069}, {-1606, -16305}, {0, -16384}, {1606, -16305}, {3196, -16069},
    {4756, -15679}, {6270, -15137}, {7723, -14449}, {9102, -13623}, {10394, -12665}, {11585, -11585},
    {12665, -10394}, {13623, -9102}, {14449, -7723}, {15137, -6270}, {15679, -4756}, {16069, -3196},
    {16305, -1606}};

int vr_init(VRaster *r, int max_w, int max_h)
{
    long cells;
    int i;
    memset(r, 0, sizeof *r);
    r->stride = max_w + 2;
    r->words = (r->stride + 31) / 32;
    cells = (long)r->stride * max_h;
    r->acc = (int *)malloc((size_t)cells * 2 * sizeof(int));
    r->bits = (unsigned *)malloc((size_t)r->words * max_h * sizeof(unsigned));
    if (r->acc == NULL || r->bits == NULL) {
        vr_free(r);
        return 0;
    }
    /* loops, not memset after malloc: GCC would make that calloc, a new import on the head unit */
    for (i = 0; i < cells * 2; i++)
        r->acc[i] = 0;
    for (i = 0; i < r->words * max_h; i++)
        r->bits[i] = 0;
    r->max_w = max_w;
    r->max_h = max_h;
    r->min_row = max_h;
    r->max_row = -1;
    return 1;
}

void vr_free(VRaster *r)
{
    free(r->acc);
    free(r->bits);
    memset(r, 0, sizeof *r);
}

/* Index of the lowest set bit (no count-leading-zeros instruction on ARMv4; a table there). */
#if defined(__GNUC__) && (!defined(__arm__) || (defined(__ARM_ARCH) && __ARM_ARCH >= 6))
#define LOWEST_BIT(low) ((int)__builtin_ctz(low)) /* count-trailing-zeros: two instructions on ARMv7 */
#else
static const unsigned char g_debruijn[32] = {0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
                                             31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};
#define LOWEST_BIT(low) g_debruijn[((low)*0x077CB531u) >> 27]
#endif

void vr_clear(VRaster *r)
{
    int y, k;
    for (y = r->min_row; y <= r->max_row; y++) {
        unsigned *bits = r->bits + y * r->words;
        int *acc = r->acc + 2 * y * r->stride;
        for (k = 0; k < r->words; k++) {
            unsigned b = bits[k];
            bits[k] = 0;
            while (b) {
                unsigned low = b & (0u - b);
                int ci = k * 32 + LOWEST_BIT(low);
                acc[2 * ci] = acc[2 * ci + 1] = 0;
                b ^= low;
            }
        }
    }
    r->min_row = r->max_h;
    r->max_row = -1;
    r->have_cell = 0;
    r->open = 0;
}

void vr_set_clip(VRaster *r, int w, int h)
{
    vr_clear(r);
    r->w = w < r->max_w ? w : r->max_w;
    r->h = h < r->max_h ? h : r->max_h;
}

/* ---------------------------------------------------------------- cells */

static void record(VRaster *r)
{
    int ci, *a;
    if (!r->have_cell || (r->cover | r->area) == 0 || r->cell_y < 0 || r->cell_y >= r->h || r->cell_x >= r->w)
        return;
    ci = r->cell_x + 1; /* the column left of the box is cell 0 */
    a = r->acc + 2 * (r->cell_y * r->stride + ci);
    a[0] += r->cover;
    a[1] += r->area;
    r->bits[r->cell_y * r->words + (ci >> 5)] |= 1u << (ci & 31);
    r->cells_total++;
    if (r->cell_y < r->min_row)
        r->min_row = r->cell_y;
    if (r->cell_y > r->max_row)
        r->max_row = r->cell_y;
}

static void set_cell(VRaster *r, int ex, int ey)
{
    if (ex < -1)
        ex = -1; /* everything left of the picture only counts for the pixels to its right */
    if (ex > r->w)
        ex = r->w;
    if (r->have_cell && ex == r->cell_x && ey == r->cell_y)
        return;
    record(r);
    r->cell_x = ex;
    r->cell_y = ey;
    r->cover = r->area = 0;
    r->have_cell = 1;
}

/* Part of an edge inside one pixel row ey; y1 and y2 are heights inside that row (0 to VR_ONE). */
static void render_hline(VRaster *r, int ey, int x1, int y1, int x2, int y2)
{
    int ex1 = x1 >> VR_SHIFT, ex2 = x2 >> VR_SHIFT;
    int fx1 = x1 & (VR_ONE - 1), fx2 = x2 & (VR_ONE - 1);
    int dx, delta, first, incr, mod, lift, rem, p;

    if (y1 == y2) {
        set_cell(r, ex2, ey);
        return;
    }
    if (ex1 == ex2) {
        delta = y2 - y1;
        r->area += (fx1 + fx2) * delta;
        r->cover += delta;
        return;
    }
    p = (VR_ONE - fx1) * (y2 - y1);
    first = VR_ONE;
    incr = 1;
    dx = x2 - x1;
    if (dx < 0) {
        p = fx1 * (y2 - y1);
        first = 0;
        incr = -1;
        dx = -dx;
    }
    delta = p / dx;
    mod = p % dx;
    if (mod < 0) {
        delta--;
        mod += dx;
    }
    r->area += (fx1 + first) * delta;
    r->cover += delta;
    y1 += delta;
    ex1 += incr;
    set_cell(r, ex1, ey);
    if (ex1 != ex2) {
        p = VR_ONE * (y2 - y1 + delta);
        lift = p / dx;
        rem = p % dx;
        if (rem < 0) {
            lift--;
            rem += dx;
        }
        mod -= dx;
        while (ex1 != ex2) {
            delta = lift;
            mod += rem;
            if (mod >= 0) {
                mod -= dx;
                delta++;
            }
            r->area += VR_ONE * delta;
            r->cover += delta;
            y1 += delta;
            ex1 += incr;
            set_cell(r, ex1, ey);
        }
    }
    delta = y2 - y1;
    r->area += (fx2 + VR_ONE - first) * delta;
    r->cover += delta;
}

/* An edge already inside the clip box (x may be just left of it). */
static void render_line(VRaster *r, int x1, int y1, int x2, int y2)
{
    int ey1 = y1 >> VR_SHIFT, ey2 = y2 >> VR_SHIFT;
    int fy1 = y1 & (VR_ONE - 1), fy2 = y2 & (VR_ONE - 1);
    int dx, dy, x, x_to, p, first, delta, rem, mod, lift, incr;

    set_cell(r, x1 >> VR_SHIFT, ey1);
    if (ey1 == ey2) {
        render_hline(r, ey1, x1, fy1, x2, fy2);
        return;
    }
    dx = x2 - x1;
    dy = y2 - y1;
    incr = 1;
    first = VR_ONE;
    if (dx == 0) {
        int ex = x1 >> VR_SHIFT, two_fx = (x1 - (ex << VR_SHIFT)) << 1;
        if (dy < 0) {
            first = 0;
            incr = -1;
        }
        delta = first - fy1;
        r->area += two_fx * delta;
        r->cover += delta;
        ey1 += incr;
        set_cell(r, ex, ey1);
        delta = first + first - VR_ONE;
        while (ey1 != ey2) {
            r->area += two_fx * delta;
            r->cover += delta;
            ey1 += incr;
            set_cell(r, ex, ey1);
        }
        delta = fy2 - VR_ONE + first;
        r->area += two_fx * delta;
        r->cover += delta;
        return;
    }
    p = (VR_ONE - fy1) * dx;
    if (dy < 0) {
        p = fy1 * dx;
        first = 0;
        incr = -1;
        dy = -dy;
    }
    delta = p / dy;
    mod = p % dy;
    if (mod < 0) {
        delta--;
        mod += dy;
    }
    x = x1 + delta;
    render_hline(r, ey1, x1, fy1, x, first);
    ey1 += incr;
    set_cell(r, x >> VR_SHIFT, ey1);
    if (ey1 != ey2) {
        p = VR_ONE * dx;
        lift = p / dy;
        rem = p % dy;
        if (rem < 0) {
            lift--;
            rem += dy;
        }
        mod -= dy;
        while (ey1 != ey2) {
            delta = lift;
            mod += rem;
            if (mod >= 0) {
                mod -= dy;
                delta++;
            }
            x_to = x + delta;
            render_hline(r, ey1, x, VR_ONE - first, x_to, first);
            x = x_to;
            ey1 += incr;
            set_cell(r, x >> VR_SHIFT, ey1);
        }
    }
    render_hline(r, ey1, x, VR_ONE - first, x2, fy2);
}

static int cut_at(int a1, int b1, int a2, int b2, int at)
{
    /* b where the edge from (a1, b1) to (a2, b2) crosses a = at */
    return b1 + (int)((long long)(b2 - b1) * (at - a1) / (a2 - a1));
}

/* Clips an edge to the box: rows outside add nothing, the part right of the box adds nothing, and
 * the part left of it becomes a vertical edge just left of the box (it still counts for the
 * pixels to its right). */
static void clip_line(VRaster *r, int x1, int y1, int x2, int y2)
{
    int H = r->h << VR_SHIFT, W = r->w << VR_SHIFT;
    const int LEFT = -VR_ONE / 2;

    if (y1 == y2 || (y1 <= 0 && y2 <= 0) || (y1 >= H && y2 >= H))
        return;
    if (y1 < 0) {
        x1 = cut_at(y1, x1, y2, x2, 0);
        y1 = 0;
    } else if (y1 > H) {
        x1 = cut_at(y1, x1, y2, x2, H);
        y1 = H;
    }
    if (y2 < 0) {
        x2 = cut_at(y1, x1, y2, x2, 0);
        y2 = 0;
    } else if (y2 > H) {
        x2 = cut_at(y1, x1, y2, x2, H);
        y2 = H;
    }
    if (y1 == y2 || (x1 >= W && x2 >= W))
        return;
    if (x1 <= 0 && x2 <= 0) {
        render_line(r, LEFT, y1, LEFT, y2);
        return;
    }
    if ((x1 < 0) != (x2 < 0)) {
        int ym = cut_at(x1, y1, x2, y2, 0);
        if (x1 < 0) {
            render_line(r, LEFT, y1, LEFT, ym);
            x1 = 0;
            y1 = ym;
        } else {
            clip_line(r, x1, y1, 0, ym);
            render_line(r, LEFT, ym, LEFT, y2);
            return;
        }
    }
    if (x1 > W || x2 > W) {
        int ym = cut_at(x1, y1, x2, y2, W);
        if (x1 > W) {
            x1 = W;
            y1 = ym;
        } else {
            x2 = W;
            y2 = ym;
        }
    }
    if (y1 != y2)
        render_line(r, x1, y1, x2, y2);
}

void vr_move_to(VRaster *r, int x, int y)
{
    if (r->open)
        vr_close(r);
    r->x = r->start_x = x;
    r->y = r->start_y = y;
    r->open = 1;
}

void vr_line_to(VRaster *r, int x, int y)
{
    clip_line(r, r->x, r->y, x, y);
    r->x = x;
    r->y = y;
}

void vr_close(VRaster *r)
{
    if (!r->open)
        return;
    if (r->x != r->start_x || r->y != r->start_y)
        clip_line(r, r->x, r->y, r->start_x, r->start_y);
    r->open = 0;
}

/* ---------------------------------------------------------------- sweep */

static int coverage_of(int v)
{
    if (v < 0)
        v = -v;
    return v > VR_ONE ? VR_ONE : v;
}

void vr_sweep(VRaster *r, vr_span_fn span, void *ctx)
{
    int y, k;
    if (r->open)
        vr_close(r);
    record(r);
    r->have_cell = 0;
    for (y = r->min_row; y <= r->max_row; y++) {
        unsigned *bits = r->bits + y * r->words;
        int *acc = r->acc + 2 * y * r->stride;
        int sum = 0, run = 0; /* cover so far; first pixel not yet emitted */
        for (k = 0; k < r->words; k++) {
            unsigned b = bits[k];
            if (b == 0)
                continue;
            bits[k] = 0;
            while (b) {
                unsigned low = b & (0u - b);
                int ci = k * 32 + LOWEST_BIT(low), x = ci - 1, area = acc[2 * ci + 1], cov;
                b ^= low;
                if (sum != 0 && run < x) {
                    int s0 = run < 0 ? 0 : run, e = x < r->w ? x : r->w;
                    if (s0 < e)
                        span(ctx, y, s0, e - s0, coverage_of(sum));
                }
                sum += acc[2 * ci];
                acc[2 * ci] = acc[2 * ci + 1] = 0;
                if (x >= 0 && x < r->w) {
                    cov = coverage_of(((sum << (VR_SHIFT + 1)) - area) >> (VR_SHIFT + 1));
                    if (cov > 0)
                        span(ctx, y, x, 1, cov);
                }
                run = x + 1;
            }
        }
        if (sum != 0 && run < r->w) {
            int s0 = run < 0 ? 0 : run;
            span(ctx, y, s0, r->w - s0, coverage_of(sum));
        }
    }
    r->min_row = r->max_h;
    r->max_row = -1;
}

/* ---------------------------------------------------------------- lines and discs */

unsigned vr_isqrt(unsigned v)
{
    unsigned res = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* Length of (dx, dy) and its direction x 16384, without overflow for long edges. */
static int direction(int dx, int dy, int *ux, int *uy)
{
    int s = 0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, m = ax > ay ? ax : ay, len, sdx, sdy;
    while ((m >> s) > 16383)
        s++;
    sdx = dx >> s;
    sdy = dy >> s;
    len = (int)vr_isqrt((unsigned)(sdx * sdx + sdy * sdy));
    if (len == 0) {
        *ux = *uy = 0;
        return 0;
    }
    *ux = (sdx << 14) / len;
    *uy = (sdy << 14) / len;
    return len << s;
}

void vr_disc(VRaster *r, int x, int y, int radius)
{
    int step, k;
    if (radius <= 0)
        return;
    step = radius < 2 * VR_ONE ? 8 : radius < 5 * VR_ONE ? 4 : radius < 12 * VR_ONE ? 2 : 1;
    /* anticlockwise on the screen, like the boxes vr_stroke makes, so overlaps add up */
    vr_move_to(r, x + (int)(((long long)radius * g_circle[0][0]) >> 14), y);
    for (k = step; k < 64; k += step)
        vr_line_to(r, x + (int)(((long long)radius * g_circle[k][0]) >> 14),
                   y - (int)(((long long)radius * g_circle[k][1]) >> 14));
    vr_close(r);
}

/* A small convex polygon (n points), turned to run the same way as vr_disc and the segment boxes,
 * so that overlapping pieces add up instead of cancelling. */
static void convex(VRaster *r, const int *p, int n)
{
    long long area = 0;
    int i;
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (long long)p[2 * i] * p[2 * j + 1] - (long long)p[2 * j] * p[2 * i + 1];
    }
    if (area == 0)
        return;
    if (area < 0) {
        vr_move_to(r, p[0], p[1]);
        for (i = 1; i < n; i++)
            vr_line_to(r, p[2 * i], p[2 * i + 1]);
    } else {
        vr_move_to(r, p[2 * (n - 1)], p[2 * (n - 1) + 1]);
        for (i = n - 2; i >= 0; i--)
            vr_line_to(r, p[2 * i], p[2 * i + 1]);
    }
    vr_close(r);
}

/* Half a disc at (x, y) bulging along direction (ux, uy) x 16384: a round line end. */
static void half_disc(VRaster *r, int x, int y, int ux, int uy, int radius)
{
    int p[2 * 34], n = 0, k, step;
    step = radius < 2 * VR_ONE ? 8 : radius < 5 * VR_ONE ? 4 : radius < 12 * VR_ONE ? 2 : 1;
    for (k = 48; k <= 80; k += step) {
        int c = g_circle[k & 63][0], s = g_circle[k & 63][1];
        int ox = (int)(((long long)ux * c - (long long)uy * s) >> 14), oy = (int)(((long long)uy * c + (long long)ux * s) >> 14);
        p[2 * n] = x + (int)(((long long)ox * radius) >> 14);
        p[2 * n + 1] = y + (int)(((long long)oy * radius) >> 14);
        n++;
    }
    convex(r, p, n);
}

/* Fills the gap on the outer side of a bend at (x, y) from direction 1 to direction 2 (x 16384):
 * a triangle for gentle bends, a rounded wedge for sharp ones, a whole disc for hairpins. */
static void bend(VRaster *r, int x, int y, int ux1, int uy1, int ux2, int uy2, int half)
{
    int cross = (ux1 * uy2 - uy1 * ux2) >> 14, dot = (ux1 * ux2 + uy1 * uy2) >> 14;
    int side = cross > 0 ? -1 : 1, ax, ay, bx, by, mx, my, p[12], n = 0;
    if (dot < -11585) { /* turning back by more than 135 degrees */
        vr_disc(r, x, y, half);
        return;
    }
    ax = side * (int)(((long long)-uy1 * half) >> 14);
    ay = side * (int)(((long long)ux1 * half) >> 14);
    bx = side * (int)(((long long)-uy2 * half) >> 14);
    by = side * (int)(((long long)ux2 * half) >> 14);
    p[n++] = x;
    p[n++] = y;
    p[n++] = x + ax;
    p[n++] = y + ay;
    if (dot < 14189) { /* more than 30 degrees: round it off through the middle direction */
        int ux, uy;
        if (direction(ax + bx, ay + by, &ux, &uy) > 0) {
            mx = (int)(((long long)ux * half) >> 14);
            my = (int)(((long long)uy * half) >> 14);
            if (dot < 0 && direction(ax + mx, ay + my, &ux, &uy) > 0) {
                p[n++] = x + (int)(((long long)ux * half) >> 14);
                p[n++] = y + (int)(((long long)uy * half) >> 14);
            }
            p[n++] = x + mx;
            p[n++] = y + my;
            if (dot < 0 && direction(mx + bx, my + by, &ux, &uy) > 0) {
                p[n++] = x + (int)(((long long)ux * half) >> 14);
                p[n++] = y + (int)(((long long)uy * half) >> 14);
            }
        }
    }
    p[n++] = x + bx;
    p[n++] = y + by;
    convex(r, p, n / 2);
}

static void stroke_solid(VRaster *r, const int *xy, int n, int width, int round, int closed)
{
    int half = width / 2, i, segs = closed ? n : n - 1;
    int pux = 0, puy = 0, have_prev = 0, fux = 0, fuy = 0, join = width >= 2 * VR_ONE;

    if (n <= 0 || half <= 0)
        return;
    if (width < 2 * VR_ONE && n > 1)
        round = 0; /* a round end on a line under 2 pixels wide does not show */
    if (n == 1 || (n == 2 && xy[0] == xy[2] && xy[1] == xy[3])) {
        if (round)
            vr_disc(r, xy[0], xy[1], half);
        return;
    }
    for (i = 0; i < segs; i++) {
        int j = (i + 1) % n;
        int x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * j], y1 = xy[2 * j + 1];
        int ux, uy, nx, ny;
        if (direction(x1 - x0, y1 - y0, &ux, &uy) == 0)
            continue;
        nx = (int)(((long long)-uy * half) >> 14);
        ny = (int)(((long long)ux * half) >> 14);
        vr_move_to(r, x0 + nx, y0 + ny);
        vr_line_to(r, x1 + nx, y1 + ny);
        vr_line_to(r, x1 - nx, y1 - ny);
        vr_line_to(r, x0 - nx, y0 - ny);
        vr_close(r);
        if (!have_prev) {
            fux = ux;
            fuy = uy;
            if (round && !closed)
                half_disc(r, x0, y0, -ux, -uy, half);
        } else if (join) {
            bend(r, x0, y0, pux, puy, ux, uy, half);
        }
        pux = ux;
        puy = uy;
        have_prev = 1;
    }
    if (!have_prev) {
        if (round)
            vr_disc(r, xy[0], xy[1], half);
        return;
    }
    if (closed) {
        if (join)
            bend(r, xy[0], xy[1], pux, puy, fux, fuy, half);
    } else if (round) {
        half_disc(r, xy[2 * (n - 1)], xy[2 * (n - 1) + 1], pux, puy, half);
    }
}

/* Scratch points for dash pieces. */
static int *g_piece;
static int g_piece_cap;

static int piece_add(int *count, int x, int y)
{
    if (*count >= g_piece_cap) {
        int cap = g_piece_cap ? g_piece_cap * 2 : 256;
        int *more = (int *)malloc((size_t)cap * 2 * sizeof(int));
        if (more == NULL)
            return 0;
        if (g_piece)
            memcpy(more, g_piece, (size_t)*count * 2 * sizeof(int));
        free(g_piece);
        g_piece = more;
        g_piece_cap = cap;
    }
    g_piece[2 * *count] = x;
    g_piece[2 * *count + 1] = y;
    (*count)++;
    return 1;
}

void vr_stroke(VRaster *r, const int *xy, int n, int width, int round, int closed, const int *dashes, int ndash)
{
    int total = 0, k, i, segs, on = 1, left, count = 0;
    for (k = 0; k < ndash; k++)
        total += dashes[k];
    if (ndash < 2 || total < VR_ONE / 4) {
        stroke_solid(r, xy, n, width, round, closed);
        return;
    }
    segs = closed ? n : n - 1;
    k = 0;
    left = dashes[0] > 0 ? dashes[0] : 1;
    if (n > 0)
        piece_add(&count, xy[0], xy[1]);
    for (i = 0; i < segs; i++) {
        int j = (i + 1) % n;
        int x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * j], y1 = xy[2 * j + 1];
        int ux, uy, len = direction(x1 - x0, y1 - y0, &ux, &uy), pos = 0;
        if (len == 0)
            continue;
        while (len - pos > left) {
            int qx, qy;
            pos += left;
            qx = x0 + (int)((long long)(x1 - x0) * pos / len);
            qy = y0 + (int)((long long)(y1 - y0) * pos / len);
            if (on) {
                piece_add(&count, qx, qy);
                stroke_solid(r, g_piece, count, width, round, 0);
                count = 0;
            } else {
                count = 0;
                piece_add(&count, qx, qy);
            }
            on = !on;
            k = (k + 1) % ndash;
            left = dashes[k] > 0 ? dashes[k] : 1;
        }
        left -= len - pos;
        if (on)
            piece_add(&count, x1, y1);
    }
    if (on && count > 0)
        stroke_solid(r, g_piece, count, width, round, 0);
}

/* ---------------------------------------------------------------- colour */

unsigned short vr_rgb565(unsigned rgb, int dim)
{
    if (dim)
        rgb = (rgb >> 1) & 0x7F7F7F;
    return (unsigned short)((((rgb >> 19) & 0x1F) << 11) | (((rgb >> 10) & 0x3F) << 5) | ((rgb >> 3) & 0x1F));
}

static unsigned spread(unsigned short c)
{
    return ((unsigned)c | (unsigned)c << 16) & 0x07E0F81Fu;
}

void vr_paint_init(VrPaint *p, VSurface *s, unsigned rgb, int opacity, int dim)
{
    p->s = s;
    p->c565 = vr_rgb565(rgb, dim);
    p->c32 = spread(p->c565);
    p->rgb = (dim ? (rgb >> 1) & 0x7F7F7F : rgb) & 0xFFFFFF;
    p->opacity = opacity < 0 ? 0 : opacity > 256 ? 256 : opacity;
}

static void mix(unsigned short *px, int len, unsigned c32, unsigned short c565, int a)
{
    int i;
    if (a >= 32) {
        for (i = 0; i < len; i++)
            px[i] = c565;
        return;
    }
    for (i = 0; i < len; i++) {
        unsigned d = spread(px[i]);
        d += ((c32 - d) * (unsigned)a) >> 5;
        d &= 0x07E0F81Fu;
        px[i] = (unsigned short)(d | d >> 16);
    }
}

/* 32-bit mixing, a 0 to 256: red and blue together, then green (each field gets the exact
 * rounded-down result; see the 16-bit spread() for why the borrows do not matter). */
static unsigned mix32(unsigned d, unsigned c, unsigned a)
{
    unsigned rb = d & 0xFF00FFu, g = d & 0x00FF00u;
    rb = (rb + ((((c & 0xFF00FFu) - rb) * a) >> 8)) & 0xFF00FFu;
    g = (g + ((((c & 0x00FF00u) - g) * a) >> 8)) & 0x00FF00u;
    return rb | g;
}

static void mix_32(unsigned *px, int len, unsigned rgb, int a)
{
    int i;
    if (a >= 256) {
        for (i = 0; i < len; i++)
            px[i] = rgb;
        return;
    }
    for (i = 0; i < len; i++)
        px[i] = mix32(px[i], rgb, (unsigned)a);
}

void vr_paint_span(void *ctx, int y, int x, int len, int coverage)
{
    VrPaint *p = (VrPaint *)ctx;
    if (p->s->px32) {
        int a = coverage * p->opacity >> VR_SHIFT;
        if (a > 0)
            mix_32(p->s->px32 + (long)y * p->s->stride + x, len, p->rgb, a);
    } else {
        int a = ((coverage * p->opacity >> VR_SHIFT) + 4) >> 3;
        if (a > 0)
            mix(p->s->px + (long)y * p->s->stride + x, len, p->c32, p->c565, a);
    }
}

void vr_mask_span(void *ctx, int y, int x, int len, int coverage)
{
    VrMask *m = (VrMask *)ctx;
    unsigned char *row = m->mask + (long)y * m->w + x;
    int v = coverage >= VR_ONE ? 255 : coverage, i;
    for (i = 0; i < len; i++)
        if (row[i] < v)
            row[i] = (unsigned char)v;
}

void vr_blit_mask(VSurface *s, int x, int y, const unsigned char *mask, int w, int h, unsigned rgb, int dim)
{
    unsigned short c565 = vr_rgb565(rgb, dim);
    unsigned c32 = spread(c565), full = (dim ? (rgb >> 1) & 0x7F7F7F : rgb) & 0xFFFFFF;
    int row, col, c0 = x < 0 ? -x : 0, c1 = x + w > s->w ? s->w - x : w;
    for (row = 0; row < h; row++) {
        int sy = y + row;
        const unsigned char *m = mask + (long)row * w;
        if (sy < 0 || sy >= s->h)
            continue;
        if (s->px32) {
            unsigned *px = s->px32 + (long)sy * s->stride + x;
            for (col = c0; col < c1; col++) {
                unsigned v = m[col];
                if (v == 0)
                    continue;
                px[col] = v == 255 ? full : mix32(px[col], full, v + (v >> 7));
            }
        } else {
            unsigned short *px = s->px + (long)sy * s->stride + x;
            for (col = c0; col < c1; col++) {
                unsigned v = m[col], a, d;
                if (v < 4)
                    continue;
                a = (v + 4) >> 3;
                if (a >= 32) {
                    px[col] = c565;
                    continue;
                }
                d = spread(px[col]);
                d = (d + (((c32 - d) * a) >> 5)) & 0x07E0F81Fu;
                px[col] = (unsigned short)(d | d >> 16);
            }
        }
    }
}

void vr_fill_surface(VSurface *s, unsigned rgb, int dim)
{
    int x, y;
    if (s->px32) {
        unsigned c = (dim ? (rgb >> 1) & 0x7F7F7F : rgb) & 0xFFFFFF;
        for (y = 0; y < s->h; y++) {
            unsigned *px = s->px32 + (long)y * s->stride;
            for (x = 0; x < s->w; x++)
                px[x] = c;
        }
    } else {
        unsigned short c = vr_rgb565(rgb, dim);
        for (y = 0; y < s->h; y++) {
            unsigned short *px = s->px + (long)y * s->stride;
            for (x = 0; x < s->w; x++)
                px[x] = c;
        }
    }
}

void vr_tint(VSurface *s, int bx, int by, int bw, int bh, int r, unsigned rgb, int alpha)
{
    unsigned c565 = vr_rgb565(rgb, 0), cs = spread((unsigned short)c565), a5, keep5, c32 = rgb & 0xFFFFFF;
    int j;
    if (alpha <= 0 || bw <= 0 || bh <= 0)
        return;
    if (alpha > 256)
        alpha = 256;
    a5 = alpha >= 256 ? 32u : (unsigned)alpha >> 3; /* 16-bit: 5-bit alpha, three channels at once */
    keep5 = 32u - a5;
    if (2 * r > bw)
        r = bw / 2;
    if (2 * r > bh)
        r = bh / 2;
    if (r < 0)
        r = 0;
    for (j = 0; j < bh; j++) {
        int y = by + j, inset = 0, x0, x1, x;
        if (y < 0 || y >= s->h)
            continue;
        if (j < r || j >= bh - r) { /* rows in the rounded corners, measured at pixel centres */
            int d = j < r ? 2 * (r - j) - 1 : 2 * (j - (bh - r)) + 1;
            inset = r - ((int)vr_isqrt((unsigned)(4 * r * r - d * d)) + 1) / 2;
        }
        x0 = bx + inset;
        x1 = bx + bw - inset;
        if (x0 < 0)
            x0 = 0;
        if (x1 > s->w)
            x1 = s->w;
        if (s->px32) {
            unsigned *row = s->px32 + (long)y * s->stride;
            for (x = x0; x < x1; x++)
                row[x] = mix32(row[x], c32, (unsigned)alpha);
        } else {
            unsigned short *row = s->px + (long)y * s->stride;
            for (x = x0; x < x1; x++) {
                unsigned d = spread(row[x]);
                d = ((d * keep5 + cs * a5) >> 5) & 0x07E0F81Fu;
                row[x] = (unsigned short)(d | d >> 16);
            }
        }
    }
}
