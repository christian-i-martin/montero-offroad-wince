/* The holiday list (holidays.txt next to the program, built by tools/holidays/build_holidays.py,
 * whose notes give the format): national holidays, and local ones for a town or a province, each
 * a regular holiday, a special non-working day or a special working holiday.
 *
 * Where the car is comes from the GPS position alone: the list holds every Luzon city and
 * municipality with its centre and size, and the car is in the town whose centre is nearest,
 * each distance divided by the town's radius (holiday_locate, the same rule as locate() in
 * build_holidays.py). It does not use the map or the address file. The town found is for the
 * screen only: never log it (it is as revealing as the position).
 *
 * Portable C: the program reads the file into memory and hands the text over. */
#ifndef HOLIDAY_H
#define HOLIDAY_H

#define HOLIDAY_NAME_MAX 80
#define HOLIDAY_ROWS 3             /* rows shown at most for one day */
#define HOLIDAY_MAX_PROVINCES 64
#define HOLIDAY_MAX_TOWNS 1024
#define HOLIDAY_MAX_ENTRIES 1536

enum { HOLIDAY_REGULAR = 'R', HOLIDAY_SPECIAL = 'S', HOLIDAY_WORKING = 'W' };

/* A regular holiday and a special non-working day are days off; a special working day is not. */
#define HOLIDAY_DAY_OFF(kind) ((kind) == HOLIDAY_REGULAR || (kind) == HOLIDAY_SPECIAL)

/* One holiday as the dashboard shows it. */
typedef struct {
    char name[HOLIDAY_NAME_MAX];
    char place[HOLIDAY_NAME_MAX];  /* the town or province of a local holiday; empty for a national one */
    int kind;                      /* HOLIDAY_REGULAR, HOLIDAY_SPECIAL or HOLIDAY_WORKING */
    int local;                     /* 1 for a local holiday */
} HolidayRow;

typedef struct {
    int province, lat_e5, lon_e5, radius_m;   /* centre in 1/100,000 degrees */
    const char *name;
} HolidayTown;

typedef struct {
    long first, last;              /* days since 1970-01-01 */
    int kind;
    const char *name, *places;     /* places: "P3 T120", empty for the whole country */
} HolidayEntry;

typedef struct {
    long first_day, last_day;      /* the dates the list covers; first_day > last_day without a V line */
    int province_count, town_count, count, bad_lines;
    const char *province[HOLIDAY_MAX_PROVINCES];
    HolidayTown town[HOLIDAY_MAX_TOWNS];
    HolidayEntry entry[HOLIDAY_MAX_ENTRIES];
} HolidayList;

/* Parses the file's text in place (tabs and line ends become string ends), so the text must stay
 * in memory while the list is used. Lines it cannot read are counted in bad_lines. Returns the
 * number of holidays. */
int holiday_parse(HolidayList *h, char *text, int size);

/* 1 when the date is inside the dates the list covers. */
int holiday_covers(const HolidayList *h, int year, int month, int day);

/* The town a position is in (index into h->town), or -1 when no town centre is near. */
int holiday_locate(const HolidayList *h, int lat_e5, int lon_e5);

/* The holidays of a day at a town (-1 when not known: national holidays only), as the dashboard
 * shows them: national first, then the province's, then the town's; a holiday held under one name
 * at several levels is shown once, at the most specific. Returns the number of rows (at most max). */
int holiday_today(const HolidayList *h, int year, int month, int day, int town, HolidayRow *rows, int max);

#endif
