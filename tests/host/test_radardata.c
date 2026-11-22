/* ha_radar_data.c: URL ids, clock and ISO time parsing, frame reuse planning. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ha_radar_data.h"
#include "test.h"

static void ids_from_urls(void)
{
    char id[RADAR_ID_MAX];
    CHECK(radar_id_from_url("https://tilecache.rainviewer.com/v2/radar/34ec6b97a411/256/7/-33.87/151.21/1/1_1.png", id, sizeof(id)));
    CHECK_STR(id, "34ec6b97a411");
    /* HA's own file URL has "/radar/" too, but its "id" has a dot: not for this function, and refused */
    CHECK(!radar_id_from_url("http://ha.local:8123/local/radar/abc123.png", id, sizeof(id)));
    CHECK(radar_id_from_url("https://x/v2/radar/abc123/256/7/1/2/1/1_1.png?x=1", id, sizeof(id)));
    CHECK_STR(id, "abc123");
}

static void ids_are_validated(void)
{
    char id[RADAR_ID_MAX];
    CHECK(!radar_id_from_url(NULL, id, sizeof(id)));
    CHECK(!radar_id_from_url("https://x/y/z.png", id, sizeof(id)));              /* no /radar/ */
    CHECK(!radar_id_from_url("https://x/radar//256/7", id, sizeof(id)));           /* empty id */
    CHECK(!radar_id_from_url("https://x/radar/../../etc/passwd", id, sizeof(id)));  /* not letters and digits */
    CHECK(!radar_id_from_url("https://x/radar/ab%2Fcd/1", id, sizeof(id)));
    CHECK(!radar_id_from_url("https://x/radar/ab-cd/1", id, sizeof(id)));
    CHECK(!radar_id_from_url("https://x/radar/0123456789012345678901234567890123/1", id, sizeof(id)));   /* too long */
    char tiny[4];
    CHECK(!radar_id_from_url("https://x/radar/abcdef/1", tiny, sizeof(tiny)));
    CHECK(!radar_id_from_url("https://x/radar/abcdef/1", id, 0));
    CHECK(radar_id_from_url("https://x/radar/abc/1", tiny, sizeof(tiny)));
    CHECK_STR(tiny, "abc");
}

static void clock_parsing(void)
{
    int h, m;
    CHECK(radar_parse_clock("1:10 PM", &h, &m));   CHECK_INT(h, 13); CHECK_INT(m, 10);
    CHECK(radar_parse_clock("12:20 AM", &h, &m));  CHECK_INT(h, 0);  CHECK_INT(m, 20);
    CHECK(radar_parse_clock("12:05 PM", &h, &m));  CHECK_INT(h, 12); CHECK_INT(m, 5);
    CHECK(radar_parse_clock("11:59 pm", &h, &m));  CHECK_INT(h, 23); CHECK_INT(m, 59);
    CHECK(radar_parse_clock("9:00AM", &h, &m));    CHECK_INT(h, 9);  CHECK_INT(m, 0);
    CHECK(radar_parse_clock("  4:30 PM  ", &h, &m)); CHECK_INT(h, 16);
    CHECK(radar_parse_clock("13:05", &h, &m));     CHECK_INT(h, 13); CHECK_INT(m, 5);
    CHECK(radar_parse_clock("07:30", &h, &m));     CHECK_INT(h, 7);
    CHECK(!radar_parse_clock("", &h, &m));
    CHECK(!radar_parse_clock("-", &h, &m));
    CHECK(!radar_parse_clock("unknown", &h, &m));
    CHECK(!radar_parse_clock("13:10 PM", &h, &m));   /* 13 o'clock PM */
    CHECK(!radar_parse_clock("0:10 AM", &h, &m));
    CHECK(!radar_parse_clock("1:60 PM", &h, &m));
    CHECK(!radar_parse_clock("24:00", &h, &m));
    CHECK(!radar_parse_clock("1:1 PM", &h, &m));
    CHECK(!radar_parse_clock("1:10 XM", &h, &m));
    CHECK(!radar_parse_clock("1:10 PM extra", &h, &m));
    CHECK(!radar_parse_clock(NULL, &h, &m));
    CHECK(!radar_parse_clock("123:10", &h, &m));
}

static void iso_parsing(void)
{
    time_t t;
    CHECK(radar_parse_iso_utc("1970-01-01T00:00:00Z", &t));  CHECK_INT(t, 0);
    CHECK(radar_parse_iso_utc("2026-11-22T01:20:00Z", &t));  CHECK_INT(t, 1795310400);
    CHECK(radar_parse_iso_utc("2000-02-29T12:00:00Z", &t));  CHECK_INT(t, 951825600);
    CHECK(radar_parse_iso_utc("2024-02-29 23:59:59", &t));   CHECK_INT(t, 1709251199);
    CHECK(!radar_parse_iso_utc("2026-02-29T00:00:00Z", &t));  /* not a leap year */
    CHECK(!radar_parse_iso_utc("1900-02-29T00:00:00Z", &t));
    CHECK(!radar_parse_iso_utc("2026-13-01T00:00:00Z", &t));
    CHECK(!radar_parse_iso_utc("2026-00-10T00:00:00Z", &t));
    CHECK(!radar_parse_iso_utc("2026-04-31T00:00:00Z", &t));
    CHECK(!radar_parse_iso_utc("2026-11-22T24:00:00Z", &t));
    CHECK(!radar_parse_iso_utc("2026-11-22T01:60:00Z", &t));
    CHECK(!radar_parse_iso_utc("2026-11-22", &t));
    CHECK(!radar_parse_iso_utc("unknown", &t));
    CHECK(!radar_parse_iso_utc("", &t));
    CHECK(!radar_parse_iso_utc("2026/11/22T01:20:00Z", &t));
    CHECK(!radar_parse_iso_utc("20x6-11-22T01:20:00Z", &t));
    CHECK(!radar_parse_iso_utc(NULL, &t));
}

static void frame_time_picks_today_or_yesterday(void)
{
    setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
    tzset();
    struct tm lt = {.tm_year = 126, .tm_mon = 10, .tm_mday = 22, .tm_hour = 13, .tm_min = 15, .tm_isdst = -1};   /* 22 Nov 2026 1:15 PM */
    time_t now = mktime(&lt);
    time_t f = radar_frame_time(now, 13, 10);          /* 5 minutes ago: today */
    CHECK_INT(now - f, 5 * 60);
    f = radar_frame_time(now, 13, 18);                 /* 3 minutes ahead: within the slack, still today */
    CHECK_INT(f - now, 3 * 60);
    f = radar_frame_time(now, 23, 50);                 /* later today = yesterday's */
    CHECK_INT(now - f, 13 * 3600 + 25 * 60);
    lt = (struct tm){.tm_year = 126, .tm_mon = 10, .tm_mday = 22, .tm_hour = 0, .tm_min = 5, .tm_isdst = -1};   /* just after midnight */
    now = mktime(&lt);
    f = radar_frame_time(now, 23, 50);
    CHECK_INT(now - f, 15 * 60);
    unsetenv("TZ");
    tzset();
}

static void plan_reuses_held_frames(void)
{
    char old_ids[6][RADAR_ID_MAX] = {"a", "b", "c", "d", "e", "f"};
    char new_ids[6][RADAR_ID_MAX] = {"b", "c", "d", "e", "f", "g"};      /* one new frame, the oldest dropped */
    int reuse[6];
    CHECK_INT(radar_plan_frames(old_ids, 6, new_ids, 6, reuse), 1);
    int want[6] = {1, 2, 3, 4, 5, -1};
    for (int i = 0; i < 6; i++) CHECK_INT(reuse[i], want[i]);
    CHECK_INT(radar_plan_frames(old_ids, 6, old_ids, 6, reuse), 0);       /* nothing new */
    CHECK_INT(radar_plan_frames(old_ids, 0, new_ids, 6, reuse), 6);       /* nothing held */
    char gone[6][RADAR_ID_MAX] = {"x", "y", "z", "w", "v", "u"};
    CHECK_INT(radar_plan_frames(old_ids, 6, gone, 6, reuse), 6);          /* everything replaced */
    CHECK_INT(radar_plan_frames(old_ids, 6, new_ids, 0, reuse), 0);
}

static void order_is_always_chronological(void)
{
    char ids[6][RADAR_ID_MAX] = {"f", "e", "d", "c", "b", "a"};          /* HA's order: newest first */
    time_t times[6] = {6000, 5400, 4800, 4200, 3600, 3000};
    int order[6];
    CHECK_INT(radar_order_frames(times, ids, 6, order), 6);
    for (int i = 0; i < 6; i++) CHECK_INT(order[i], 5 - i);                /* a, b, c, d, e, f */

    char shuf[6][RADAR_ID_MAX] = {"c", "a", "f", "b", "e", "d"};         /* any order in, ascending out */
    time_t st[6] = {3, 1, 6, 2, 5, 4};
    CHECK_INT(radar_order_frames(st, shuf, 6, order), 6);
    for (int i = 1; i < 6; i++) CHECK(st[order[i - 1]] < st[order[i]]);

    time_t dup_t[4] = {10, 20, 20, 30};                                    /* the same time twice: one is dropped */
    char dup_i[4][RADAR_ID_MAX] = {"a", "b", "c", "d"};
    CHECK_INT(radar_order_frames(dup_t, dup_i, 4, order), 3);
    char same_i[3][RADAR_ID_MAX] = {"a", "b", "a"};                        /* the same id twice */
    time_t same_t[3] = {10, 20, 30};
    CHECK_INT(radar_order_frames(same_t, same_i, 3, order), 2);
    CHECK_INT(radar_order_frames(times, ids, 0, order), 0);
    CHECK_INT(radar_order_frames(times, ids, 1, order), 1);
}

static void next_check_is_two_minutes_past_the_ten(void)
{
    /* 2026-11-22T01:20:00Z = 1795310400 is on a mark */
    time_t mark = 1795310400;
    CHECK_INT(radar_next_check(mark - 1), mark + 120);                    /* just before the mark: that mark's :02 */
    CHECK_INT(radar_next_check(mark), mark + 120);                        /* on the mark: this mark's :02 (still ahead) */
    CHECK_INT(radar_next_check(mark + 119), mark + 120);
    CHECK_INT(radar_next_check(mark + 120), mark + 600 + 120);            /* at :02 itself: wait for the next */
    CHECK_INT(radar_next_check(mark + 300), mark + 600 + 120);
    CHECK_INT(radar_next_check(mark + 599), mark + 600 + 120);
    for (time_t t = mark; t < mark + 3600; t += 37) {                      /* always ahead, never more than 10 min away, always :x2 */
        time_t n = radar_next_check(t);
        CHECK(n > t && n - t <= 600);
        CHECK_INT(n % 600, 120);
    }
}

void test_radardata(void)
{
    RUN(ids_from_urls);
    RUN(ids_are_validated);
    RUN(clock_parsing);
    RUN(iso_parsing);
    RUN(frame_time_picks_today_or_yesterday);
    RUN(plan_reuses_held_frames);
    RUN(order_is_always_chronological);
    RUN(next_check_is_two_minutes_past_the_ten);
}
