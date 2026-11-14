#include "sun.h"

#include <math.h>

#define PI 3.14159265358979323846
#define RAD (PI / 180.0)

/* Howard Hinnant's days_from_civil / civil_from_days (proleptic Gregorian calendar). */
long sun_days_from_civil(int y, int m, int d)
{
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (long)y - era * 400;
    doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void sun_civil_from_days(long z, int *year, int *month, int *day)
{
    long era, doe, yoe, doy, mp, y;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(y + (*month <= 2));
}

static double wrap360(double a)
{
    a = fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

int sun_times(int year, int month, int day, double lat, double lon, double *rise_utc_min, double *set_utc_min)
{
    /* Julian date at 00:00 UTC of the date; 2440587.5 is 1970-01-01 00:00 UTC. */
    double jd0 = 2440587.5 + (double)sun_days_from_civil(year, month, day);
    double n = ceil(jd0 - 2451545.0 + 0.0008);
    double mean_noon = n - lon / 360.0;
    double m = wrap360(357.5291 + 0.98560028 * mean_noon);
    double c = 1.9148 * sin(m * RAD) + 0.0200 * sin(2 * m * RAD) + 0.0003 * sin(3 * m * RAD);
    double ecliptic = wrap360(m + c + 180.0 + 102.9372);
    double transit = 2451545.0 + mean_noon + 0.0053 * sin(m * RAD) - 0.0069 * sin(2 * ecliptic * RAD);
    double sin_dec = sin(ecliptic * RAD) * sin(23.4397 * RAD);
    double cos_dec = cos(asin(sin_dec));
    double cos_hour = (sin(-0.833 * RAD) - sin(lat * RAD) * sin_dec) / (cos(lat * RAD) * cos_dec);
    double hour_days;

    if (cos_hour < -1.0 || cos_hour > 1.0)
        return 0;
    hour_days = acos(cos_hour) / RAD / 360.0;
    *rise_utc_min = (transit - hour_days - jd0) * 1440.0;
    *set_utc_min = (transit + hour_days - jd0) * 1440.0;
    return 1;
}
