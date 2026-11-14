#include "map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAP_PI 3.14159265358979323846
#define MAP_MAX_ZOOM 20 /* world pixels must fit in 32-bit numbers on the head unit */
#define MAX_LAT 85.0511287798

static unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

unsigned short map_rgb565(unsigned rgb)
{
    return (unsigned short)((((rgb >> 19) & 0x1F) << 11) | (((rgb >> 10) & 0x3F) << 5) | ((rgb >> 3) & 0x1F));
}

int map_init(MapSet *s, int cache_tiles)
{
    int i;
    memset(s, 0, sizeof *s);
    s->max_zoom = -1;
    if (cache_tiles < 1)
        cache_tiles = 1;
    /* malloc and a loop rather than calloc (GCC turns malloc plus memset into calloc), and map_ln
     * rather than log: the dashboard only uses COREDLL functions already seen working on the car,
     * since a missing import stops the exe from loading. A slot is valid once `used` is set. */
    s->cache = (MapTile *)malloc((size_t)cache_tiles * sizeof(MapTile));
    if (s->cache == NULL)
        return 0;
    for (i = 0; i < cache_tiles; i++)
        s->cache[i].used = 0;
    s->cache_size = cache_tiles;
    return 1;
}

void map_free(MapSet *s)
{
    free(s->cache);
    free(s->bytes);
    memset(s, 0, sizeof *s);
    s->max_zoom = -1;
}

int map_add_pack(MapSet *s, void *file, map_read_fn read)
{
    unsigned char h[64];
    MapPack p;
    unsigned side;

    if (s->count >= MAP_MAX_PACKS || read(file, 0, h, sizeof h) != (int)sizeof h)
        return 0;
    if (memcmp(h, "MTPK", 4) != 0 || le32(h + 4) != 1 || le32(h + 28) != MAP_TILE || le32(h + 32) != 1)
        return 0;
    if (le32(h + 8) > MAP_MAX_ZOOM)
        return 0;
    p.file = file;
    p.read = read;
    p.zoom = (int)le32(h + 8);
    p.x0 = le32(h + 12);
    p.y0 = le32(h + 16);
    p.cols = le32(h + 20);
    p.rows = le32(h + 24);
    p.index_offset = le32(h + 36);
    p.background = le32(h + 40) & 0xFFFFFF;
    side = 1u << p.zoom;
    if (p.cols == 0 || p.rows == 0 || p.x0 >= side || p.y0 >= side || p.cols > side - p.x0 ||
        p.rows > side - p.y0 || p.index_offset < 64)
        return 0;
    s->packs[s->count++] = p;
    if (s->max_zoom < 0) {
        s->min_zoom = s->max_zoom = p.zoom;
    } else {
        if (p.zoom < s->min_zoom)
            s->min_zoom = p.zoom;
        if (p.zoom > s->max_zoom)
            s->max_zoom = p.zoom;
    }
    return 1;
}

int map_has_zoom(const MapSet *s, int zoom)
{
    int i;
    for (i = 0; i < s->count; i++)
        if (s->packs[i].zoom == zoom)
            return 1;
    return 0;
}

static const MapPack *find_pack(const MapSet *s, int zoom, unsigned x, unsigned y)
{
    int i;
    for (i = 0; i < s->count; i++) {
        const MapPack *p = &s->packs[i];
        if (p->zoom == zoom && x - p->x0 < p->cols && y - p->y0 < p->rows) /* unsigned: also x >= x0 */
            return p;
    }
    return NULL;
}

int map_tile_bytes(MapSet *s, int zoom, unsigned x, unsigned y, const unsigned char **data)
{
    const MapPack *p = find_pack(s, zoom, x, y);
    unsigned char e[8];
    unsigned offset, length;

    *data = NULL;
    if (p == NULL)
        return -1;
    if (p->read(p->file, p->index_offset + 8u * ((y - p->y0) * p->cols + (x - p->x0)), e, 8) != 8) {
        s->read_errors++;
        return 0;
    }
    offset = le32(e);
    length = le32(e + 4);
    if (length == 0)
        return 0;
    if (length > MAP_MAX_TILE_BYTES) {
        s->failed++;
        return 0;
    }
    if (length > s->bytes_size) {
        unsigned char *bigger = (unsigned char *)realloc(s->bytes, length);
        if (bigger == NULL) {
            s->failed++;
            return 0;
        }
        s->bytes = bigger;
        s->bytes_size = length;
    }
    if (p->read(p->file, offset, s->bytes, length) != (int)length) {
        s->read_errors++;
        return 0;
    }
    *data = s->bytes;
    return (int)length;
}

/* The decoded tile from the cache, decoding it on a miss. Returns NULL with *fill set when the
 * tile is drawn as one colour: outside every pack, an empty entry, or a tile that failed. */
static const unsigned short *get_tile(MapSet *s, int zoom, unsigned x, unsigned y, unsigned *fill)
{
    MapTile *slot = NULL;
    const MapPack *p;
    const unsigned char *data;
    unsigned char *rgb;
    int i, n, w = 0, h = 0, ok = 0;

    s->clock++;
    for (i = 0; i < s->cache_size; i++) {
        MapTile *t = &s->cache[i];
        if (t->used && t->zoom == zoom && t->x == x && t->y == y) {
            t->last_use = s->clock;
            *fill = t->fill;
            return t->solid ? NULL : t->px;
        }
        if (slot == NULL || (slot->used && (!t->used || t->last_use < slot->last_use)))
            slot = t; /* a free slot, otherwise the least recently used */
    }
    p = find_pack(s, zoom, x, y);
    if (p == NULL) {
        *fill = MAP_OUTSIDE_RGB;
        return NULL;
    }
    n = map_tile_bytes(s, zoom, x, y, &data);
    rgb = n > 0 ? map_png_decode(data, n, &w, &h) : NULL;
    if (rgb != NULL && w == MAP_TILE && h == MAP_TILE) {
        const unsigned char *q = rgb;
        for (i = 0; i < MAP_TILE * MAP_TILE; i++, q += 3)
            slot->px[i] = (unsigned short)(((q[0] >> 3) << 11) | ((q[1] >> 2) << 5) | (q[2] >> 3));
        ok = 1;
        s->decoded++;
    } else if (n > 0) {
        s->failed++;
    }
    if (rgb != NULL)
        map_png_free(rgb);
    slot->used = 1;
    slot->zoom = zoom;
    slot->x = x;
    slot->y = y;
    slot->last_use = s->clock;
    slot->solid = !ok;
    slot->fill = p->background;
    *fill = p->background;
    return ok ? slot->px : NULL;
}

/* Natural logarithm for x > 0, to about 1e-15: x = m * 2^k with m in [1, 2), then
 * ln m = 2 (z + z^3/3 + z^5/5 + ...) with z = (m - 1) / (m + 1) <= 1/3. */
static double map_ln(double x)
{
    double z, z2, term, sum = 0.0;
    int k = 0, n;
    if (x <= 0.0)
        return -1e300;
    while (x >= 2.0) {
        x /= 2.0;
        k++;
    }
    while (x < 1.0) {
        x *= 2.0;
        k--;
    }
    z = (x - 1.0) / (x + 1.0);
    z2 = z * z;
    term = z;
    for (n = 1; n < 60; n += 2) {
        sum += term / n;
        term *= z2;
    }
    return 2.0 * sum + k * 0.69314718055994530942;
}

void map_world_pixel(double lat, double lon, int zoom, double *px, double *py)
{
    double size, s;
    if (zoom < 0)
        zoom = 0;
    if (zoom > MAP_MAX_ZOOM)
        zoom = MAP_MAX_ZOOM;
    if (lat > MAX_LAT)
        lat = MAX_LAT;
    if (lat < -MAX_LAT)
        lat = -MAX_LAT;
    size = (double)MAP_TILE * (double)(1u << zoom);
    s = sin(lat * MAP_PI / 180.0);
    *px = (lon + 180.0) / 360.0 * size;
    *py = (0.5 - map_ln((1.0 + s) / (1.0 - s)) / (4.0 * MAP_PI)) * size;
}

double map_metres_per_pixel(double lat, int zoom)
{
    if (zoom < 0)
        zoom = 0;
    if (zoom > MAP_MAX_ZOOM)
        zoom = MAP_MAX_ZOOM;
    return 156543.03392804097 * cos(lat * MAP_PI / 180.0) / (double)(1u << zoom);
}

static void fill_part(unsigned short *out, int stride, int x0, int y0, int x1, int y1, unsigned short c)
{
    int x, y;
    for (y = y0; y < y1; y++) {
        unsigned short *d = out + (long)y * stride;
        for (x = x0; x < x1; x++)
            d[x] = c;
    }
}

int map_compose(MapSet *s, unsigned short *out, int w, int h, int stride, int zoom, double cx, double cy, int night)
{
    long left, top, tx, ty, side;
    int result;

    if (s->count == 0)
        return MAP_NO_FILES;
    if (zoom < 0 || zoom > MAP_MAX_ZOOM || w <= 0 || h <= 0)
        return MAP_OUTSIDE;
    side = 1L << zoom;
    left = (long)floor(cx - w / 2.0);
    top = (long)floor(cy - h / 2.0);
    tx = (long)floor(cx / MAP_TILE);
    ty = (long)floor(cy / MAP_TILE);
    result = ty >= 0 && ty < side && find_pack(s, zoom, (unsigned)(((tx % side) + side) % side), (unsigned)ty)
           ? MAP_SHOWN : MAP_OUTSIDE;

    for (ty = (long)floor((double)top / MAP_TILE); ty * MAP_TILE < top + h; ty++) {
        long ty0 = ty * MAP_TILE;
        int y0 = (int)((ty0 > top ? ty0 : top) - top);
        int y1 = (int)((ty0 + MAP_TILE < top + h ? ty0 + MAP_TILE : top + h) - top);
        for (tx = (long)floor((double)left / MAP_TILE); tx * MAP_TILE < left + w; tx++) {
            long tx0 = tx * MAP_TILE;
            int x0 = (int)((tx0 > left ? tx0 : left) - left);
            int x1 = (int)((tx0 + MAP_TILE < left + w ? tx0 + MAP_TILE : left + w) - left);
            unsigned fill = MAP_OUTSIDE_RGB;
            const unsigned short *px = NULL;
            int y;
            if (ty >= 0 && ty < side)
                px = get_tile(s, zoom, (unsigned)(((tx % side) + side) % side), (unsigned)ty, &fill);
            if (px == NULL) {
                unsigned short c = map_rgb565(fill);
                fill_part(out, stride, x0, y0, x1, y1, night ? MAP_DIM565(c) : c);
                continue;
            }
            for (y = y0; y < y1; y++) {
                const unsigned short *src = px + (long)(top + y - ty0) * MAP_TILE + (left + x0 - tx0);
                unsigned short *dst = out + (long)y * stride + x0;
                if (night) {
                    int x;
                    for (x = 0; x < x1 - x0; x++)
                        dst[x] = MAP_DIM565(src[x]);
                } else {
                    memcpy(dst, src, (size_t)(x1 - x0) * sizeof *dst);
                }
            }
        }
    }
    return result;
}
