#include "time_fmt.h"

#include <stdio.h>

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
