#include "island.h"

/* Timings (ms from the start of a move). Opening: the button grows into the panel with a small
 * bounce, then the text fades in. Closing: the text fades out first, then the panel shrinks back
 * into the button. A row added or removed: the panel's height follows, new text fades in. */
#define OPEN_SHAPE 420
#define OPEN_TEXT_FROM 300
#define OPEN_TEXT_TO 480
#define CLOSE_TEXT_TO 110
#define CLOSE_SHAPE_FROM 90
#define CLOSE_SHAPE_TO 450
#define RESIZE_SHAPE 320
#define RESIZE_TEXT_FROM 200
#define RESIZE_TEXT_TO 360
#define APPEAR_SHAPE 260
#define LEAVE_SHAPE 220
#define BOUNCE 1.3              /* the overshoot of the growing moves: about 6 % past the size */

static const int g_length[] = {0, OPEN_TEXT_TO, CLOSE_SHAPE_TO, RESIZE_TEXT_TO, APPEAR_SHAPE, LEAVE_SHAPE};

/* ---------------------------------------------------------------- curves */

static double clamp01(double t) { return t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t; }
static double out_cubic(double t) { double u = 1.0 - t; return 1.0 - u * u * u; }
static double in_cubic(double t) { return t * t * t; }
static double in_out_cubic(double t)
{
    double u = 2.0 - 2.0 * t;
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - u * u * u / 2.0;
}
static double out_back(double t)
{
    double u = t - 1.0;
    return 1.0 + (BOUNCE + 1.0) * u * u * u + BOUNCE * u * u;
}

/* 0 before a, 256 after b, a straight line between. */
static int ramp(unsigned e, int a, int b)
{
    if ((int)e <= a)
        return 0;
    if ((int)e >= b)
        return 256;
    return (int)((e - (unsigned)a) * 256u / (unsigned)(b - a));
}

/* ---------------------------------------------------------------- shapes */

static void dot_box(IslandBox *b)
{
    b->x = ISLAND_DOT_X;
    b->y = ISLAND_DOT_Y;
    b->w = b->h = ISLAND_DOT_D;
    b->r = ISLAND_DOT_D / 2;
}

static void point_box(IslandBox *b) /* nothing: the button's middle */
{
    b->x = ISLAND_DOT_X + ISLAND_DOT_D / 2;
    b->y = ISLAND_DOT_Y + ISLAND_DOT_D / 2;
    b->w = b->h = b->r = 0;
}

static void panel_box(int rows, IslandBox *b)
{
    b->x = ISLAND_PANEL_X;
    b->y = ISLAND_PANEL_Y;
    b->w = ISLAND_PANEL_W;
    b->h = 2 * ISLAND_PAD + (rows < 1 ? 1 : rows) * ISLAND_ROW_H;
    b->r = ISLAND_RADIUS;
}

static void dot_icon(double *x, double *y)
{
    *x = ISLAND_DOT_X + (ISLAND_DOT_D - ISLAND_ICON) / 2;
    *y = ISLAND_DOT_Y + (ISLAND_DOT_D - ISLAND_ICON) / 2;
}

static void panel_icon(double *x, double *y)
{
    *x = ISLAND_PANEL_X + 16;
    *y = ISLAND_PANEL_Y + ISLAND_PAD + 12;
}

/* Where a move ends, or where it rests. */
static void target(const Island *s, int move, IslandBox *b, double *ix, double *iy)
{
    int where = move == ISLAND_OPENING || move == ISLAND_RESIZING ? ISLAND_OPEN
              : move == ISLAND_CLOSING || move == ISLAND_APPEARING ? ISLAND_DOT
              : move == ISLAND_LEAVING ? ISLAND_HIDDEN : s->rest;
    if (where == ISLAND_OPEN) {
        panel_box(s->rows, b);
        panel_icon(ix, iy);
    } else {
        if (where == ISLAND_DOT)
            dot_box(b);
        else
            point_box(b);
        dot_icon(ix, iy);
    }
}

/* The shape and icon at now: the middle and the top move along one curve, the size along another
 * (the bounce), so a bounce grows the panel evenly to both sides and downwards only. */
static void current(const Island *s, unsigned now, IslandBox *b, double *ix, double *iy)
{
    IslandBox to;
    double tx, ty, p = 1.0, q = 1.0, fcx, tcx;
    unsigned e = now - s->t0;

    target(s, s->move, &to, &tx, &ty);
    switch (s->move) {
    case ISLAND_OPENING:
        p = out_cubic(clamp01(e / (double)OPEN_SHAPE));
        q = out_back(clamp01(e / (double)OPEN_SHAPE));
        break;
    case ISLAND_CLOSING:
        p = q = in_out_cubic(clamp01(((double)e - CLOSE_SHAPE_FROM) / (CLOSE_SHAPE_TO - CLOSE_SHAPE_FROM)));
        break;
    case ISLAND_RESIZING:
        p = out_cubic(clamp01(e / (double)RESIZE_SHAPE));
        q = to.h > s->from.h ? out_back(clamp01(e / (double)RESIZE_SHAPE)) : p;
        break;
    case ISLAND_APPEARING:
        p = out_cubic(clamp01(e / (double)APPEAR_SHAPE));
        q = out_back(clamp01(e / (double)APPEAR_SHAPE));
        break;
    case ISLAND_LEAVING:
        p = q = in_cubic(clamp01(e / (double)LEAVE_SHAPE));
        break;
    default:
        *b = to;
        *ix = tx;
        *iy = ty;
        return;
    }
    fcx = s->from.x + s->from.w / 2.0;
    tcx = to.x + to.w / 2.0;
    b->w = s->from.w + (to.w - s->from.w) * q;
    b->h = s->from.h + (to.h - s->from.h) * q;
    b->x = fcx + (tcx - fcx) * p - b->w / 2.0;
    b->y = s->from.y + (to.y - s->from.y) * p;
    b->r = s->from.r + (to.r - s->from.r) * p;
    if (b->w < 0) b->w = 0;
    if (b->h < 0) b->h = 0;
    *ix = s->from_icon_x + (tx - s->from_icon_x) * p;
    *iy = s->from_icon_y + (ty - s->from_icon_y) * p;
}

static int iround(double v) { return (int)(v < 0 ? v - 0.5 : v + 0.5); }

/* ---------------------------------------------------------------- moves */

/* Starts a move from wherever the shape is now, keeping each row's text as it is shown now. */
static void begin(Island *s, int move, unsigned now)
{
    IslandFrame f;
    int i;
    island_frame(s, now, &f);
    for (i = 0; i < HOLIDAY_ROWS; i++)
        s->from_alpha[i] = i < f.rows ? f.alpha[i] : 0;
    current(s, now, &s->from, &s->from_icon_x, &s->from_icon_y);
    s->move = move;
    s->t0 = now;
}

void island_init(Island *s)
{
    int i;
    s->rest = ISLAND_HIDDEN;
    s->move = ISLAND_STILL;
    s->t0 = 0;
    point_box(&s->from);
    dot_icon(&s->from_icon_x, &s->from_icon_y);
    s->rows = s->kept_rows = 0;
    for (i = 0; i < HOLIDAY_ROWS; i++)
        s->from_alpha[i] = 0;
    s->hold_until = 0;
}

void island_set_rows(Island *s, int rows, unsigned now)
{
    if (rows < 0)
        rows = 0;
    if (rows > HOLIDAY_ROWS)
        rows = HOLIDAY_ROWS;
    if (rows == s->rows)
        return;
    if (rows == 0) {
        s->rows = 0;
        if (s->rest != ISLAND_HIDDEN) {
            begin(s, ISLAND_LEAVING, now);
            s->rest = ISLAND_HIDDEN;
        }
        return;
    }
    if (s->rest == ISLAND_OPEN) {
        s->kept_rows = rows < s->rows ? rows : s->rows;
        begin(s, ISLAND_RESIZING, now);
        s->rows = rows;
        s->hold_until = now + RESIZE_TEXT_TO + ISLAND_HOLD_MS;
        return;
    }
    s->rows = rows;
    if (s->rest == ISLAND_HIDDEN) {
        begin(s, ISLAND_APPEARING, now);
        s->rest = ISLAND_DOT;
    }
}

void island_open(Island *s, unsigned now)
{
    if (s->rows == 0)
        return;
    s->hold_until = now + OPEN_TEXT_TO + ISLAND_HOLD_MS;
    if (s->rest == ISLAND_OPEN)
        return; /* already open or opening: it only stays longer */
    begin(s, ISLAND_OPENING, now);
    s->rest = ISLAND_OPEN;
}

void island_close(Island *s, unsigned now)
{
    if (s->rest != ISLAND_OPEN)
        return;
    begin(s, ISLAND_CLOSING, now);
    s->rest = ISLAND_DOT;
}

void island_snap(Island *s)
{
    if (s->rest == ISLAND_OPEN)
        s->rest = ISLAND_DOT;
    s->move = ISLAND_STILL;
}

int island_step(Island *s, unsigned now)
{
    if (s->move != ISLAND_STILL && now - s->t0 >= (unsigned)g_length[s->move])
        s->move = ISLAND_STILL;
    if (s->rest == ISLAND_OPEN && s->move == ISLAND_STILL && (int)(now - s->hold_until) >= 0)
        island_close(s, now);
    return s->move != ISLAND_STILL;
}

void island_frame(const Island *s, unsigned now, IslandFrame *f)
{
    IslandBox b;
    double ix, iy;
    unsigned e = now - s->t0;
    int i, min_side;

    current(s, now, &b, &ix, &iy);
    f->x = iround(b.x);
    f->y = iround(b.y);
    f->w = iround(b.w);
    f->h = iround(b.h);
    min_side = f->w < f->h ? f->w : f->h;
    f->radius = iround(b.r);
    if (f->radius > min_side / 2)
        f->radius = min_side / 2;
    f->icon = f->w >= ISLAND_ICON + 8 && f->h >= ISLAND_ICON + 8;
    f->icon_x = iround(ix);
    f->icon_y = iround(iy);
    f->rows = s->rows;
    f->panel = s->rows > 0 && f->w >= ISLAND_PANEL_W - 2;
    for (i = 0; i < HOLIDAY_ROWS; i++) {
        int a = 0, from = s->from_alpha[i];
        switch (s->move) {
        case ISLAND_OPENING: a = from + (256 - from) * ramp(e, OPEN_TEXT_FROM, OPEN_TEXT_TO) / 256; break;
        case ISLAND_RESIZING:
            from = i < s->kept_rows ? from : 0;
            a = from + (256 - from) * ramp(e, RESIZE_TEXT_FROM, RESIZE_TEXT_TO) / 256;
            break;
        case ISLAND_CLOSING: a = from * (256 - ramp(e, 0, CLOSE_TEXT_TO)) / 256; break;
        case ISLAND_STILL: a = s->rest == ISLAND_OPEN ? 256 : 0; break;
        }
        /* a row is shown only once the panel is wide and tall enough to hold it */
        if (i >= s->rows || !f->panel || f->y + ISLAND_PAD + (i + 1) * ISLAND_ROW_H > f->y + f->h)
            a = 0;
        f->alpha[i] = a;
    }
}

int island_hit(const Island *s, unsigned now, int x, int y)
{
    IslandFrame f;
    int slack;
    if (s->rest == ISLAND_HIDDEN && s->move == ISLAND_STILL)
        return ISLAND_HIT_NONE;
    island_frame(s, now, &f);
    if (f.w < 1 || f.h < 1)
        return ISLAND_HIT_NONE;
    slack = f.w < ISLAND_PANEL_W / 2 ? 10 : 4; /* the button answers a finger a little outside it */
    if (x < f.x - slack || x >= f.x + f.w + slack || y < f.y - slack || y >= f.y + f.h + slack)
        return ISLAND_HIT_NONE;
    return s->rest == ISLAND_OPEN ? ISLAND_HIT_PANEL : ISLAND_HIT_DOT;
}

void island_bounds(const IslandFrame *f, int *x, int *y, int *w, int *h)
{
    *x = f->x - 1;
    *y = f->y - 1;
    *w = f->w + 2;
    *h = f->h + 2;
}
