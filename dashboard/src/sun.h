/* Sunrise and sunset times (the "sunrise equation", accurate to a minute or two).
 * Portable C so it can be tested on the build machine. */
#ifndef SUN_H
#define SUN_H

/* For the calendar date year-month-day at latitude/longitude (degrees, north and east positive),
 * set the minutes after 00:00 UTC of that date when the sun rises and sets. Values can be below 0
 * or above 1440 far from Greenwich. Returns 1, or 0 when the sun does not rise or set that day. */
int sun_times(int year, int month, int day, double lat, double lon, double *rise_utc_min, double *set_utc_min);

/* Days since 1970-01-01 for a calendar date, and back. */
long sun_days_from_civil(int year, int month, int day);
void sun_civil_from_days(long days, int *year, int *month, int *day);

#endif
