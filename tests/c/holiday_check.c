/* Answers holiday questions with dashboard/src/holiday.c, for tests/test_holidays.py, which asks
 * the same of the reference reader in tools/holidays/build_holidays.py.
 * Usage: holiday_check HOLIDAYS_TXT < queries
 * Queries, one a line: "L lat_e5 lon_e5" (prints the town index, -1 for none) and
 * "D year month day town" (prints the rows: name, kind, local, place, separated by 0x1F; rows
 * separated by 0x1E; an empty line when there are none). */
#include <stdio.h>
#include <stdlib.h>

#include "holiday.h"

static char g_text[256 * 1024];
static HolidayList g_list;

int main(int argc, char **argv)
{
    FILE *f;
    size_t n;
    char line[128];

    if (argc != 2 || (f = fopen(argv[1], "rb")) == NULL) {
        fprintf(stderr, "usage: %s HOLIDAYS_TXT < queries\n", argv[0]);
        return 2;
    }
    n = fread(g_text, 1, sizeof g_text - 1, f);
    fclose(f);
    holiday_parse(&g_list, g_text, (int)n);
    fprintf(stderr, "%d holidays, %d towns, %d bad lines\n", g_list.count, g_list.town_count, g_list.bad_lines);
    while (fgets(line, sizeof line, stdin)) {
        int a, b, c, d;
        if (sscanf(line, "L %d %d", &a, &b) == 2) {
            printf("%d\n", holiday_locate(&g_list, a, b));
        } else if (sscanf(line, "D %d %d %d %d", &a, &b, &c, &d) == 4) {
            HolidayRow rows[HOLIDAY_ROWS];
            int i, k = holiday_today(&g_list, a, b, c, d, rows, HOLIDAY_ROWS);
            for (i = 0; i < k; i++)
                printf("%s%s\x1f%c\x1f%d\x1f%s", i ? "\x1e" : "", rows[i].name, rows[i].kind, rows[i].local, rows[i].place);
            printf("\n");
        }
    }
    return 0;
}
