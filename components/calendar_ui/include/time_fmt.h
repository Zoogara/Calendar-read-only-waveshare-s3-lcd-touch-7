#pragma once
/*
 * Time-of-day text in the user's chosen clock style (config page -> Time format): 24-hour
 * ("14:35") or 12-hour with AM/PM ("2:35 PM"). Pure C with no LVGL or ESP-IDF calls, so it is
 * unit-tested on a computer (tests/host). Noon is "12:00 PM" and midnight "12:00 AM".
 *
 * Every function NUL-terminates `out` (truncating if it doesn't fit) and treats `hour` modulo 24,
 * except tfmt_hour(), which accepts 24 as "end of the day".
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A time with minutes: "14:35" or "2:35 PM". */
void tfmt_hm(bool h24, int hour, int min, char *out, size_t sz);

/* A whole hour, as on a timeline: "14:00" or "2 PM". 24 is midnight at the end of the day:
 * "24:00" or "12 AM". */
void tfmt_hour(bool h24, int hour, char *out, size_t sz);

/* A span: "12:30 - 13:00" or "12:30 PM - 1:00 PM" (with `sep` between, e.g. " - "). */
void tfmt_range(bool h24, int sh, int sm, int eh, int em, const char *sep, char *out, size_t sz);

/* "AM" or "PM" for an hour in 12-hour style; "" in 24-hour style. */
const char *tfmt_ampm(bool h24, int hour);

/* The hour's own digits on a clock face: "14" (24-hour, two digits) or "2" (12-hour, one or two
 * digits, no leading zero, 12 for midnight and noon). */
void tfmt_clock_hour(bool h24, int hour, char *out, size_t sz);

/* A length of time, for uptime: "42 s", "5 min", "3 h 05 min", "2 d 4 h 12 min" (once there is a
 * day, hours and minutes always follow it). */
void tfmt_duration(unsigned long secs, char *out, size_t sz);

#ifdef __cplusplus
}
#endif
