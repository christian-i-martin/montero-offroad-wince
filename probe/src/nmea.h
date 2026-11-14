/* Minimal NMEA 0183 reader for the head unit's GPS (COM1:, 38400 baud, SiRF).
 * Keeps speed, course, altitude, fix, satellite count, UTC time and date. The position is kept
 * in memory for on-screen calculations only (the dashboard's sunset time): never log it.
 * Portable C so it can be tested on the build machine. */
#ifndef NMEA_H
#define NMEA_H

typedef struct {
    int have_rmc;  int valid;          /* RMC status A = valid fix */
    int speed_dkmh;                    /* tenths of km/h */
    int course_ddeg;                   /* tenths of a degree, 0..3599 */
    int have_gga;  int fix_quality;  int satellites;
    int altitude_dm;                   /* tenths of a metre above mean sea level */
    int have_time; int utc_seconds;    /* RMC time of day, seconds after 00:00 UTC */
    int have_date; int year, month, day;  /* RMC date; old receivers may report it 1024 weeks early */
    int have_position; int lat_e6, lon_e6; /* RMC, micro-degrees, north and east positive */
    unsigned rmc_count;                /* RMC sentences parsed, so callers can tell fresh data */
    unsigned sentences, bad_checksums;
    char seen[96];                     /* sentence types seen, e.g. "GPRMC GPGGA PSRFTXT" */
} NmeaState;

/* Feed raw bytes from the port; complete lines are parsed as they arrive. */
void nmea_feed(NmeaState *state, char *line_buffer, int *line_length, int line_capacity,
               const char *data, int length);

/* Parse one sentence without the trailing CR/LF. Returns 1 if it updated the state. */
int nmea_parse_sentence(NmeaState *state, const char *sentence);

#endif
