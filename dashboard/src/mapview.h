/* The map page's view. The page shows the map alone across the whole screen; it opens when the
 * car on the dashboard's map is tapped and closes with Back.
 *
 * The view follows the car (the car in the middle of the screen, the map turned as on the
 * dashboard: the direction of travel up) until a finger drags the map. It then stays where it was
 * left, turned as it was, while the car moves on it, until "Centre on car" is pressed or nothing
 * has touched the page for MAPVIEW_IDLE_MS. Its zoom is its own, starting at the dashboard's, so the
 * dashboard is unchanged after Back. A dragged view stays inside the box the map file covers, so it
 * never drifts into empty land.
 *
 * Positions are Web Mercator x and y (0 to 1, x east from 180 W, y south from 85 N, as vmap.h),
 * zooms any fraction. The view's middle is a position, so, like the GPS position, it is never
 * logged. Portable C (no Windows headers), tested on the build machine. */
#ifndef MAPVIEW_H
#define MAPVIEW_H

#include "model.h"

#define MAPVIEW_IDLE_MS 30000u   /* follows the car again after 30 s without a touch */
#define MAPVIEW_DRAG_PX 10       /* a press becomes a drag once the finger has moved this far:
                                  * touch screens report a few pixels of jitter, and a tap must not
                                  * move the map */

/* The box the map file covers, Web Mercator. */
typedef struct { double left, top, right, bottom; } MapBox;

typedef struct {
    double zoom;
    int follow;                  /* the middle of the screen is the car, turned as the dashboard */
    double mx, my;               /* the middle of the screen while not following */
    double bearing;              /* the direction up the screen while not following, degrees */
    int pressed;                 /* a finger is down on the map (not on a button) */
    int dragging;                /* ... and has moved at least MAPVIEW_DRAG_PX */
    int press_x, press_y;        /* where it went down */
    double start_mx, start_my;   /* the middle of the screen at that moment */
    double start_bearing;        /* the turn at that moment */
    unsigned touch_ms;           /* the last touch, for MAPVIEW_IDLE_MS */
} MapView;

/* Opens the view at a zoom, following the car. */
void mapview_open(MapView *v, double zoom, unsigned now_ms);

/* The position in the middle of the screen (the car's, m->map_mx and m->map_my, while following),
 * and the direction up the screen (m->map_bearing while following). */
void mapview_centre(const MapView *v, const DashModel *m, double *mx, double *my);
double mapview_bearing(const MapView *v, const DashModel *m);

/* A finger went down on the map at screen point (x, y). */
void mapview_press(MapView *v, const DashModel *m, int x, int y, unsigned now_ms);

/* The finger moved to (x, y). box: the map file's box, or NULL for no limit. Returns 1 when the
 * view moved (the screen needs drawing again). */
int mapview_move(MapView *v, int x, int y, const MapBox *box, unsigned now_ms);

/* The finger went up (or the window was hidden while it was down). */
void mapview_release(MapView *v, unsigned now_ms);

/* Another zoom, keeping the middle of the screen in place. */
void mapview_set_zoom(MapView *v, double zoom, unsigned now_ms);

/* Follows the car again ("Centre on car"). */
void mapview_follow(MapView *v);

/* Call about once a second: follows the car again after MAPVIEW_IDLE_MS without a touch.
 * Returns 1 when it did. */
int mapview_idle(MapView *v, unsigned now_ms);

/* Where position (mx, my) lands on a screen whose point (cx, cy) shows (cmx, cmy) at this zoom,
 * with `bearing` up the screen. */
void mapview_to_screen(double cmx, double cmy, double zoom, double bearing, int cx, int cy, double mx, double my,
                       double *sx, double *sy);

#endif
