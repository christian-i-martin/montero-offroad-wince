/* Decoder for CarInfo frames relayed by Main.exe (message 0x4A, Bundle key "data").
 *
 * Layouts and scaling come from SkCom.dll CarInfoApi::procCarInfoMsg and the factory
 * CarInfo.exe screen; see docs/vehicle-data-research.md. Portable C, no Windows headers,
 * so it can be unit-tested on the build machine.
 */
#ifndef CARINFO_H
#define CARINFO_H

typedef struct {
    int have_compass;       int compass;          /* 1..8 = N, NE, E, SE, S, SW, W, NW */
    int have_pressure;      int pressure_hpa;     /* whole hPa */
    int have_elevation;     int elevation_m;      /* signed metres */
    int have_outside;       int outside_c;        /* signed whole degrees */
    int have_battery;       int battery_dv;       /* tenths of a volt */
    int have_avg_speed;     int avg_speed_kmh;
    int have_avg_fuel;      int avg_fuel_dkmpl;   /* tenths of km/L */
    int have_range;         int range_km;
    int have_service_dist;  int service_dist;     /* unit not confirmed */
    int have_service_month; int service_month;
    int have_service_due;   int service_due;      /* 1 = service reminder active */
    int have_lights;        int lights;           /* frame 0x33 status bits; 0x04 = headlights (seen) */
    unsigned frames;                              /* frames decoded */
} CarInfoState;

/* Apply one frame ([command][second byte][payload...]) to the state.
 * Returns the number of fields updated, 0 for a known frame with nothing to show,
 * or -1 for an unknown command or a frame that is too short. */
int carinfo_apply(CarInfoState *state, const unsigned char *frame, int length);

/* "N", "NE", ... or "?" */
const char *carinfo_compass_name(int value);

/* One-line summary such as "Compass NE | 962 hPa | 428 m | ...". Returns chars written. */
int carinfo_format(const CarInfoState *state, char *out, int out_size);

#endif
