/* Offline address lookup for the dashboard: reads a Montero address file (*.addr, the format and
 * the reference lookup are in tools/address/mtaddr.py) and turns a GPS position into the nearest
 * named road, the barangay, the city or municipality, the province and the nearest named place.
 *
 * Portable C: the file is read through a callback, so the same code runs on the head unit and in
 * the tests on the build machine. A lookup reads one grid cell's list, then the outlines and
 * roads near the position; nothing is kept between lookups except two small buffers. The
 * result is for the screen only: never log it (it is as revealing as the position). */
#ifndef ADDR_H
#define ADDR_H

#define ADDR_NAME_MAX 96
#define ADDR_ROAD_MATCH_M 50.0
#define ADDR_MAX_SHAPE_BYTES (2u * 1024u * 1024u)   /* larger outlines are skipped */
#define ADDR_CHUNK_ITEMS 128                         /* items read from the file at a time */

enum { ADDR_PROVINCE = 1, ADDR_CITY = 2, ADDR_BARANGAY = 3, ADDR_ROAD = 4, ADDR_PLACE = 5 };

/* Reads size bytes at offset into buffer. Returns the number of bytes read, or -1. */
typedef int (*addr_read_fn)(void *file, unsigned offset, void *buffer, unsigned size);

typedef struct {
    char road[ADDR_NAME_MAX];        /* empty when no named road is within ADDR_ROAD_MATCH_M */
    char barangay[ADDR_NAME_MAX];
    char city[ADDR_NAME_MAX];
    char province[ADDR_NAME_MAX];
    char place[ADDR_NAME_MAX];       /* nearest named place within the file's place radius */
    int road_m, place_m;             /* distances in metres, -1 without a name */
} AddrResult;

typedef struct {
    void *file;
    addr_read_fn read;
    int lon0, lat0;
    unsigned cell, cols, rows, cells_offset, items_offset, item_count, coords_offset, names_offset, names_size;
    unsigned road_radius_m, place_radius_m;
    unsigned char *items;            /* ADDR_CHUNK_ITEMS item records */
    unsigned char *coords;           /* one shape's coordinates */
    unsigned coords_size;
    unsigned lookups, read_errors, skipped;
} AddrFile;

/* Returns 1 when the header is valid (the caller keeps the file open), 0 otherwise. */
int addr_open(AddrFile *a, void *file, addr_read_fn read);
void addr_close(AddrFile *a);

/* Position in micro-degrees. Fills r (empty strings where nothing was found) and returns 1 when
 * the position is inside the file's grid, 0 when it is outside (r is then empty). */
int addr_lookup(AddrFile *a, int lon_e6, int lat_e6, AddrResult *r);

#endif
