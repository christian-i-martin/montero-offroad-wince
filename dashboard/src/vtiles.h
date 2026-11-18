/* Map pictures for smooth dragging and turning of the vector map.
 *
 * Drawing the whole vector map takes 0.1 to 0.6 s on the head unit, too slow for a finger. So the
 * map is drawn once, north up and without labels, into 256 x 256 pictures ("map pictures", 128 KB
 * each) that are kept in memory (the unit has hundreds of MB free). A frame then only copies the
 * pictures onto the screen, moved, turned and scaled as the view needs (vt_compose), and the
 * labels are drawn on top, upright, from kept letter pictures (vmap.c). Missing pictures are drawn
 * one at a time (vt_fill): those on the screen first, then a ring around it, then the next zoom
 * levels, while nothing else happens. Until a picture is there, the same place is taken from the
 * pictures of the zoom shown before (scaled), or left in the land colour.
 *
 * Picture (tx, ty) at zoom z covers world pixels tx * 256 to tx * 256 + 255 (and the same for y)
 * at that zoom, any fraction of a zoom included; pictures are kept per zoom (1/256 steps) and per
 * look (dark or light, dimmed or not). Every detail of the map is in them: they are drawn by the
 * same vmap_draw as the full map, which they match exactly when not turned.
 *
 * Portable C (no Windows headers), tested on the build machine. */
#ifndef VTILES_H
#define VTILES_H

#include "vmap.h"

#define VT_SIZE 256
#define VT_STRIDE 257           /* each picture holds one more column and row, copied from its right and
                                 * lower neighbours, so mixing four pixels never needs a second picture */
#define VT_GRID 12              /* the most pictures across or down that one frame looks at */
#define VT_CHUNK_W 64           /* turned frames: chunks of 64 x 16 screen pixels (see vt_compose) */
#define VT_CHUNK_H 16

typedef struct {
    int used, z256, look;       /* zoom x 256; style | dim << 1 */
    long tx, ty;
    unsigned last_use;
    unsigned short *px;         /* 257 x 257 RGB565: the picture and its border */
} VtPic;

#define VT_BUCKETS 1024

typedef struct {
    VMap *map;
    VtPic *pics;
    int *next;                  /* chains of the lookup table */
    int bucket[VT_BUCKETS];     /* first picture of each chain, -1 for none */
    int count;                  /* pictures kept at most */
    int allocated;              /* pictures with memory so far */
    unsigned clock;             /* counts frames, for keeping what a frame uses */
    int cur_z256, prev_z256;    /* the zoom shown, and the one before (its pictures stand in) */
    int drawn;                  /* pictures drawn since vt_init */
    unsigned us_drawing;        /* time spent drawing them (map->clock), microseconds */
    unsigned us_last;
    int missing;                /* pictures on the screen that the last vt_compose did not have */
    int chunk_w, chunk_h;       /* turned or scaled frames are composed in chunks of this size (0: rows) */
} VTiles;

/* count: the most pictures kept (129 KB each, allocated as they are needed). */
int vt_init(VTiles *t, VMap *m, int count);
void vt_free(VTiles *t);

/* Drops every picture (after a change of the map file, or to measure drawing them). */
void vt_clear(VTiles *t);

/* Draws the view onto the surface from the pictures, without labels: moved, turned and scaled as
 * needed. smooth: mix the four nearest picture pixels (bilinear), otherwise take the nearest one.
 * Returns the number of pictures on the screen that are not drawn yet (also in t->missing). */
int vt_compose(VTiles *t, VSurface *s, const VMapView *v, int smooth);

/* Draws one missing picture for the view at zoom v->zoom + zoom_step on a w x h screen: on the
 * screen first (nearest the middle first), then up to `ring` pictures beyond the screen's edges.
 * Returns 1 when it drew one, 0 when none is missing there. Pictures outside the map file's area
 * are never missing: they are land. */
int vt_fill(VTiles *t, const VMapView *v, int w, int h, int ring, double zoom_step);

#endif
