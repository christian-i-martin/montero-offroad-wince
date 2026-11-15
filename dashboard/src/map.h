/* Offline map for the dashboard: reads Montero tile packs (*.pack, the format is described in
 * tools/map/mtpack.py), decodes their PNG tiles and composes the picture around a position.
 *
 * Tiles are 256 x 256 pixels and numbered like OpenStreetMap's (Web Mercator): at zoom z the
 * world is 2^z tiles wide and high, x grows east from 180 W, y grows south from about 85 N.
 * A "world pixel" is a pixel position on that whole-world picture at one zoom level.
 *
 * Portable C: files are read through a callback, so the same code runs on the head unit and in
 * the tests on the build machine. Decoded tiles are kept in a small cache as RGB565 (16-bit
 * colour, the head unit's screen format). */
#ifndef MAP_H
#define MAP_H

#define MAP_TILE 256
#define MAP_MAX_PACKS 128
#define MAP_MAX_TILE_BYTES (1024 * 1024)
#define MAP_OUTSIDE_RGB 0x20262C   /* drawn where no pack has tiles */

enum { MAP_NO_FILES = -1, MAP_OUTSIDE = 0, MAP_SHOWN = 1 };

/* Map styles: dark packs (the Map folder) are drawn at night, and by day when there are no light
 * ones; light packs (the Map\Day folder) by day. */
enum { MAP_DARK = 0, MAP_LIGHT = 1, MAP_STYLES = 2 };

/* Reads size bytes at offset into buffer. Returns the number of bytes read, or -1. */
typedef int (*map_read_fn)(void *file, unsigned offset, void *buffer, unsigned size);

typedef struct {
    void *file;
    map_read_fn read;
    int zoom, style;
    unsigned x0, y0, cols, rows, index_offset, background;
} MapPack;

typedef struct {
    int used, zoom, style;
    unsigned x, y, last_use;
    int solid;                        /* drawn as one colour: an empty entry or a tile that failed */
    unsigned fill;                    /* that colour, 0x00RRGGBB */
    unsigned short px[MAP_TILE * MAP_TILE];
} MapTile;

typedef struct {
    MapPack packs[MAP_MAX_PACKS];
    int count;                        /* packs of both styles */
    int style_packs[MAP_STYLES];
    int style;                        /* the style drawn (map_use_style); MAP_DARK at first */
    int min_zoom, max_zoom;           /* zoom levels of that style; max_zoom < 0 when it has none */
    MapTile *cache;
    int cache_size;
    unsigned clock;
    unsigned char *bytes;             /* compressed bytes of the tile being decoded */
    unsigned bytes_size;
    unsigned decoded, failed, read_errors;
} MapSet;

/* cache_tiles decoded tiles are kept (each 128 KB). Returns 0 when memory runs out. */
int map_init(MapSet *s, int cache_tiles);
void map_free(MapSet *s);

/* Adds one pack file (a dark one, or of the given style). Returns 1 when its header is valid; the
 * caller keeps the file open. While the style in use has no packs, the new pack's style is used. */
int map_add_pack(MapSet *s, void *file, map_read_fn read);
int map_add_pack_style(MapSet *s, void *file, map_read_fn read, int style);

/* Draws this style from now on, or the other one when this one has no packs. Sets min_zoom and
 * max_zoom to the zoom levels of the style in use, and returns that style. */
int map_use_style(MapSet *s, int style);

/* 1 when some pack of the style in use has tiles at this zoom level. */
int map_has_zoom(const MapSet *s, int zoom);

/* World pixel of a position at a zoom level (Web Mercator). */
void map_world_pixel(double lat, double lon, int zoom, double *px, double *py);

/* Ground distance covered by one pixel at this latitude and zoom, in metres. */
double map_metres_per_pixel(double lat, int zoom);

/* Draws the map picture for a w x h box whose centre is world pixel (cx, cy) at zoom into out
 * (RGB565, stride in pixels, may be negative for bottom-up bitmaps; out points at the top row).
 * night halves the brightness. Returns MAP_SHOWN when a pack covers the centre, MAP_OUTSIDE
 * when none does (the box is then mostly MAP_OUTSIDE_RGB), MAP_NO_FILES without packs. */
int map_compose(MapSet *s, unsigned short *out, int w, int h, int stride, int zoom, double cx, double cy, int night);

/* Compressed PNG bytes of one tile, for the preview on the build machine. Returns the length
 * (0 for an empty entry, -1 when no pack has the tile); *data points into the set's buffer. */
int map_tile_bytes(MapSet *s, int zoom, unsigned x, unsigned y, const unsigned char **data);

/* Semi-transparent panel: mixes colour rgb (0x00RRGGBB) at alpha (0 clear to 256 solid) into the
 * box (bx, by, bw, bh) of a w x h picture (RGB565, stride in pixels, may be negative; out points
 * at the top row), with corners rounded to radius r. Parts outside the picture are skipped. */
void map_tint(unsigned short *out, int w, int h, int stride, int bx, int by, int bw, int bh, int r, unsigned rgb,
              int alpha);

/* 0x00RRGGBB to RGB565, and halving for night. */
unsigned short map_rgb565(unsigned rgb);
#define MAP_DIM565(c) ((unsigned short)(((c) >> 1) & 0x7BEF))

/* PNG decoding (pngdec.c, stb_image): RGB bytes, 3 per pixel, or NULL. Free with map_png_free. */
unsigned char *map_png_decode(const unsigned char *data, int length, int *w, int *h);
void map_png_free(unsigned char *pixels);

#endif
