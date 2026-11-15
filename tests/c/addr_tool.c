/* Answers address lookups through dashboard/src/addr.c, for tests/test_addr_reader.py.
 * Usage: addr_tool FILE.addr < points   (one "lon_e6 lat_e6" per line)
 * Prints one line per point: inside<TAB>province<TAB>city<TAB>barangay<TAB>road<TAB>road_m<TAB>place<TAB>place_m
 * and a last line "stats lookups read_errors skipped". */
#include <stdio.h>

#include "addr.h"

static int file_read_at(void *file, unsigned offset, void *buffer, unsigned size)
{
    if (fseek((FILE *)file, (long)offset, SEEK_SET) != 0)
        return -1;
    return (int)fread(buffer, 1, size, (FILE *)file);
}

int main(int argc, char **argv)
{
    AddrFile a;
    AddrResult r;
    FILE *f;
    int lon, lat;

    if (argc != 2 || (f = fopen(argv[1], "rb")) == NULL)
        return 2;
    if (!addr_open(&a, f, file_read_at)) {
        printf("not an address file\n");
        return 1;
    }
    while (scanf("%d %d", &lon, &lat) == 2) {
        int in = addr_lookup(&a, lon, lat, &r);
        printf("%d\t%s\t%s\t%s\t%s\t%d\t%s\t%d\n", in, r.province, r.city, r.barangay, r.road, r.road_m, r.place,
               r.place_m);
    }
    printf("stats %u %u %u\n", a.lookups, a.read_errors, a.skipped);
    addr_close(&a);
    fclose(f);
    return 0;
}
