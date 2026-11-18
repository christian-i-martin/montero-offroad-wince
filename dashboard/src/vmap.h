/* The vector map: draws a Montero vector map file (*.vmap, format in tools/vmap/mtvmap.py) at any
 * zoom (fractions too) and any rotation, into a 16-bit (RGB565) picture.
 *
 * The file holds the shapes of an area in tiles (OpenStreetMap numbering, Web Mercator), one set
 * of tiles per data zoom, and the drawing rules (layers) compiled from our map styles. For a view
 * the renderer picks the data zoom made for it, reads the tiles the screen needs (kept in a small
 * cache), and draws layer by layer: areas, then lines (outlines, casings, roads), dots, then the
 * labels. Every tile of a layer goes into one filling pass (vraster.c), so tiles join without
 * seams. Labels are placed most important first and skip anything they would overlap; names along
 * roads follow the road letter by letter and are never upside down.
 *
 * The pilot of the "real map app" wish (docs/vehicle-data-research.md, "Vector map pilot").
 * Positions are never logged. Portable C (no Windows headers), tested on the build machine. */
#ifndef VMAP_H
#define VMAP_H

#include "vfont.h"
#include "vicons.h"
#include "vraster.h"

enum { VM_AREA = 1, VM_LINE = 2, VM_DOT = 3, VM_LABEL_POINT = 4, VM_LABEL_LINE = 5, VM_MARKER = 6 };
enum { VM_ANCHOR_CENTRE = 0, VM_ANCHOR_LEFT = 1, VM_ANCHOR_TOP = 2 };

#define VM_MAX_LAYERS 128
#define VM_MAX_LEVELS 8
#define VM_MAX_VISIBLE 48
#define VM_MAX_PLACED 256
#define VM_GLYPH_SLOTS 4096   /* letter pictures kept (filled outline and halo of a letter at one size and angle) */
#define VM_ANGLES 128         /* letters along roads are drawn at 128 angles round the circle (2.8 degrees apart) */

/* What vmap_draw draws. */
#define VM_DRAW_SHAPES 1      /* land, areas, lines, dots */
#define VM_DRAW_LABELS 2      /* names (on top of whatever the surface holds), placed afresh */
#define VM_DRAW_LABELS_KEPT 4 /* names as last placed, moved and turned with the map, while the view
                               * stays within VM_KEEP_DEGREES and VM_KEEP_PIXELS of it (otherwise
                               * placed afresh): for frames while a finger moves the map */
#define VM_MARKER_SIZE 30     /* place markers: a 26 px badge in a 2 px ring, centred on the place */
#define VM_KEEP_DEGREES 20
#define VM_KEEP_PIXELS 120

typedef int (*vmap_read_fn)(void *file, unsigned offset, void *buffer, unsigned size);
typedef unsigned (*vmap_clock_fn)(void); /* microseconds, for the statistics (may be NULL) */

typedef struct {
    int kind, flags, ndash, anchor, first8, end8;   /* zooms x 8 */
    unsigned colour[2], colour2[2];                 /* dark, light (0x00RRGGBB) */
    int opacity;                                    /* 0 to 256 */
    int offset_x, offset_y;                         /* text heights x 64 */
    int dashes[4];                                  /* line widths x 64 */
    int spacing, max_angle, max_width;              /* text heights x 16, degrees, text heights x 16 */
    int icon;                                       /* place markers: pictogram (vicons.h) */
    const unsigned short *size, *size2;             /* pixels x 64, one per zoom table entry */
} VmLayer;

typedef struct {
    int zoom, cols, rows, first8, end8;
    unsigned x0, y0;
    unsigned index;                                 /* offset of the tile index in the file */
} VmLevel;

typedef struct {
    unsigned char type, reserved;
    unsigned short nparts;
    unsigned first_part, first_point, text;
    unsigned short rank, reserved2;
    short box[4];
} VmFeature;                                        /* 28 bytes, as in the file */

typedef struct {
    unsigned short layer, count;
    unsigned first;
} VmDraw;

typedef struct {
    int used, level;
    unsigned x, y, last_use;
    unsigned char *data;
    int nfeatures, ndraws;
    const VmFeature *features;
    const unsigned *parts;
    const VmDraw *draws;
    const short *points;
    const unsigned short *indexes;
    const char *text;
    unsigned text_size;
} VmTile;

typedef struct {
    double mx, my;     /* the point drawn at (cx, cy): Web Mercator, 0 to 1 (x east from 180 W, y south) */
    double zoom;       /* any fraction; the file's data zooms decide the detail */
    double bearing;    /* degrees clockwise from north: this direction points up the screen */
    int style;         /* 0 dark, 1 light */
    int dim;           /* half brightness (the dark map at night) */
    int cx, cy;        /* screen point of (mx, my) */
    int labels;        /* 1: draw labels */
} VMapView;

typedef struct {
    unsigned us_total, us_tiles, us_areas, us_lines, us_labels;
    int tiles_drawn, tiles_loaded, load_errors, features, labels_placed, labels_tried;
    int glyphs_new;                       /* letter pictures made (not found in the cache) */
    int markers;                          /* place markers drawn */
    int labels_kept;                      /* 1: labels drawn from the kept placement */
    unsigned long cells;                  /* pixel cells filled (edges crossing pixels) */
    unsigned us_layer[VM_MAX_LAYERS];     /* time per layer, to find the costly ones */
    int layer_features[VM_MAX_LAYERS];
} VMapStats;

typedef struct {
    const char *text;
    int x, y;
} VmPlaced;

typedef struct {
    unsigned key, key2;       /* letter, size, angle, halo and half-pixel position */
    unsigned stamp;           /* last use, to replace the oldest of its group of four */
    short x, y, w, h;         /* top-left of the picture from the letter's origin pixel; size */
    unsigned char *bits;      /* w x h coverage of the letter, then w x h of its halo */
} VmGlyphPic;

/* A letter of the last label placement, kept to draw the next frames without placing again. */
typedef struct {
    int dx, dy;               /* anchor: world offset from the placement's middle, 1/256 pixel, north up */
    int ox, oy;               /* upright letters: origin from the anchor on the screen, 1/256 pixel */
    unsigned short glyph;
    unsigned char angle;      /* letters along a road: direction against north, VM_ANGLES steps */
    unsigned char upright, size_q, halo;      /* upright: 0 along a road, 1 upright letter, 2 place marker */
    unsigned rgb, halo_rgb;
} VmKept;

typedef struct {
    void *file;
    vmap_read_fn read;
    vmap_clock_fn clock;
    unsigned char *head;
    unsigned head_size;
    int nlayers, nlevels, zoom_start8, zoom_count;
    unsigned land[2];
    double west, south, east, north;
    VmLayer layers[VM_MAX_LAYERS];
    VmLevel levels[VM_MAX_LEVELS];
    VmTile *cache;
    int cache_size;
    unsigned use_clock;
    unsigned char *packed;
    unsigned packed_cap;
    VRaster raster;
    VFont *font;
    int *pts, *cum, *dir;                 /* a line's screen points, lengths along it, directions */
    int pts_cap;
    unsigned char *grid;
    int grid_w, grid_h;
    unsigned char *mask, *halo;
    int mask_cap;
    VmPlaced placed[VM_MAX_PLACED];
    int nplaced;
    VmGlyphPic *glyphs;                   /* VM_GLYPH_SLOTS, made on first use */
    unsigned glyph_clock;
    short angles[VM_ANGLES][2];           /* cos and sin x 16384 */
    int angles_ready;
    VmKept *kept;                         /* the last label placement */
    int nkept, kept_cap, keeping, kept_ok;
    VMapView kept_view;
    int kept_w, kept_h, kept_c16, kept_s16; /* its screen size; its turn (cos, sin x 16384) */
    unsigned char *markers[VICON_COUNT];  /* place marker masks: ring, badge, pictogram (VM_MARKER_SIZE^2 each) */
} VMap;

/* Reads the header, the drawing rules and the tile indexes; keeps cache_tiles tiles in memory.
 * Returns 1 when the file is a usable vector map. The caller keeps the file open. */
int vmap_open(VMap *m, void *file, vmap_read_fn read, int cache_tiles);
void vmap_close(VMap *m);

/* The font for labels (owned by the caller); without one no labels are drawn. */
void vmap_set_font(VMap *m, VFont *f);

/* Web Mercator x and y (0 to 1) of a position. */
void vmap_mercator(double lat, double lon, double *mx, double *my);

/* The middle of the area the file covers. */
void vmap_middle(const VMap *m, double *mx, double *my);

/* The zooms the file can draw (the lowest data zoom up to 19.5). */
void vmap_zoom_range(const VMap *m, double *lowest, double *highest);

/* Draws the view across the whole surface. Returns 1 when a tile of the file was drawn, 0 when
 * the view lies outside the area (the surface is then plain land colour). */
int vmap_render(VMap *m, VSurface *s, const VMapView *v, VMapStats *st);

/* The same in parts: what is VM_DRAW_SHAPES (land, areas, lines, dots; the surface is filled) and/or
 * VM_DRAW_LABELS (names placed for this view and drawn over what the surface holds). Map pictures
 * (vtiles.c) are drawn with shapes only, and the labels go on top of the composed screen. */
int vmap_draw(VMap *m, VSurface *s, const VMapView *v, int what, VMapStats *st);

/* Upright text on one line with an optional halo (pixels), its baseline starting at (x, y); for
 * buttons and notes drawn over the map. Returns the width in pixels. Needs a font. */
int vmap_text(VMap *m, VSurface *s, const char *text, int x, int y, int size, unsigned rgb, unsigned halo_rgb,
              int halo);
int vmap_text_width(VMap *m, const char *text, int size);

/* A box with rounded corners, opacity 0 to 256. */
void vmap_box(VMap *m, VSurface *s, int x, int y, int w, int h, int radius, unsigned rgb, int opacity);

/* sin and cos of an angle in degrees, and 2 to the power x (no maths library needed on the head
 * unit). */
void vmap_sincos(double degrees, double *s, double *c);
double vmap_exp2(double x);

#endif
