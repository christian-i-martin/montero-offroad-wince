#include "mapview.h"

#include <string.h>

#include "vmap.h"

/* World pixels per Web Mercator unit at a zoom. */
static double scale_of(double zoom)
{
    return 256.0 * vmap_exp2(zoom);
}

/* Keeps (mx, my) inside the box. */
static void clamp(double *mx, double *my, const MapBox *b)
{
    if (b == NULL)
        return;
    if (*mx < b->left)
        *mx = b->left;
    if (*mx > b->right)
        *mx = b->right;
    if (*my < b->top)
        *my = b->top;
    if (*my > b->bottom)
        *my = b->bottom;
}

static int distance(int d) { return d < 0 ? -d : d; } /* not abs(): no new import on the head unit */

void mapview_open(MapView *v, double zoom, unsigned now_ms)
{
    memset(v, 0, sizeof *v);
    v->zoom = zoom;
    v->follow = 1;
    v->touch_ms = now_ms;
}

void mapview_centre(const MapView *v, const DashModel *m, double *mx, double *my)
{
    if (v->follow && m->have_position) {
        *mx = m->map_mx;
        *my = m->map_my;
    } else {
        *mx = v->mx;
        *my = v->my;
    }
}

double mapview_bearing(const MapView *v, const DashModel *m)
{
    return v->follow ? m->map_bearing : v->bearing;
}

void mapview_press(MapView *v, const DashModel *m, int x, int y, unsigned now_ms)
{
    v->pressed = 1;
    v->dragging = 0;
    v->press_x = x;
    v->press_y = y;
    mapview_centre(v, m, &v->start_mx, &v->start_my);
    v->start_bearing = mapview_bearing(v, m);
    v->touch_ms = now_ms;
}

int mapview_move(MapView *v, int x, int y, const MapBox *box, unsigned now_ms)
{
    int dx = x - v->press_x, dy = y - v->press_y;
    double mx, my, s, c, scale;

    if (!v->pressed)
        return 0;
    v->touch_ms = now_ms;
    if (!v->dragging) {
        if (distance(dx) < MAPVIEW_DRAG_PX && distance(dy) < MAPVIEW_DRAG_PX)
            return 0;
        v->dragging = 1;
        v->follow = 0;
        v->mx = v->start_mx;
        v->my = v->start_my;
        v->bearing = v->start_bearing; /* the map stays turned as it was */
    }
    /* the point under the finger stays under it: the finger's move, turned back to north up */
    scale = scale_of(v->zoom);
    vmap_sincos(v->bearing, &s, &c);
    mx = v->start_mx - (c * dx - s * dy) / scale;
    my = v->start_my - (s * dx + c * dy) / scale;
    clamp(&mx, &my, box);
    if (mx == v->mx && my == v->my)
        return 0;
    v->mx = mx;
    v->my = my;
    return 1;
}

void mapview_release(MapView *v, unsigned now_ms)
{
    v->pressed = 0;
    v->dragging = 0;
    v->touch_ms = now_ms;
}

void mapview_set_zoom(MapView *v, double zoom, unsigned now_ms)
{
    v->touch_ms = now_ms;
    v->pressed = v->dragging = 0;
    v->zoom = zoom; /* the middle is a position, not a pixel: it stays as it is */
}

void mapview_follow(MapView *v)
{
    v->follow = 1;
    v->pressed = v->dragging = 0;
}

int mapview_idle(MapView *v, unsigned now_ms)
{
    if (v->follow || v->pressed || now_ms - v->touch_ms < MAPVIEW_IDLE_MS)
        return 0;
    v->follow = 1;
    return 1;
}

void mapview_to_screen(double cmx, double cmy, double zoom, double bearing, int cx, int cy, double mx, double my,
                       double *sx, double *sy)
{
    double scale = scale_of(zoom), dx = (mx - cmx) * scale, dy = (my - cmy) * scale, s, c;
    vmap_sincos(bearing, &s, &c);
    /* the inverse of mapview_move's turn */
    *sx = cx + c * dx + s * dy;
    *sy = cy - s * dx + c * dy;
}
