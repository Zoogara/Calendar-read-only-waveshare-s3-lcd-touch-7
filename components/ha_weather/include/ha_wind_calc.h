#pragma once
/*
 * Pure-C arithmetic for the wind graph (no ESP-IDF calls, so it is tested on a
 * computer): resamples held-value sensor histories onto a time grid and works
 * out an hourly vector-mean wind direction.
 */

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One state change: the value holds from time t until the next change. */
typedef struct {
    time_t t;
    float v;
} change_t;

/* The series' time-weighted mean in each of `count` (<= 128) buckets of `step`
 * seconds from `start`; NAN where the series has no value in a bucket. */
void wind_resample_mean(const change_t *ch, int n, time_t start, int step, int count, float *out);

/* The highest value in effect at any time in each bucket; NAN where none. */
void wind_resample_max(const change_t *ch, int n, time_t start, int step, int count, float *out);

typedef struct {
    bool ok;        /* the direction series had data this hour */
    bool calm;      /* mean speed below the calm limit - no meaningful direction */
    float dir;      /* degrees the wind comes FROM, 0 = north, clockwise */
    float speed;    /* mean speed over the hour */
} wind_hour_t;

/* For each of `hours` hours from `start`: the speed-weighted vector mean of the
 * direction series (degrees), pairing each direction reading with the speed
 * reading nearest it. Hours with a mean speed under `calm_below` are calm. */
void wind_hourly(const change_t *dir, int nd, const change_t *spd, int ns, time_t start, int hours,
                 float calm_below, wind_hour_t *out);

/* Speed-weighted vector mean of the hourly values (over however many hours the caller passes - the last four for the "Trending" text);
 * returns -1 if every hour is calm or empty. */
float wind_overall_dir(const wind_hour_t *h, int n);

/* "N", "NNE", ... for a bearing in degrees. */
const char *wind_compass16(float deg);

#ifdef __cplusplus
}
#endif
