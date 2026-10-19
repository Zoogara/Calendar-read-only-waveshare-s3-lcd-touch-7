#include "ha_wind_calc.h"

#include <math.h>
#include <stddef.h>

#define MAX_BUCKETS 128

static time_t tmin_t(time_t a, time_t b) { return a < b ? a : b; }
static time_t tmax_t(time_t a, time_t b) { return a > b ? a : b; }

void wind_resample_mean(const change_t *ch, int n, time_t start, int step, int count, float *out)
{
    double sum[MAX_BUCKETS] = {0}, w[MAX_BUCKETS] = {0};
    if (count > MAX_BUCKETS) {
        count = MAX_BUCKETS;
    }
    time_t end = start + (time_t)step * count;
    for (int i = 0; i < n; i++) {
        time_t a = ch[i].t, b = (i + 1 < n) ? ch[i + 1].t : end;
        if (b <= start || a >= end) {
            continue;
        }
        a = tmax_t(a, start);
        b = tmin_t(b, end);
        int b0 = (int)((a - start) / step), b1 = (int)((b - 1 - start) / step);
        for (int k = b0; k <= b1 && k < count; k++) {
            time_t bs = start + (time_t)k * step, be = bs + step;
            double ov = (double)(tmin_t(b, be) - tmax_t(a, bs));
            if (ov > 0) {
                sum[k] += (double)ch[i].v * ov;
                w[k] += ov;
            }
        }
    }
    for (int k = 0; k < count; k++) {
        out[k] = w[k] > 0 ? (float)(sum[k] / w[k]) : NAN;
    }
}

void wind_resample_max(const change_t *ch, int n, time_t start, int step, int count, float *out)
{
    if (count > MAX_BUCKETS) {
        count = MAX_BUCKETS;
    }
    for (int k = 0; k < count; k++) {
        out[k] = NAN;
    }
    time_t end = start + (time_t)step * count;
    for (int i = 0; i < n; i++) {
        time_t a = ch[i].t, b = (i + 1 < n) ? ch[i + 1].t : end;
        if (b <= start || a >= end) {
            continue;
        }
        a = tmax_t(a, start);
        b = tmin_t(b, end);
        int b0 = (int)((a - start) / step), b1 = (int)((b - 1 - start) / step);
        for (int k = b0; k <= b1 && k < count; k++) {
            if (isnan(out[k]) || ch[i].v > out[k]) {
                out[k] = ch[i].v;
            }
        }
    }
}

void wind_hourly(const change_t *dir, int nd, const change_t *spd, int ns, time_t start, int hours,
                 float calm_below, wind_hour_t *out)
{
    double ex[MAX_BUCKETS] = {0}, ey[MAX_BUCKETS] = {0}, ws[MAX_BUCKETS] = {0}, w[MAX_BUCKETS] = {0};
    if (hours > MAX_BUCKETS) {
        hours = MAX_BUCKETS;
    }
    time_t end = start + 3600L * hours;
    int si = 0;                              /* speed reading nearest the current direction reading */
    for (int i = 0; i < nd; i++) {
        time_t a = dir[i].t, b = (i + 1 < nd) ? dir[i + 1].t : end;
        if (b <= start || a >= end) {
            continue;
        }
        /* The speed reading logged with this direction (within 10 s after it),
         * else the latest one before it. */
        while (si + 1 < ns && spd[si + 1].t <= a + 10) {
            si++;
        }
        float s = (ns > 0 && spd[si].t <= a + 10) ? spd[si].v : NAN;
        a = tmax_t(a, start);
        b = tmin_t(b, end);
        double th = (double)dir[i].v * M_PI / 180.0;
        for (int h = (int)((a - start) / 3600); h <= (int)((b - 1 - start) / 3600) && h < hours; h++) {
            time_t hs = start + 3600L * h, he = hs + 3600;
            double ov = (double)(tmin_t(b, he) - tmax_t(a, hs));
            if (ov <= 0) {
                continue;
            }
            w[h] += ov;
            if (!isnan(s)) {
                ws[h] += (double)s * ov;
                ex[h] += (double)s * ov * sin(th);
                ey[h] += (double)s * ov * cos(th);
            }
        }
    }
    for (int h = 0; h < hours; h++) {
        out[h].ok = w[h] > 0;
        out[h].speed = w[h] > 0 ? (float)(ws[h] / w[h]) : NAN;
        out[h].calm = !out[h].ok || isnan(out[h].speed) || out[h].speed < calm_below ||
                      (fabs(ex[h]) + fabs(ey[h])) < 1e-9;
        double d = atan2(ex[h], ey[h]) * 180.0 / M_PI;
        out[h].dir = (float)(d < 0 ? d + 360.0 : d);
    }
}

float wind_overall_dir(const wind_hour_t *h, int n)
{
    double ex = 0, ey = 0;
    for (int i = 0; i < n; i++) {
        if (!h[i].ok || h[i].calm) {
            continue;
        }
        double th = (double)h[i].dir * M_PI / 180.0;
        ex += (double)h[i].speed * sin(th);
        ey += (double)h[i].speed * cos(th);
    }
    if (fabs(ex) + fabs(ey) < 1e-9) {
        return -1.0f;
    }
    double d = atan2(ex, ey) * 180.0 / M_PI;
    return (float)(d < 0 ? d + 360.0 : d);
}

const char *wind_compass16(float deg)
{
    static const char *N16[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    float d = fmodf(deg, 360.0f);
    if (d < 0) {
        d += 360.0f;
    }
    return N16[((int)floorf((d + 11.25f) / 22.5f)) % 16];
}
