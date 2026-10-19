/* ha_wind_calc.c: resampling of held-value histories and the hourly vector-mean wind direction. */
#include <math.h>

#include "ha_wind_calc.h"
#include "test.h"

#define H 3600

static void mean_two_values_in_a_bucket(void)
{
    change_t ch[] = {{0, 10}, {1800, 20}};
    float out[2];
    wind_resample_mean(ch, 2, 0, H, 2, out);
    CHECK_NEAR(out[0], 15.0, 1e-4);     /* half an hour at 10, half at 20 */
    CHECK_NEAR(out[1], 20.0, 1e-4);     /* the last reading holds on */
}

static void mean_ignores_time_before_the_series(void)
{
    change_t ch[] = {{1800, 20}};
    float out[2];
    wind_resample_mean(ch, 1, 0, H, 2, out);
    CHECK_NEAR(out[0], 20.0, 1e-4);     /* not diluted by the half hour with no data */
    CHECK_NEAR(out[1], 20.0, 1e-4);
}

static void mean_uses_a_reading_from_before_the_window(void)
{
    change_t ch[] = {{-7200, 5}, {H, 9}};
    float out[2];
    wind_resample_mean(ch, 2, 0, H, 2, out);
    CHECK_NEAR(out[0], 5.0, 1e-4);
    CHECK_NEAR(out[1], 9.0, 1e-4);
}

static void mean_without_data_is_nan(void)
{
    float out[3] = {1, 1, 1};
    wind_resample_mean(NULL, 0, 0, H, 3, out);
    CHECK(isnan(out[0]) && isnan(out[1]) && isnan(out[2]));

    change_t late[] = {{3 * H + 10, 4}};            /* only after the window */
    wind_resample_mean(late, 1, 0, H, 3, out);
    CHECK(isnan(out[0]) && isnan(out[2]));
}

static void mean_stops_at_128_buckets(void)
{
    change_t ch[] = {{0, 7}};
    float out[130];
    for (int i = 0; i < 130; i++) {
        out[i] = -1;
    }
    wind_resample_mean(ch, 1, 0, 60, 200, out);
    CHECK_NEAR(out[127], 7.0, 1e-4);
    CHECK_NEAR(out[128], -1.0, 1e-4);   /* untouched */
}

static void max_per_bucket(void)
{
    change_t ch[] = {{0, 3}, {1000, 8}, {2000, 4}};
    float out[2];
    wind_resample_max(ch, 3, 0, H, 2, out);
    CHECK_NEAR(out[0], 8.0, 1e-4);
    CHECK_NEAR(out[1], 4.0, 1e-4);      /* still in effect into the next bucket */

    change_t late[] = {{5000, 9}};
    wind_resample_max(late, 1, 0, H, 2, out);
    CHECK(isnan(out[0]));
    CHECK_NEAR(out[1], 9.0, 1e-4);
}

static float ang_diff(float a, float b)
{
    float d = fabsf(a - b);
    return d > 180 ? 360 - d : d;
}

static void hourly_steady_wind(void)
{
    change_t dir[] = {{0, 90}};
    change_t spd[] = {{0, 10}};
    wind_hour_t out[2];
    wind_hourly(dir, 1, spd, 1, 0, 2, 2.0f, out);
    for (int h = 0; h < 2; h++) {
        CHECK(out[h].ok);
        CHECK(!out[h].calm);
        CHECK_NEAR(out[h].dir, 90.0, 0.01);
        CHECK_NEAR(out[h].speed, 10.0, 1e-4);
    }
}

static void hourly_calm_and_missing(void)
{
    change_t dir[] = {{0, 90}};
    change_t slow[] = {{0, 1}};
    wind_hour_t out[1];
    wind_hourly(dir, 1, slow, 1, 0, 1, 2.0f, out);
    CHECK(out[0].ok && out[0].calm);                 /* 1 km/h is under the 2 km/h limit */

    wind_hourly(dir, 1, NULL, 0, 0, 1, 2.0f, out);   /* no speed readings at all */
    CHECK(out[0].ok && out[0].calm);
    CHECK_NEAR(out[0].speed, 0.0, 1e-6);             /* unknown speed counts as none */

    change_t later[] = {{H, 90}};                    /* direction only from hour 1 */
    change_t spd[] = {{0, 10}};
    wind_hour_t two[2];
    wind_hourly(later, 1, spd, 1, 0, 2, 2.0f, two);
    CHECK(!two[0].ok && two[0].calm);
    CHECK(two[1].ok && !two[1].calm);
}

static void hourly_vector_mean(void)
{
    wind_hour_t out[1];

    /* opposite winds of equal strength cancel: no meaningful direction */
    change_t d1[] = {{0, 0}, {1800, 180}};
    change_t s1[] = {{0, 10}, {1800, 10}};
    wind_hourly(d1, 2, s1, 2, 0, 1, 2.0f, out);
    CHECK(out[0].calm);

    /* 350 and 10 degrees average to north, not to 180 */
    change_t d2[] = {{0, 350}, {1800, 10}};
    wind_hourly(d2, 2, s1, 2, 0, 1, 2.0f, out);
    CHECK(!out[0].calm);
    CHECK_NEAR(ang_diff(out[0].dir, 0), 0.0, 0.01);

    /* a strong north wind and a weak east one: weighted by speed */
    change_t d3[] = {{0, 0}, {1800, 90}};
    change_t s3[] = {{0, 30}, {1800, 10}};
    wind_hourly(d3, 2, s3, 2, 0, 1, 2.0f, out);
    CHECK_NEAR(out[0].dir, 18.435, 0.01);           /* atan2(10, 30) */
    CHECK_NEAR(out[0].speed, 20.0, 1e-3);
}

static void overall_direction(void)
{
    wind_hour_t h[3] = {
        {true, false, 90, 10},
        {true, true, 270, 50},        /* calm: ignored whatever it says */
        {true, false, 90, 10},
    };
    CHECK_NEAR(wind_overall_dir(h, 3), 90.0, 0.01);

    wind_hour_t weighted[2] = {{true, false, 0, 30}, {true, false, 90, 10}};
    CHECK_NEAR(wind_overall_dir(weighted, 2), 18.435, 0.01);

    wind_hour_t none[2] = {{true, true, 0, 0}, {false, false, 0, 0}};
    CHECK_NEAR(wind_overall_dir(none, 2), -1.0, 1e-6);
}

static void compass_points(void)
{
    CHECK_STR(wind_compass16(0), "N");
    CHECK_STR(wind_compass16(11.2f), "N");
    CHECK_STR(wind_compass16(11.3f), "NNE");
    CHECK_STR(wind_compass16(45), "NE");
    CHECK_STR(wind_compass16(90), "E");
    CHECK_STR(wind_compass16(180), "S");
    CHECK_STR(wind_compass16(270), "W");
    CHECK_STR(wind_compass16(337.5f), "NNW");
    CHECK_STR(wind_compass16(348.7f), "NNW");
    CHECK_STR(wind_compass16(348.8f), "N");
    CHECK_STR(wind_compass16(359.9f), "N");
    CHECK_STR(wind_compass16(-10), "N");
    CHECK_STR(wind_compass16(370), "N");
}

void test_wind(void)
{
    RUN(mean_two_values_in_a_bucket);
    RUN(mean_ignores_time_before_the_series);
    RUN(mean_uses_a_reading_from_before_the_window);
    RUN(mean_without_data_is_nan);
    RUN(mean_stops_at_128_buckets);
    RUN(max_per_bucket);
    RUN(hourly_steady_wind);
    RUN(hourly_calm_and_missing);
    RUN(hourly_vector_mean);
    RUN(overall_direction);
    RUN(compass_points);
}
