#include "addr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE 64
#define ITEM_SIZE 32
#define M_PER_DEG_LAT 110574.0
#define M_PER_DEG_LON_EQUATOR 111320.0

static unsigned le32(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static int sle32(const unsigned char *p) { return (int)le32(p); }

int addr_open(AddrFile *a, void *file, addr_read_fn read)
{
    unsigned char h[HEADER_SIZE];

    memset(a, 0, sizeof *a);
    if (read(file, 0, h, HEADER_SIZE) != HEADER_SIZE || memcmp(h, "MTAD", 4) != 0 || le32(h + 4) != 1)
        return 0;
    a->file = file;
    a->read = read;
    a->lon0 = sle32(h + 8);
    a->lat0 = sle32(h + 12);
    a->cell = le32(h + 16);
    a->cols = le32(h + 20);
    a->rows = le32(h + 24);
    a->cells_offset = le32(h + 28);
    a->items_offset = le32(h + 32);
    a->item_count = le32(h + 36);
    a->coords_offset = le32(h + 40);
    a->names_offset = le32(h + 44);
    a->names_size = le32(h + 48);
    a->road_radius_m = le32(h + 52);
    a->place_radius_m = le32(h + 56);
    if (a->cell == 0 || a->cols == 0 || a->rows == 0 || a->cols > 100000 || a->rows > 100000)
        return 0;
    a->items = (unsigned char *)malloc(ADDR_CHUNK_ITEMS * ITEM_SIZE);
    if (a->items == NULL)
        return 0;
    return 1;
}

void addr_close(AddrFile *a)
{
    free(a->items);
    free(a->coords);
    memset(a, 0, sizeof *a);
}

/* One shape's coordinates into a->coords. Returns the byte count, or 0. */
static unsigned load_coords(AddrFile *a, unsigned offset, unsigned bytes)
{
    if (bytes == 0 || bytes > ADDR_MAX_SHAPE_BYTES) {
        a->skipped++;
        return 0;
    }
    if (bytes > a->coords_size) {
        unsigned char *bigger = (unsigned char *)realloc(a->coords, bytes);
        if (bigger == NULL) {
            a->skipped++;
            return 0;
        }
        a->coords = bigger;
        a->coords_size = bytes;
    }
    if (a->read(a->file, a->coords_offset + offset, a->coords, bytes) != (int)bytes) {
        a->read_errors++;
        return 0;
    }
    return bytes;
}

/* Even-odd rule over all rings: holes and several outer rings work. */
static int inside(const unsigned char *rings, unsigned ring_count, unsigned points, int x, int y)
{
    const unsigned char *pt = rings + 4 * ring_count;
    unsigned r, used = 0;
    int odd = 0;
    for (r = 0; r < ring_count; r++) {
        unsigned n = le32(rings + 4 * r), i;
        if (used + n > points)
            break;
        for (i = 0; i + 1 < n; i++) {
            int x1 = sle32(pt + 8 * i), y1 = sle32(pt + 8 * i + 4);
            int x2 = sle32(pt + 8 * i + 8), y2 = sle32(pt + 8 * i + 12);
            if ((y1 > y) != (y2 > y) && (double)x < x1 + (double)(y - y1) * (double)(x2 - x1) / (double)(y2 - y1))
                odd = !odd;
        }
        pt += 8 * n;
        used += n;
    }
    return odd;
}

static double polyline_distance(const unsigned char *pt, unsigned n, int x, int y, double kx, double ky)
{
    double best = 1e30;
    unsigned i;
    for (i = 0; i + 1 < n; i++) {
        double ax = (sle32(pt + 8 * i) - x) * kx, ay = (sle32(pt + 8 * i + 4) - y) * ky;
        double bx = (sle32(pt + 8 * i + 8) - x) * kx, by = (sle32(pt + 8 * i + 12) - y) * ky;
        double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, t = 0.0, ex, ey, d2;
        if (len2 > 0) {
            t = -(ax * dx + ay * dy) / len2;
            if (t < 0)
                t = 0;
            if (t > 1)
                t = 1;
        }
        ex = ax + t * dx;
        ey = ay + t * dy;
        d2 = ex * ex + ey * ey;
        if (d2 < best)
            best = d2;
    }
    return pow(best, 0.5); /* pow, not sqrt: only COREDLL functions already used on the car */
}

static void read_name(AddrFile *a, unsigned offset, char *out)
{
    unsigned n = ADDR_NAME_MAX - 1;
    int got;
    out[0] = 0;
    if (offset >= a->names_size)
        return;
    if (n > a->names_size - offset)
        n = a->names_size - offset;
    got = a->read(a->file, a->names_offset + offset, out, n);
    if (got < 0) {
        a->read_errors++;
        out[0] = 0;
        return;
    }
    out[got] = 0; /* the name ends at its zero byte; a longer one is cut */
}

int addr_lookup(AddrFile *a, int lon_e6, int lat_e6, AddrResult *r)
{
    unsigned char cell_range[8];
    unsigned col, row, start, end, at;
    unsigned area_name[4] = {0, 0, 0, 0}, road_name = 0, place_name = 0;
    double area_size[4] = {-1, -1, -1, -1}, road_best = ADDR_ROAD_MATCH_M, place_best, kx, ky;
    int have_road = 0, have_place = 0, k;

    memset(r, 0, sizeof *r);
    r->road_m = r->place_m = -1;
    a->lookups++;
    if (lon_e6 < a->lon0 || lat_e6 < a->lat0)
        return 0;
    col = (unsigned)(lon_e6 - a->lon0) / a->cell;
    row = (unsigned)(lat_e6 - a->lat0) / a->cell;
    if (col >= a->cols || row >= a->rows)
        return 0;
    if (a->read(a->file, a->cells_offset + 4u * (row * a->cols + col), cell_range, 8) != 8) {
        a->read_errors++;
        return 1;
    }
    start = le32(cell_range);
    end = le32(cell_range + 4);
    if (end < start || end > a->item_count)
        return 1;
    kx = M_PER_DEG_LON_EQUATOR * cos(lat_e6 / 1e6 * 3.14159265358979323846 / 180.0) / 1e6;
    ky = M_PER_DEG_LAT / 1e6;
    place_best = (double)a->place_radius_m;

    for (at = start; at < end; at += ADDR_CHUNK_ITEMS) {
        unsigned n = end - at < ADDR_CHUNK_ITEMS ? end - at : ADDR_CHUNK_ITEMS, i;
        if (a->read(a->file, a->items_offset + ITEM_SIZE * at, a->items, n * ITEM_SIZE) != (int)(n * ITEM_SIZE)) {
            a->read_errors++;
            break;
        }
        for (i = 0; i < n; i++) {
            const unsigned char *it = a->items + ITEM_SIZE * i;
            unsigned kind = it[0], rings = (unsigned)it[2] | (unsigned)it[3] << 8, name = le32(it + 4);
            int x0 = sle32(it + 8), y0 = sle32(it + 12), x1 = sle32(it + 16), y1 = sle32(it + 20);
            unsigned coord = le32(it + 24), points = le32(it + 28);
            if (kind >= ADDR_PROVINCE && kind <= ADDR_BARANGAY) {
                double size = (double)(x1 - x0) * (double)(y1 - y0);
                if (lon_e6 < x0 || lon_e6 > x1 || lat_e6 < y0 || lat_e6 > y1)
                    continue;
                if (area_size[kind] >= 0 && size >= area_size[kind])
                    continue; /* a smaller one of this kind already contains the point */
                if (!load_coords(a, coord, 4 * rings + 8 * points))
                    continue;
                if (inside(a->coords, rings, points, lon_e6, lat_e6)) {
                    area_size[kind] = size;
                    area_name[kind] = name;
                }
            } else if (kind == ADDR_ROAD) {
                double d;
                /* the box grown by the match distance must contain the point */
                if ((lon_e6 - x1) * kx > road_best || (x0 - lon_e6) * kx > road_best ||
                    (lat_e6 - y1) * ky > road_best || (y0 - lat_e6) * ky > road_best)
                    continue;
                if (points < 2 || !load_coords(a, coord, 8 * points))
                    continue;
                d = polyline_distance(a->coords, points, lon_e6, lat_e6, kx, ky);
                if (d < road_best) {
                    road_best = d;
                    road_name = name;
                    have_road = 1;
                }
            } else if (kind == ADDR_PLACE) {
                double dx = (x0 - lon_e6) * kx, dy = (y0 - lat_e6) * ky, d = pow(dx * dx + dy * dy, 0.5);
                if (d < place_best) {
                    place_best = d;
                    place_name = name;
                    have_place = 1;
                }
            }
        }
    }
    for (k = ADDR_PROVINCE; k <= ADDR_BARANGAY; k++) {
        if (area_size[k] < 0)
            continue;
        read_name(a, area_name[k], k == ADDR_PROVINCE ? r->province : k == ADDR_CITY ? r->city : r->barangay);
    }
    if (have_road) {
        read_name(a, road_name, r->road);
        r->road_m = (int)floor(road_best + 0.5);
    }
    if (have_place) {
        read_name(a, place_name, r->place);
        r->place_m = (int)floor(place_best + 0.5);
    }
    return 1;
}
