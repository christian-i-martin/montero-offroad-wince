/* Self-test for the map reader (dashboard/src/map.c) against packs written by
 * tests/test_map_reader.py with tools/map/mtpack.py.
 * Usage: map_selftest GOOD14.pack GOOD15.pack NOT_A_PACK
 *
 * GOOD14: zoom 14, tiles x 1000..1002, y 2000..2001, background 0x102030:
 *   (1000,2000) RGB tile, solid A        (1001,2000) RGB tile, quadrants TL B, TR C, BL E, BR F
 *   (1002,2000) palette tile, solid D    (1000,2001) the same bytes as (1000,2000)
 *   (1001,2001) bytes that are not a PNG (1002,2001) no tile
 * GOOD15: zoom 15, one tile (5,5), solid A. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "map.h"

#define A 0xC82828u
#define B 0x00C800u
#define C 0x0000C8u
#define D 0xF0F010u
#define E 0xC8C8C8u
#define F 0x640064u
#define BG 0x102030u

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int file_read_at(void *file, unsigned offset, void *buffer, unsigned size)
{
    if (fseek((FILE *)file, (long)offset, SEEK_SET) != 0)
        return -1;
    return (int)fread(buffer, 1, size, (FILE *)file);
}

static unsigned short px[256 * 256];

/* Compose a w x h box centred on world pixel (cx, cy) at zoom 14 into px (stride w). */
static int at(MapSet *s, int w, int h, double cx, double cy, int night)
{
    memset(px, 0xAB, sizeof px);
    return map_compose(s, px, w, h, w, 14, cx, cy, night);
}

static unsigned short c565(unsigned rgb) { return map_rgb565(rgb); }

int main(int argc, char **argv)
{
    MapSet s, small;
    FILE *f14, *f15, *junk;
    const unsigned char *data;
    double x, y;
    unsigned short bottom_up[16 * 8];
    int n, i;

    if (argc != 4) {
        fprintf(stderr, "usage: %s GOOD14.pack GOOD15.pack NOT_A_PACK\n", argv[0]);
        return 2;
    }
    f14 = fopen(argv[1], "rb");
    f15 = fopen(argv[2], "rb");
    junk = fopen(argv[3], "rb");
    if (!f14 || !f15 || !junk)
        return 2;

    CHECK(map_init(&s, 12));
    CHECK(at(&s, 8, 8, 1000.0 * 256, 2000.0 * 256, 0) == MAP_NO_FILES);
    CHECK(!map_add_pack(&s, junk, file_read_at));
    CHECK(map_add_pack(&s, f14, file_read_at));
    CHECK(map_add_pack(&s, f15, file_read_at));
    CHECK(s.count == 2 && s.min_zoom == 14 && s.max_zoom == 15);
    CHECK(map_has_zoom(&s, 14) && map_has_zoom(&s, 15) && !map_has_zoom(&s, 13));

    /* projection */
    map_world_pixel(0.0, 0.0, 1, &x, &y);
    CHECK(fabs(x - 256.0) < 1e-6 && fabs(y - 256.0) < 1e-6);
    map_world_pixel(85.0511287798, -180.0, 0, &x, &y);
    CHECK(fabs(x) < 1e-6 && fabs(y) < 1e-3);
    map_world_pixel(14.5995, 120.9842, 14, &x, &y); /* Manila: tile 13698, 7520 at zoom 14 */
    CHECK((int)(x / 256) == 13698 && (int)(y / 256) == 7520);
    CHECK(fabs(map_metres_per_pixel(0.0, 0) - 156543.034) < 0.01);
    CHECK(fabs(map_metres_per_pixel(60.0, 1) - 156543.034 / 4) < 0.01);
    CHECK(map_rgb565(0xFFFFFF) == 0xFFFF && map_rgb565(0xF80000) == 0xF800 && map_rgb565(0x0000F8) == 0x001F);
    CHECK(MAP_DIM565(0xFFFF) == 0x7BEF);

    /* raw tile bytes */
    n = map_tile_bytes(&s, 14, 1000, 2000, &data);
    CHECK(n > 8 && memcmp(data, "\x89PNG", 4) == 0);
    CHECK(map_tile_bytes(&s, 14, 1002, 2001, &data) == 0);
    CHECK(map_tile_bytes(&s, 14, 1003, 2000, &data) == -1);
    CHECK(map_tile_bytes(&s, 13, 1000, 2000, &data) == -1);

    /* quadrants of tile (1001, 2000): a 64 x 32 box centred on the tile's middle */
    CHECK(at(&s, 64, 32, 1001.0 * 256 + 128, 2000.0 * 256 + 128, 0) == MAP_SHOWN);
    CHECK(px[0] == c565(B) && px[31] == c565(B) && px[32] == c565(C) && px[63] == c565(C));
    CHECK(px[15 * 64] == c565(B) && px[16 * 64] == c565(E) && px[16 * 64 + 32] == c565(F) && px[31 * 64 + 63] == c565(F));
    CHECK(s.decoded == 1);

    /* across the border of tiles 1000 and 1001, and the palette tile 1002 */
    CHECK(at(&s, 64, 8, 1001.0 * 256, 2000.0 * 256 + 50, 0) == MAP_SHOWN);
    CHECK(px[0] == c565(A) && px[31] == c565(A) && px[32] == c565(B) && px[63] == c565(B));
    CHECK(at(&s, 16, 16, 1002.0 * 256 + 128, 2000.0 * 256 + 128, 0) == MAP_SHOWN);
    CHECK(px[0] == c565(D) && px[255] == c565(D));
    CHECK(s.decoded == 3 && s.failed == 0);

    /* the same bytes shared by (1000, 2001), the corrupt tile and the empty entry */
    CHECK(at(&s, 16, 16, 1000.0 * 256 + 128, 2001.0 * 256 + 128, 0) == MAP_SHOWN && px[0] == c565(A));
    CHECK(at(&s, 16, 16, 1001.0 * 256 + 128, 2001.0 * 256 + 128, 0) == MAP_SHOWN && px[0] == c565(BG));
    CHECK(s.failed == 1);
    CHECK(at(&s, 16, 16, 1001.0 * 256 + 128, 2001.0 * 256 + 128, 0) == MAP_SHOWN && s.failed == 1); /* remembered */
    CHECK(at(&s, 16, 16, 1002.0 * 256 + 128, 2001.0 * 256 + 128, 0) == MAP_SHOWN && px[255] == c565(BG));

    /* outside the pack: right of it, and north of the world */
    CHECK(at(&s, 16, 16, 1003.0 * 256 + 128, 2000.0 * 256 + 128, 0) == MAP_OUTSIDE && px[0] == c565(MAP_OUTSIDE_RGB));
    CHECK(at(&s, 64, 8, 1003.0 * 256, 2000.0 * 256 + 50, 0) == MAP_OUTSIDE); /* centre just outside */
    CHECK(px[0] == c565(D) && px[63] == c565(MAP_OUTSIDE_RGB));
    CHECK(at(&s, 16, 16, 1000.0 * 256 + 128, -500.0, 0) == MAP_OUTSIDE && px[0] == c565(MAP_OUTSIDE_RGB));

    /* night halves the brightness; the cache already holds every tile */
    n = (int)s.decoded;
    CHECK(at(&s, 64, 32, 1001.0 * 256 + 128, 2000.0 * 256 + 128, 1) == MAP_SHOWN);
    CHECK(px[0] == MAP_DIM565(c565(B)) && px[63] == MAP_DIM565(c565(C)));
    CHECK((int)s.decoded == n);

    /* bottom-up bitmap: negative stride, out points at the top row (the last one in memory) */
    CHECK(map_compose(&s, bottom_up + 7 * 16, 16, 8, -16, 14, 1001.0 * 256 + 128, 2000.0 * 256 + 128, 0) == MAP_SHOWN);
    CHECK(bottom_up[7 * 16] == c565(B) && bottom_up[7 * 16 + 15] == c565(C));   /* top row */
    CHECK(bottom_up[0] == c565(E) && bottom_up[15] == c565(F));                 /* bottom row */

    /* zoom 15 pack */
    memset(px, 0, sizeof px);
    CHECK(map_compose(&s, px, 8, 8, 8, 15, 5.0 * 256 + 128, 5.0 * 256 + 128, 0) == MAP_SHOWN && px[0] == c565(A));

    /* a cache of two tiles still draws correctly while it evicts */
    CHECK(map_init(&small, 2));
    rewind(f14);
    CHECK(map_add_pack(&small, f14, file_read_at));
    for (i = 0; i < 3; i++) {
        CHECK(map_compose(&small, px, 8, 8, 8, 14, 1000.0 * 256 + 128, 2000.0 * 256 + 128, 0) == MAP_SHOWN &&
              px[0] == c565(A));
        CHECK(map_compose(&small, px, 8, 8, 8, 14, 1001.0 * 256 + 10, 2000.0 * 256 + 10, 0) == MAP_SHOWN &&
              px[0] == c565(B));
        CHECK(map_compose(&small, px, 8, 8, 8, 14, 1002.0 * 256 + 128, 2000.0 * 256 + 128, 0) == MAP_SHOWN &&
              px[0] == c565(D));
    }
    CHECK(small.decoded == 9); /* three tiles, two slots: every draw decodes again */
    map_free(&small);
    map_free(&s);
    fclose(f14);
    fclose(f15);
    fclose(junk);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("map self-test passed\n");
    return 0;
}
