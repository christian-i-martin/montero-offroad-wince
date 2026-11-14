#include "model.h"

#include <math.h>
#include <string.h>

#include "sun.h"

void series_init(Series *s, int cap)
{
    memset(s, 0, sizeof *s);
    s->cap = cap < 1 ? 1 : (cap > SERIES_MAX ? SERIES_MAX : cap);
}

void series_push(Series *s, float value, unsigned tick)
{
    int i;
    if (s->count < s->cap) {
        i = (s->head + s->count) % s->cap;
        s->count++;
    } else {
        i = s->head;
        s->head = (s->head + 1) % s->cap;
    }
    s->v[i] = value;
    s->t[i] = tick;
}

int series_get(const Series *s, int i, float *value, unsigned *tick)
{
    int k;
    if (i < 0 || i >= s->count)
        return 0;
    k = (s->head + i) % s->cap;
    if (value)
        *value = s->v[k];
    if (tick)
        *tick = s->t[k];
    return 1;
}

static const char *const g_points[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

const char *dash_point_name(float degrees)
{
    double d = fmod((double)degrees, 360.0);
    if (d < 0)
        d += 360.0;
    return g_points[(int)floor((d + 22.5) / 45.0) % 8];
}

void dash_init(DashModel *m)
{
    memset(m, 0, sizeof *m);
    series_init(&m->pressure, 120);   /* 120 x 30 s = 1 h */
    series_init(&m->outside, 120);    /* 120 x 90 s = 3 h */
    series_init(&m->battery, 120);    /* 120 x 15 s = 30 min */
    series_init(&m->elevation, ELEVATION_POINTS);
    series_init(&m->sealevel, 120);   /* 120 x 90 s = 3 h */
    series_init(&m->speed, 120);      /* 120 x 5 s = 10 min */
    m->map_zoom = MAP_DEFAULT_ZOOM;
    m->map_max_zoom = -1;
}

/* First sample at once, then one per interval. */
static int sample_due(const Series *s, unsigned now, unsigned *last, unsigned every)
{
    if (s->count == 0 || now - *last >= every) {
        *last = now;
        return 1;
    }
    return 0;
}

static float median3(const float *v)
{
    float a = v[0], b = v[1], c = v[2];
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return b;
}

static void update_speed(DashModel *m, const DashInputs *in, double dt, unsigned now)
{
    float speed = (float)in->gps.speed_dkmh / 10.0f;

    if (!m->gps_ok) {
        m->have_speed = 0;
        m->speed_count = 0;
        m->speed_sum = 0; /* the graph skips the time without GPS */
        m->speed_n = 0;
        return;
    }
    m->have_speed = 1;
    m->speed_kmh = speed < 2.0f ? 0.0f : speed; /* GPS speed wanders by 1 to 2 km/h when parked */
    m->speeds[m->speed_count % 3] = speed;
    m->speed_count++;
    if (m->speed_count >= 3) { /* a median of three ignores a single spike */
        float s = median3(m->speeds);
        if (s > m->top_kmh)
            m->top_kmh = s;
    }
    if (speed >= 3.0f) {
        double metres = speed / 3.6 * dt;
        m->moving_m += metres;
        m->moving_s += dt;
        m->elev_dist_m += metres;
    }
    m->speed_sum += m->speed_kmh;
    m->speed_n++;
    if (m->speed.count == 0 || now - m->speed_ms >= SPEED_EVERY_MS) {
        series_push(&m->speed, m->speed_sum / (float)m->speed_n, now);
        m->speed_ms = now;
        m->speed_sum = 0;
        m->speed_n = 0;
    }
}

static void update_heading(DashModel *m, const DashInputs *in)
{
    float speed = (float)in->gps.speed_dkmh / 10.0f;
    int c = in->car.compass;

    if (!m->gps_ok)
        m->gps_heading = 0;
    else if (speed >= 6.0f)
        m->gps_heading = 1;
    else if (speed < 4.0f)
        m->gps_heading = 0;

    if (m->gps_heading) {
        m->heading_src = SRC_GPS;
        m->heading_deg = (float)in->gps.course_ddeg / 10.0f;
    } else if (m->car_ok && in->car.have_compass && c >= 1 && c <= 8) {
        m->heading_src = SRC_CAR;
        m->heading_deg = (float)(c - 1) * 45.0f;
        m->compass_point = c;
    } else {
        m->heading_src = SRC_NONE;
    }
}

static void update_elevation(DashModel *m, const DashInputs *in, unsigned now)
{
    int gps_alt = m->gps_ok && in->gps.have_gga && in->gps.fix_quality > 0;
    int car_alt = m->car_ok && in->car.have_elevation;
    int i;

    if (gps_alt) {
        if (now - m->alt_last_ms > 5000)
            m->alt_count = 0; /* a gap: start the average again */
        m->alt[m->alt_next] = (float)in->gps.altitude_dm / 10.0f;
        m->alt_next = (m->alt_next + 1) % 10;
        if (m->alt_count < 10)
            m->alt_count++;
        m->alt_last_ms = now;
    }
    if (gps_alt && m->alt_count >= 3) {
        float sum = 0;
        for (i = 0; i < m->alt_count; i++)
            sum += m->alt[(m->alt_next - 1 - i + 10) % 10];
        m->elev_src = SRC_GPS;
        m->elev_m = sum / (float)m->alt_count;
        if (car_alt) { /* remember how far the car's sensor is off, for when GPS drops out */
            float diff = m->elev_m - (float)in->car.elevation_m;
            if (!m->have_offset) {
                m->car_offset = diff;
                m->have_offset = 1;
            } else {
                m->car_offset += (diff - m->car_offset) * 0.02f;
            }
        }
    } else if (car_alt) {
        m->elev_src = SRC_CAR;
        m->elev_m = (float)in->car.elevation_m + (m->have_offset ? m->car_offset : 0.0f);
    } else {
        m->elev_src = SRC_NONE;
    }

    if (m->elev_src != SRC_NONE) {
        if (m->elevation.count == 0) {
            series_push(&m->elevation, m->elev_m, now);
            m->elev_dist_m = 0;
        } else if (m->elev_dist_m >= ELEVATION_EVERY_M) {
            series_push(&m->elevation, m->elev_m, now);
            m->elev_dist_m -= ELEVATION_EVERY_M;
            if (m->elev_dist_m >= ELEVATION_EVERY_M)
                m->elev_dist_m = 0;
        }
    }
}

/* Climb rate: elevation now against about a minute ago, from samples every 5 s. */
static void update_climb(DashModel *m, unsigned now)
{
    int i, best = -1;
    unsigned best_age = 0;

    if (m->elev_src == SRC_NONE || m->elev_src != m->climb_src) {
        m->climb_count = 0;
        m->climb_next = 0;
        m->climb_src = m->elev_src;
        m->have_climb = 0;
        if (m->elev_src == SRC_NONE)
            return;
    }
    if (m->climb_count == 0 || now - m->climb_last_ms >= 5000) {
        m->climb_v[m->climb_next] = m->elev_m;
        m->climb_t[m->climb_next] = now;
        m->climb_next = (m->climb_next + 1) % 24;
        if (m->climb_count < 24)
            m->climb_count++;
        m->climb_last_ms = now;
    }
    for (i = 0; i < m->climb_count; i++) {
        unsigned age = now - m->climb_t[i];
        if (age >= 45000 && (best < 0 || (age > 60000 ? age - 60000 : 60000 - age) <
                                             (best_age > 60000 ? best_age - 60000 : 60000 - best_age))) {
            best = i;
            best_age = age;
        }
    }
    m->have_climb = best >= 0;
    if (best >= 0)
        m->climb_m_min = (m->elev_m - m->climb_v[best]) / ((float)best_age / 60000.0f);
}

static void update_car(DashModel *m, const DashInputs *in, unsigned now)
{
    float first, last, value;
    unsigned t0, t1;
    int dv = in->car.battery_dv;

    m->have_pressure = m->car_ok && in->car.have_pressure;
    if (m->have_pressure) {
        m->pressure_hpa = in->car.pressure_hpa;
        if (sample_due(&m->pressure, now, &m->pressure_ms, PRESSURE_EVERY_MS))
            series_push(&m->pressure, (float)m->pressure_hpa, now);
        if (m->elev_src == SRC_GPS && sample_due(&m->sealevel, now, &m->sealevel_ms, SEALEVEL_EVERY_MS)) {
            double f = 1.0 - 2.25577e-5 * m->elev_m;
            if (f > 0.1)
                series_push(&m->sealevel, (float)(m->pressure_hpa / pow(f, 5.25588)), now);
        }
    }
    /* Weather trend: sea-level pressure (needs GPS altitude) over at least an hour, scaled to 3 h. */
    m->weather = WEATHER_UNKNOWN;
    if (m->sealevel.count >= 6 && series_get(&m->sealevel, 0, NULL, &t0) &&
        series_get(&m->sealevel, m->sealevel.count - 1, NULL, &t1) && t1 - t0 >= 3600000u) {
        int i;
        double delta;
        first = last = 0;
        for (i = 0; i < 3; i++) {
            series_get(&m->sealevel, i, &value, NULL);
            first += value / 3.0f;
            series_get(&m->sealevel, m->sealevel.count - 1 - i, &value, NULL);
            last += value / 3.0f;
        }
        delta = (last - first) * 10800000.0 / (double)(t1 - t0);
        m->weather = delta <= -3.0 ? WEATHER_FALLING_FAST
                   : delta <= -1.0 ? WEATHER_FALLING
                   : delta >= 1.0 ? WEATHER_RISING : WEATHER_STEADY;
    }

    m->have_outside = m->car_ok && in->car.have_outside;
    if (m->have_outside) {
        m->outside_c = in->car.outside_c;
        if (sample_due(&m->outside, now, &m->outside_ms, OUTSIDE_EVERY_MS))
            series_push(&m->outside, (float)m->outside_c, now);
    }
    m->have_temp_change = 0;
    if (m->outside.count >= 2 && series_get(&m->outside, 0, &first, &t0) &&
        series_get(&m->outside, m->outside.count - 1, &last, &t1) && t1 - t0 >= 1800000u) {
        m->have_temp_change = 1;
        m->temp_change = last - first;
        m->temp_span_s = (t1 - t0) / 1000u;
    }

    m->have_battery = m->car_ok && in->car.have_battery;
    if (m->have_battery) {
        m->battery_v = (float)dv / 10.0f;
        m->battery_state = dv >= 148 ? BATT_HIGH : dv >= 132 ? BATT_CHARGING : dv >= 122 ? BATT_NOT_CHARGING : BATT_LOW;
        if (sample_due(&m->battery, now, &m->battery_ms, BATTERY_EVERY_MS))
            series_push(&m->battery, m->battery_v, now);
    } else {
        m->battery_state = BATT_UNKNOWN;
    }
}

static int round_minutes(double minutes)
{
    int v = (int)floor(minutes + 0.5) % 1440;
    return v < 0 ? v + 1440 : v;
}

/* Local time. GPS time (UTC, exact) once a fix has been seen, kept running with the tick count, so
 * the head unit's clock (it resets to 2012 on this car) is used only when it looks set. Time zone:
 * the menu choice, else the unit's clock against GPS time when that clock looks set, else the
 * unit's time-zone setting. */
static void update_clock(DashModel *m, const DashInputs *in, unsigned now)
{
    double unit_s = 0, utc_now = 0, local;
    long days;
    int y, mo, d, guard;

    if (m->gps_ok && in->gps.have_time && in->gps.have_date) {
        days = sun_days_from_civil(in->gps.year, in->gps.month, in->gps.day);
        y = in->gps.year;
        for (guard = 0; y < 2020 && guard < 4; guard++) { /* receivers from before a 1024-week rollover */
            days += 7168;
            sun_civil_from_days(days, &y, &mo, &d);
        }
        m->utc_base = (double)days * 86400.0 + in->gps.utc_seconds;
        m->utc_base_ms = now;
        m->have_utc = 1;
    }
    if (m->have_utc)
        utc_now = m->utc_base + (double)(now - m->utc_base_ms) / 1000.0;
    if (in->clock_valid)
        unit_s = (double)sun_days_from_civil(in->year, in->month, in->day) * 86400.0 + in->hour * 3600.0 +
                 in->minute * 60.0 + in->second;

    if (in->tz_override)
        m->utc_offset_min = in->tz_override_min;
    else if (in->clock_valid && m->have_utc && fabs(unit_s - utc_now) <= 14 * 3600.0)
        m->utc_offset_min = (int)floor((unit_s - utc_now) / 900.0 + 0.5) * 15;
    else
        m->utc_offset_min = in->system_utc_offset_min;

    if (m->have_utc) {
        local = utc_now + m->utc_offset_min * 60.0;
        m->clock_src = SRC_GPS;
    } else if (in->clock_valid) {
        local = unit_s;
        m->clock_src = SRC_CAR;
    } else {
        m->clock_valid = 0;
        m->clock_src = SRC_NONE;
        return;
    }
    days = (long)floor(local / 86400.0);
    sun_civil_from_days(days, &m->year, &m->month, &m->day);
    local -= (double)days * 86400.0;
    m->hour = (int)(local / 3600.0);
    m->minute = (int)(local / 60.0) % 60;
    m->second = (int)local % 60;
    m->clock_valid = 1;
}

static void update_sun(DashModel *m, unsigned now)
{
    double rise, set, now_min;
    int offset = m->utc_offset_min, event, y, mo, d;

    if (!m->have_position || !m->clock_valid || (m->sun_valid && now - m->sun_ms < 10000))
        return;
    m->sun_ms = now;
    m->sun_valid = 1;
    now_min = m->hour * 60 + m->minute + m->second / 60.0;
    m->sun_event = SUN_NONE;
    if (!sun_times(m->year, m->month, m->day, m->lat, m->lon, &rise, &set))
        return;
    rise += offset;
    set += offset;
    if (now_min < rise) {
        event = round_minutes(rise);
        m->sun_event = SUN_SUNRISE;
    } else if (now_min < set) {
        event = round_minutes(set);
        m->sun_event = SUN_SUNSET;
        m->sun_minutes_left = (int)floor(set - now_min + 0.5);
    } else { /* after sunset: tomorrow's sunrise */
        sun_civil_from_days(sun_days_from_civil(m->year, m->month, m->day) + 1, &y, &mo, &d);
        if (!sun_times(y, mo, d, m->lat, m->lon, &rise, &set))
            return;
        event = round_minutes(rise + offset);
        m->sun_event = SUN_SUNRISE;
    }
    m->sun_hour = event / 60;
    m->sun_minute = event % 60;
}

void dash_update(DashModel *m, const DashInputs *in)
{
    unsigned now = in->now_ms;
    double dt = m->started ? (double)(now - m->last_ms) / 1000.0 : 0.0;

    if (dt > 5.0)
        dt = 0.0; /* a long pause (window hidden, clock jump): do not count it as driving */
    m->now_ms = now;
    if (!m->started)
        m->start_ms = now;
    m->gps_ok = in->gps_fresh && in->gps.have_rmc && in->gps.valid;
    update_clock(m, in, now);
    if (m->clock_valid && !m->have_since) { /* the start time, even when the clock came later */
        long s = (long)m->hour * 3600 + m->minute * 60 + m->second - (long)((now - m->start_ms) / 1000u);
        s = (s % 86400 + 86400) % 86400;
        m->have_since = 1;
        m->since_hour = (int)(s / 3600);
        m->since_minute = (int)(s / 60 % 60);
    }
    m->satellites = in->gps_fresh ? in->gps.satellites : 0;
    m->car_ok = in->car_fresh;
    m->night = in->lights_on;
    if (m->gps_ok && in->gps.have_position) {
        m->have_position = 1;
        m->lat = in->gps.lat_e6 / 1e6;
        m->lon = in->gps.lon_e6 / 1e6;
    }

    update_speed(m, in, dt, now);
    m->have_avg = m->moving_s >= 60.0;
    if (m->have_avg)
        m->avg_kmh = (float)(m->moving_m / m->moving_s * 3.6);
    m->have_top = m->top_kmh >= 3.0f;
    update_heading(m, in);
    update_elevation(m, in, now);
    update_climb(m, now);
    update_car(m, in, now);
    update_sun(m, now);

    m->last_ms = now;
    m->started = 1;
}
