/* Text for the vector map, drawn by our own code instead of GDI: letters are read from a TrueType
 * file (the Barlow fonts the dashboard ships) as outlines and filled through vraster.c, so a label
 * can be turned to any angle (road names follow their road) and gets a halo (a soft outline in
 * the background colour that keeps it readable over roads).
 *
 * Reads the glyf, loca, cmap (format 4), hmtx, hhea, head and maxp tables; composite letters
 * (an n with a tilde: n plus a tilde) are combined. No kerning, no hinting. Whole-number maths
 * only. Portable C, tested on the build machine. */
#ifndef VFONT_H
#define VFONT_H

#include "vraster.h"

typedef struct {
    short x, y;
    unsigned char on;         /* on the curve, otherwise a curve control point */
} VfPoint;

typedef struct {
    unsigned glyph;
    int used;
    int npoints, ncontours;
    VfPoint *points;
    unsigned short *ends;     /* last point of each contour */
} VfGlyph;

#define VF_CACHE 512
#define VF_MAP 0x3000             /* letters below this code point are looked up once and kept */

typedef struct {
    const unsigned char *data;
    unsigned size;
    unsigned glyf, loca, cmap4, hmtx, glyph_count, metrics;
    int loca_long;
    int units;                /* font units per em (the text size) */
    int ascent, descent;      /* font units, descent negative */
    VfGlyph cache[VF_CACHE];
    unsigned short map[VF_MAP];   /* code point -> glyph; 0xFFFF until looked up */
} VFont;

/* data stays owned by the caller and must stay valid. Returns 1 when the font can be used. */
int vf_load(VFont *f, const unsigned char *data, unsigned size);
void vf_free(VFont *f);

/* Next character of UTF-8 text (advances *p; 0 at the end). */
unsigned vf_next_char(const char **p);
unsigned vf_glyph(const VFont *f, unsigned codepoint);
int vf_advance(const VFont *f, unsigned glyph);   /* font units */

/* The outline of a letter, or NULL (empty letters such as the space have none). */
const VfGlyph *vf_outline(VFont *f, unsigned glyph);

/* Adds a letter's outline to the raster. A point (fx, fy) in font units (y up) lands at
 * x = tx + (a * fx + b * fy) / 65536, y = ty + (c * fx + d * fy) / 65536, in 1/256 pixel. */
void vf_add(VRaster *r, VFont *f, unsigned glyph, int a, int b, int c, int d, int tx, int ty);

#endif
