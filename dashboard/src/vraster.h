/* Smooth-edged (anti-aliased) shape filling for the vector map, in whole-number maths only: the
 * head unit's processor has no floating-point hardware.
 *
 * Shapes are given as closed outlines in 1/256 pixel units (VR_ONE per pixel). Every edge adds
 * to "cells" (one per pixel it crosses) the share of the pixel it covers; a sweep along each row
 * then turns the cells into runs of pixels with a coverage of 0 (empty) to 256 (full) and hands
 * them to a span function, which mixes a colour into the picture or writes a text mask. This is
 * the method of FreeType's and AGG's renderers. Overlapping outlines add up (the "non-zero"
 * rule): a pixel covered twice is simply covered, and an outline running the other way (a hole
 * in an area) takes coverage away.
 *
 * Lines with a width become outlines too (vr_stroke): one box per segment, a wedge on the outer
 * side of each bend and, for round ends, a half disc at both ends; all of them overlap into one
 * shape. Dashes cut the line into pieces first.
 *
 * Portable C (no Windows headers), tested on the build machine. */
#ifndef VRASTER_H
#define VRASTER_H

#define VR_SHIFT 8
#define VR_ONE (1 << VR_SHIFT)

typedef struct {
    int w, h;                 /* clip box: 0 <= x < w, 0 <= y < h pixels */
    int max_w, max_h;         /* the largest clip box (memory allocated) */
    int stride, words;        /* cells per row (max_w + 2: one left of the box, one spare), bitmap words per row */
    int *acc;                 /* cover and area of every cell, in pairs */
    unsigned *bits;           /* which cells of each row have something, so a sweep visits them in order */
    int min_row, max_row;     /* rows that have cells */
    int cell_x, cell_y, cover, area, have_cell;
    int x, y, start_x, start_y, open;
    unsigned long cells_total; /* cells filled since vr_init, for the statistics */
} VRaster;

/* Called for each run of pixels: row y, columns x to x + len - 1, coverage 1 to 256. */
typedef void (*vr_span_fn)(void *ctx, int y, int x, int len, int coverage);

/* max_w x max_h: the largest clip box. Takes about 8 bytes per pixel (3 MB for 800 x 480): a
 * full-screen grid of cells instead of a sorted list makes the sweep visit cells in order. */
int vr_init(VRaster *r, int max_w, int max_h);
void vr_free(VRaster *r);

/* The clip box for the next shapes (h at most max_h). Clears the raster. */
void vr_set_clip(VRaster *r, int w, int h);

void vr_move_to(VRaster *r, int x, int y);
void vr_line_to(VRaster *r, int x, int y);
void vr_close(VRaster *r);

/* Emits the spans of everything added since the last sweep (non-zero rule), then clears. */
void vr_sweep(VRaster *r, vr_span_fn span, void *ctx);
void vr_clear(VRaster *r);

/* Line outlines. xy: n points (x, y pairs, 1/256 pixel); width in 1/256 pixel. round: round ends
 * (otherwise square-cut at the end points); bends are always rounded. closed: the last point joins
 * the first. dashes: lengths on, off, on, off ... in 1/256 pixel (ndash 0: solid). */
void vr_stroke(VRaster *r, const int *xy, int n, int width, int round, int closed, const int *dashes, int ndash);

/* A filled disc at (x, y), radius in 1/256 pixel. */
void vr_disc(VRaster *r, int x, int y, int radius);

/* Integer square root. */
unsigned vr_isqrt(unsigned v);

/* A picture to draw into: 16-bit RGB565 (px), or 32-bit 0x00RRGGBB when px32 is set (the head
 * unit's screen has 32 bits per pixel, and copying a 32-bit picture to it needs no conversion). */
typedef struct {
    unsigned short *px;       /* top row (16-bit) */
    int w, h, stride;         /* stride in pixels, negative for bottom-up bitmaps */
    unsigned *px32;           /* top row (32-bit); NULL for a 16-bit picture */
} VSurface;

typedef struct {
    VSurface *s;
    unsigned c32;             /* colour spread out for mixing (see spread() in vraster.c) */
    unsigned short c565;
    unsigned rgb;             /* the colour for 32-bit pictures */
    int opacity;              /* 0 to 256 */
} VrPaint;

/* rgb 0x00RRGGBB; opacity 0 to 256; dim halves the brightness. */
void vr_paint_init(VrPaint *p, VSurface *s, unsigned rgb, int opacity, int dim);
void vr_paint_span(void *ctx, int y, int x, int len, int coverage);

typedef struct {
    unsigned char *mask;      /* w x h coverage bytes, 0 to 255 */
    int w;
} VrMask;
void vr_mask_span(void *ctx, int y, int x, int len, int coverage);

/* Mixes rgb into the surface through a w x h mask placed at (x, y). */
void vr_blit_mask(VSurface *s, int x, int y, const unsigned char *mask, int w, int h, unsigned rgb, int dim);

unsigned short vr_rgb565(unsigned rgb, int dim);
void vr_fill_surface(VSurface *s, unsigned rgb, int dim);

/* A see-through panel: mixes rgb at alpha (0 clear to 256 solid) into the box (x, y, w, h) with
 * corners rounded to radius r (measured at pixel centres); parts outside the surface are skipped.
 * The dashboard's panels over the map (tinted, never blurred: the user's rule). */
void vr_tint(VSurface *s, int x, int y, int w, int h, int r, unsigned rgb, int alpha);

/* RGB565 to 0x00RRGGBB, low bits filled so white stays white. */
#define VR_565_TO_32(c)                                                                                               \
    ((((unsigned)(c)&0xF800u) << 8) | (((unsigned)(c)&0xE000u) << 3) | (((unsigned)(c)&0x07E0u) << 5) |              \
     (((unsigned)(c)&0x0600u) >> 1) | (((unsigned)(c)&0x001Fu) << 3) | (((unsigned)(c)&0x001Cu) >> 2))

#endif
