#include "time_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int norm(int hour)
{
    hour %= 24;
    return hour < 0 ? hour + 24 : hour;
}

static int to12(int hour)
{
    int h = norm(hour) % 12;
    return h == 0 ? 12 : h;
}

const char *tfmt_ampm(bool h24, int hour)
{
    if (h24) {
        return "";
    }
    return norm(hour) < 12 ? "AM" : "PM";
}

void tfmt_hm(bool h24, int hour, int min, char *out, size_t sz)
{
    if (h24) {
        snprintf(out, sz, "%02d:%02d", norm(hour), min);
    } else {
        snprintf(out, sz, "%d:%02d %s", to12(hour), min, tfmt_ampm(false, hour));
    }
}

void tfmt_hour(bool h24, int hour, char *out, size_t sz)
{
    if (hour == 24) {
        snprintf(out, sz, h24 ? "24:00" : "12 AM");
    } else if (h24) {
        snprintf(out, sz, "%d:00", norm(hour));
    } else {
        snprintf(out, sz, "%d %s", to12(hour), tfmt_ampm(false, hour));
    }
}

void tfmt_range(bool h24, int sh, int sm, int eh, int em, const char *sep, char *out, size_t sz)
{
    char a[16], b[16];
    tfmt_hm(h24, sh, sm, a, sizeof(a));
    tfmt_hm(h24, eh, em, b, sizeof(b));
    snprintf(out, sz, "%s%s%s", a, sep, b);
}

void tfmt_clock_hour(bool h24, int hour, char *out, size_t sz)
{
    if (h24) {
        snprintf(out, sz, "%02d", norm(hour));
    } else {
        snprintf(out, sz, "%d", to12(hour));
    }
}

void tfmt_duration(unsigned long secs, char *out, size_t sz)
{
    unsigned long d = secs / 86400, h = (secs / 3600) % 24, m = (secs / 60) % 60;
    if (d > 0) {
        snprintf(out, sz, "%lu d %lu h %02lu min", d, h, m);
    } else if (h > 0) {
        snprintf(out, sz, "%lu h %02lu min", h, m);
    } else if (m > 0) {
        snprintf(out, sz, "%lu min", m);
    } else {
        snprintf(out, sz, "%lu s", secs);
    }
}

/* "Wed 14 Oct" - %a/%b are the C locale's English abbreviations, the day is unpadded. */
static void day_text(const struct tm *tm, char *out, size_t sz)
{
    char wd[8], mon[8];
    strftime(wd, sizeof(wd), "%a", tm);
    strftime(mon, sizeof(mon), "%b", tm);
    snprintf(out, sz, "%s %d %s", wd, tm->tm_mday, mon);
}

static bool same_day(const struct tm *a, const struct tm *b)
{
    return a->tm_year == b->tm_year && a->tm_yday == b->tm_yday;
}

void tfmt_event_when(bool h24, time_t start, time_t end, bool all_day, char *out, size_t sz)
{
    struct tm s, e;
    localtime_r(&start, &s);
    char ds[24], de[24];
    day_text(&s, ds, sizeof(ds));
    if (all_day) {
        time_t last = end > start ? end - 1 : start;       /* the end is exclusive: the day after the last */
        localtime_r(&last, &e);
        if (same_day(&s, &e)) {
            snprintf(out, sz, "All day, %s", ds);
        } else {
            day_text(&e, de, sizeof(de));
            snprintf(out, sz, "All day, %s - %s", ds, de);
        }
        return;
    }
    char ts[16], te[16];
    tfmt_hm(h24, s.tm_hour, s.tm_min, ts, sizeof(ts));
    if (end <= start) {
        snprintf(out, sz, "%s, %s", ds, ts);
        return;
    }
    localtime_r(&end, &e);
    tfmt_hm(h24, e.tm_hour, e.tm_min, te, sizeof(te));
    if (same_day(&s, &e)) {
        snprintf(out, sz, "%s, %s - %s", ds, ts, te);
    } else {
        day_text(&e, de, sizeof(de));
        snprintf(out, sz, "%s %s - %s %s", ds, ts, de, te);
    }
}
