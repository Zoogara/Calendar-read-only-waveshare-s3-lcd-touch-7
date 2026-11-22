#include "ha_radar_data.h"
#include <ctype.h>
#include <string.h>

bool radar_id_from_url(const char *url, char *id, size_t id_sz)
{
    if (url == NULL || id == NULL || id_sz == 0) {
        return false;
    }
    const char *p = strstr(url, "/radar/");
    if (p == NULL) {
        return false;
    }
    p += 7;
    size_t n = strcspn(p, "/?#");
    if (n == 0 || n >= id_sz || n >= RADAR_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)p[i])) {
            return false;
        }
    }
    memcpy(id, p, n);
    id[n] = '\0';
    return true;
}

bool radar_parse_clock(const char *s, int *hour24, int *minute)
{
    if (s == NULL) {
        return false;
    }
    while (*s == ' ') s++;
    if (!isdigit((unsigned char)*s)) {
        return false;
    }
    int h = 0, n = 0;
    while (isdigit((unsigned char)*s) && n < 3) {
        h = h * 10 + (*s++ - '0');
        n++;
    }
    if (n > 2 || *s != ':') {
        return false;
    }
    s++;
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
        return false;
    }
    int m = (s[0] - '0') * 10 + (s[1] - '0');
    s += 2;
    while (*s == ' ') s++;
    if (m > 59) {
        return false;
    }
    if (*s == '\0') {
        if (h > 23) return false;
    } else {
        char a = (char)toupper((unsigned char)s[0]), b = (char)toupper((unsigned char)s[1]);
        if ((a != 'A' && a != 'P') || b != 'M' || h < 1 || h > 12) {
            return false;
        }
        const char *rest = s + 2;
        while (*rest == ' ') rest++;
        if (*rest != '\0') return false;
        h = h % 12 + (a == 'P' ? 12 : 0);
    }
    *hour24 = h;
    *minute = m;
    return true;
}

static int days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static bool digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

bool radar_parse_iso_utc(const char *s, time_t *out)
{
    if (s == NULL || strlen(s) < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') ||
        s[13] != ':' || s[16] != ':') {
        return false;
    }
    int Y, M, D, h, m, sec;
    if (!digits(s, 4, &Y) || !digits(s + 5, 2, &M) || !digits(s + 8, 2, &D) || !digits(s + 11, 2, &h) ||
        !digits(s + 14, 2, &m) || !digits(s + 17, 2, &sec)) {
        return false;
    }
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (Y < 1970 || Y > 2100 || M < 1 || M > 12 || D < 1 || h > 23 || m > 59 || sec > 60) {
        return false;
    }
    if (D > dim[M - 1] + (M == 2 && is_leap(Y) ? 1 : 0)) {
        return false;
    }
    *out = (time_t)days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + (sec == 60 ? 59 : sec);
    return true;
}

time_t radar_frame_time(time_t now, int hour24, int minute)
{
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = hour24;
    lt.tm_min = minute;
    lt.tm_sec = 0;
    lt.tm_isdst = -1;
    time_t t = mktime(&lt);
    if (t > now + 5 * 60) {          /* a few minutes of slack: HA's clock and ours can differ */
        t -= 86400;
    }
    return t;
}

int radar_plan_frames(const char old_ids[][RADAR_ID_MAX], int n_old, const char new_ids[][RADAR_ID_MAX], int n_new,
                      int reuse_index[])
{
    int fetch = 0;
    for (int i = 0; i < n_new; i++) {
        reuse_index[i] = -1;
        for (int j = 0; j < n_old; j++) {
            if (strncmp(new_ids[i], old_ids[j], RADAR_ID_MAX) == 0) {
                reuse_index[i] = j;
                break;
            }
        }
        if (reuse_index[i] < 0) fetch++;
    }
    return fetch;
}

int radar_order_frames(const time_t times[], const char ids[][RADAR_ID_MAX], int n, int order[])
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < m && !dup; k++) {
            dup = times[order[k]] == times[i] || strncmp(ids[order[k]], ids[i], RADAR_ID_MAX) == 0;
        }
        if (dup) {
            continue;
        }
        int pos = m;                     /* insertion sort, oldest first; stable for equal times (dropped above) */
        while (pos > 0 && times[order[pos - 1]] > times[i]) {
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = i;
        m++;
    }
    return m;
}

time_t radar_next_check(time_t now)
{
    time_t mark = (now / 600 + 1) * 600;           /* the next UTC :00, :10, :20 ... strictly after now */
    time_t at = mark + RADAR_CHECK_OFFSET_S;
    if (at - 600 > now) {                            /* the previous mark's check is still ahead (now is in :00-:02) */
        at -= 600;
    }
    return at;
}
