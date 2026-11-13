/* time_fmt.c: 12-hour (AM/PM) and 24-hour time text. */
#include <string.h>

#include "test.h"
#include "time_fmt.h"

static char out[32];

static void hm_24h(void)
{
    tfmt_hm(true, 0, 0, out, sizeof(out));   CHECK_STR(out, "00:00");
    tfmt_hm(true, 9, 5, out, sizeof(out));   CHECK_STR(out, "09:05");
    tfmt_hm(true, 12, 0, out, sizeof(out));  CHECK_STR(out, "12:00");
    tfmt_hm(true, 14, 35, out, sizeof(out)); CHECK_STR(out, "14:35");
    tfmt_hm(true, 23, 59, out, sizeof(out)); CHECK_STR(out, "23:59");
}

static void hm_12h(void)
{
    tfmt_hm(false, 0, 0, out, sizeof(out));   CHECK_STR(out, "12:00 AM");    /* midnight */
    tfmt_hm(false, 0, 30, out, sizeof(out));  CHECK_STR(out, "12:30 AM");
    tfmt_hm(false, 1, 5, out, sizeof(out));   CHECK_STR(out, "1:05 AM");
    tfmt_hm(false, 11, 59, out, sizeof(out)); CHECK_STR(out, "11:59 AM");
    tfmt_hm(false, 12, 0, out, sizeof(out));  CHECK_STR(out, "12:00 PM");    /* noon */
    tfmt_hm(false, 12, 45, out, sizeof(out)); CHECK_STR(out, "12:45 PM");
    tfmt_hm(false, 13, 5, out, sizeof(out));  CHECK_STR(out, "1:05 PM");
    tfmt_hm(false, 14, 35, out, sizeof(out)); CHECK_STR(out, "2:35 PM");
    tfmt_hm(false, 23, 59, out, sizeof(out)); CHECK_STR(out, "11:59 PM");
}

static void hour_labels(void)
{
    tfmt_hour(true, 9, out, sizeof(out));   CHECK_STR(out, "9:00");
    tfmt_hour(true, 13, out, sizeof(out));  CHECK_STR(out, "13:00");
    tfmt_hour(true, 24, out, sizeof(out));  CHECK_STR(out, "24:00");
    tfmt_hour(false, 0, out, sizeof(out));  CHECK_STR(out, "12 AM");
    tfmt_hour(false, 4, out, sizeof(out));  CHECK_STR(out, "4 AM");
    tfmt_hour(false, 12, out, sizeof(out)); CHECK_STR(out, "12 PM");
    tfmt_hour(false, 17, out, sizeof(out)); CHECK_STR(out, "5 PM");
    tfmt_hour(false, 24, out, sizeof(out)); CHECK_STR(out, "12 AM");              /* end of the day */
}

static void ranges(void)
{
    tfmt_range(true, 12, 30, 13, 0, " - ", out, sizeof(out));  CHECK_STR(out, "12:30 - 13:00");
    tfmt_range(false, 12, 30, 13, 0, " - ", out, sizeof(out)); CHECK_STR(out, "12:30 PM - 1:00 PM");
    tfmt_range(false, 9, 0, 10, 30, "-", out, sizeof(out));    CHECK_STR(out, "9:00 AM-10:30 AM");
    tfmt_range(false, 11, 30, 12, 15, " - ", out, sizeof(out)); CHECK_STR(out, "11:30 AM - 12:15 PM");   /* crosses noon */
    tfmt_range(false, 23, 0, 0, 30, " - ", out, sizeof(out));  CHECK_STR(out, "11:00 PM - 12:30 AM");   /* crosses midnight */
}

static void ampm_and_clock_hour(void)
{
    CHECK_STR(tfmt_ampm(true, 15), "");
    CHECK_STR(tfmt_ampm(false, 0), "AM");
    CHECK_STR(tfmt_ampm(false, 11), "AM");
    CHECK_STR(tfmt_ampm(false, 12), "PM");
    CHECK_STR(tfmt_ampm(false, 23), "PM");

    tfmt_clock_hour(true, 7, out, sizeof(out));   CHECK_STR(out, "07");
    tfmt_clock_hour(true, 14, out, sizeof(out));  CHECK_STR(out, "14");
    tfmt_clock_hour(false, 0, out, sizeof(out));  CHECK_STR(out, "12");
    tfmt_clock_hour(false, 7, out, sizeof(out));  CHECK_STR(out, "7");      /* no leading zero */
    tfmt_clock_hour(false, 12, out, sizeof(out)); CHECK_STR(out, "12");
    tfmt_clock_hour(false, 14, out, sizeof(out)); CHECK_STR(out, "2");
    tfmt_clock_hour(false, 22, out, sizeof(out)); CHECK_STR(out, "10");
}

static void every_hour_and_minute_is_sane(void)
{
    /* the longest possible text, so the callers' buffers can be sized: "12:59 PM" is 8 characters */
    size_t longest = 0;
    for (int h = 0; h < 24; h++) {
        for (int m = 0; m < 60; m++) {
            tfmt_hm(false, h, m, out, sizeof(out));
            if (strlen(out) > longest) {
                longest = strlen(out);
            }
            CHECK(strstr(out, "AM") != NULL || strstr(out, "PM") != NULL);
            tfmt_hm(true, h, m, out, sizeof(out));
            CHECK_INT(strlen(out), 5);
        }
    }
    CHECK_INT(longest, 8);
}

static void truncates_safely(void)
{
    char tiny[6];
    memset(tiny, 'x', sizeof(tiny));
    tfmt_hm(false, 12, 30, tiny, sizeof(tiny));          /* "12:30 PM" does not fit */
    CHECK_INT(strlen(tiny), 5);                          /* cut, but terminated within the buffer */
    char one[1] = {'x'};
    tfmt_hm(true, 1, 2, one, sizeof(one));
    CHECK_INT(one[0], 0);
}

static void out_of_range_hours_wrap(void)
{
    tfmt_hm(true, 25, 0, out, sizeof(out));   CHECK_STR(out, "01:00");
    tfmt_hm(false, -1, 0, out, sizeof(out));  CHECK_STR(out, "11:00 PM");
}

static void durations(void)
{
    tfmt_duration(0, out, sizeof(out));         CHECK_STR(out, "0 s");
    tfmt_duration(42, out, sizeof(out));        CHECK_STR(out, "42 s");
    tfmt_duration(59, out, sizeof(out));        CHECK_STR(out, "59 s");
    tfmt_duration(60, out, sizeof(out));        CHECK_STR(out, "1 min");
    tfmt_duration(299, out, sizeof(out));       CHECK_STR(out, "4 min");
    tfmt_duration(3600, out, sizeof(out));      CHECK_STR(out, "1 h 00 min");
    tfmt_duration(3 * 3600 + 5 * 60 + 9, out, sizeof(out));  CHECK_STR(out, "3 h 05 min");
    tfmt_duration(86399, out, sizeof(out));     CHECK_STR(out, "23 h 59 min");
    tfmt_duration(86400, out, sizeof(out));     CHECK_STR(out, "1 d 0 h 00 min");
    tfmt_duration(2 * 86400 + 4 * 3600 + 12 * 60, out, sizeof(out));  CHECK_STR(out, "2 d 4 h 12 min");
    tfmt_duration(400UL * 86400, out, sizeof(out));  CHECK_STR(out, "400 d 0 h 00 min");
}

void test_timefmt(void)
{
    RUN(durations);
    RUN(hm_24h);
    RUN(hm_12h);
    RUN(hour_labels);
    RUN(ranges);
    RUN(ampm_and_clock_hour);
    RUN(every_hour_and_minute_is_sane);
    RUN(truncates_safely);
    RUN(out_of_range_hours_wrap);
}
