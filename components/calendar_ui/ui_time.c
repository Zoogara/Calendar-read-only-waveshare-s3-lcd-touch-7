#include "calendar_ui_internal.h"
#include <string.h>

time_t ui_start_of_day(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    /* localtime_r() set tm_isdst for t's own instant - after zeroing the
     * time-of-day that's *usually* still right for the same calendar
     * date, but tm_add_days()/tm_add_months() below actually change the
     * date, where a stale isdst carried over from the original instant
     * can land mktime() on the wrong side of a DST transition (computes
     * the wrong UTC offset, off by exactly the DST delta) - see their
     * comments. Resetting here too, for the same reason and for
     * consistency with the rest of this file. */
    tm.tm_isdst = -1;
    return mktime(&tm);
}

time_t ui_add_days(time_t t, int days)
{
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_mday += days;
    /* Without this, mktime() reuses whatever DST flag localtime_r() set
     * for t's OWN date - wrong once `days` has moved onto the other side
     * of a DST transition, computing the resulting instant with the
     * wrong UTC offset (off by exactly the DST delta, 1 hour in AU).
     * Concretely observed: a month-view grid cell landing exactly on the
     * transition ended up 1 hour off, crossed back over local midnight,
     * and rendered as the PREVIOUS day's date number - two grid cells
     * showing the same day. -1 lets mktime() correctly re-derive DST
     * status for the actual resulting date instead of trusting a flag
     * computed for a different one. */
    tm.tm_isdst = -1;
    return mktime(&tm);
}

time_t ui_start_of_week(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    /* tm_wday: 0=Sunday..6=Saturday. Convert to Monday-based offset. */
    int mon_offset = (tm.tm_wday == 0) ? 6 : (tm.tm_wday - 1);
    return ui_add_days(ui_start_of_day(t), -mon_offset);
}

time_t ui_start_of_month(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_mday = 1;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    /* See ui_add_days()'s comment - the 1st of the month can be on the
     * other side of a DST transition from t's own date. */
    tm.tm_isdst = -1;
    return mktime(&tm);
}

time_t ui_add_months(time_t t, int delta)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int total = tm.tm_year * 12 + tm.tm_mon + delta;
    tm.tm_year = total / 12;
    tm.tm_mon = total % 12;
    if (tm.tm_mon < 0) { /* C's % can return negative */
        tm.tm_mon += 12;
        tm.tm_year -= 1;
    }
    if (tm.tm_mday > 28) {
        tm.tm_mday = 1; /* avoid rollover surprises (e.g. Jan 31 -> Mar 3) */
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    /* See ui_add_days()'s comment - a month (or several) away can easily
     * land on the other side of a DST transition from t's own date. */
    tm.tm_isdst = -1;
    return mktime(&tm);
}

bool ui_same_local_day(time_t a, time_t b)
{
    struct tm ta, tb;
    localtime_r(&a, &ta);
    localtime_r(&b, &tb);
    return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}
