/* components/readlater: the link check every link goes through, and the read-later list (add, duplicates, the cap,
 * expiry, removal, and the form kept in flash). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link_url.h"
#include "readlater_store.h"
#include "test.h"

#define T0 1795310400u            /* 2026-11-22 01:20 UTC: a valid clock */
#define DAY 86400u

static void urls_that_are_fine(void)
{
    CHECK(link_url_ok("https://en.wikipedia.org/wiki/Magnus_Carlsen"));
    CHECK(link_url_ok("http://example.com"));
    CHECK(link_url_ok("HTTPS://Example.COM/Path?x=1&y=2#frag"));
    CHECK(link_url_ok("https://www.abc.net.au/news/2026-11-22/some-story/104123456"));
    CHECK(link_url_ok("https://host.example:8443/a%20b/c?d=e%26f"));
    CHECK(link_url_ok("https://192.168.0.1/x"));
    CHECK(link_url_ok("https://a.co?x=1"));                    /* no path, a query */
}

static void urls_that_are_refused(void)
{
    CHECK(!link_url_ok(NULL));
    CHECK(!link_url_ok(""));
    CHECK(!link_url_ok("javascript:alert(1)"));
    CHECK(!link_url_ok("JavaScript:alert(1)"));
    CHECK(!link_url_ok("data:text/html,<script>alert(1)</script>"));
    CHECK(!link_url_ok("ftp://example.com/file"));
    CHECK(!link_url_ok("//example.com/x"));                      /* scheme-relative */
    CHECK(!link_url_ok("/relative/path"));
    CHECK(!link_url_ok("example.com"));
    CHECK(!link_url_ok("https://"));                             /* no host */
    CHECK(!link_url_ok("https:///path"));
    CHECK(!link_url_ok("https://trusted.example@evil.example/")); /* user-info disguise */
    CHECK(!link_url_ok("https://user:pw@example.com/"));
    CHECK(!link_url_ok("https://exa mple.com/"));
    CHECK(!link_url_ok("https://example.com/a b"));              /* space */
    CHECK(!link_url_ok("https://example.com/a\tb"));
    CHECK(!link_url_ok("https://example.com/a\nb"));
    CHECK(!link_url_ok("https://example.com/\"onmouseover=alert(1)"));
    CHECK(!link_url_ok("https://example.com/'><script>"));
    CHECK(!link_url_ok("https://example.com/<b>"));
    CHECK(!link_url_ok("https://example.com/a\\b"));
    CHECK(!link_url_ok("https://example.com/`x`"));
    CHECK(!link_url_ok("https://example.com/caf\xC3\xA9"));       /* non-ASCII: must arrive percent-encoded */
    CHECK(!link_url_ok("https://exam_ple.com/"));
    CHECK(!link_url_ok("https://example.com:/x"));                /* empty port */
    CHECK(!link_url_ok("https://example.com:123456/x"));          /* port too long */
    CHECK(!link_url_ok("https://example.com:80a/x"));
    CHECK(!link_url_ok("https://[::1]/x"));                       /* brackets */
    /* length: 199 characters is the most (LINK_URL_MAX 200 includes the NUL) */
    char big[300];
    strcpy(big, "https://example.com/");
    memset(big + strlen(big), 'a', 179);
    big[20 + 179] = '\0';
    CHECK_INT(strlen(big), 199);
    CHECK(link_url_ok(big));
    big[199] = 'a';
    big[200] = '\0';
    CHECK(!link_url_ok(big));
}

static void add_puts_newest_first_and_cleans_the_title(void)
{
    rl_list_t l = {0};
    CHECK_INT(rl_add(&l, T0, "First", "https://a.example/1"), RL_ADDED);
    CHECK_INT(rl_add(&l, T0 + 10, "  Second \t\n story  ", "https://a.example/2"), RL_ADDED);
    CHECK_INT(l.n, 2);
    CHECK_STR(l.items[0].title, "Second story");              /* collapsed and trimmed, newest first */
    CHECK_STR(l.items[1].title, "First");
    CHECK_INT(l.items[0].saved, T0 + 10);
    CHECK_INT(rl_add(&l, T0, "x", "javascript:alert(1)"), RL_BAD_URL);
    CHECK_INT(l.n, 2);
    CHECK_INT(rl_add(&l, T0, "", "https://a.example/empty-title"), RL_ADDED);
    CHECK_STR(l.items[0].title, "https://a.example/empty-title");   /* the address stands in */
    CHECK_INT(rl_add(&l, T0, NULL, "https://a.example/null-title"), RL_ADDED);
    CHECK_STR(l.items[0].title, "https://a.example/null-title");
}

static void a_long_title_is_cut_on_a_character_boundary(void)
{
    rl_list_t l = {0};
    char t[400];
    t[0] = '\0';
    for (int i = 0; i < 100; i++) strcat(t, "\xC3\xA9");       /* 100 x e-acute = 200 bytes */
    rl_add(&l, T0, t, "https://a.example/long");
    size_t n = strlen(l.items[0].title);
    CHECK(n > 0 && n < RL_TITLE_LEN);
    CHECK(n % 2 == 0);                                         /* never half a character */
    CHECK(memcmp(l.items[0].title, "\xC3\xA9", 2) == 0);
    /* four-byte characters, same */
    t[0] = '\0';
    for (int i = 0; i < 60; i++) strcat(t, "\xF0\x9F\x98\x80");
    rl_add(&l, T0, t, "https://a.example/emoji");
    CHECK(strlen(l.items[0].title) % 4 == 0);
}

static void the_same_link_moves_to_the_top(void)
{
    rl_list_t l = {0};
    rl_add(&l, T0, "A", "https://a.example/a");
    rl_add(&l, T0 + 1, "B", "https://a.example/b");
    rl_add(&l, T0 + 2, "C", "https://a.example/c");
    CHECK_INT(rl_add(&l, T0 + 100, "A again", "https://a.example/a"), RL_ALREADY);
    CHECK_INT(l.n, 3);
    CHECK_STR(l.items[0].title, "A again");
    CHECK_INT(l.items[0].saved, T0 + 100);                     /* its life restarts */
    CHECK_STR(l.items[1].title, "C");
    CHECK_STR(l.items[2].title, "B");
}

static void the_list_is_capped_and_the_oldest_goes(void)
{
    rl_list_t l = {0};
    char url[64], title[32];
    for (int i = 0; i < RL_MAX; i++) {
        snprintf(url, sizeof(url), "https://a.example/%d", i);
        snprintf(title, sizeof(title), "T%d", i);
        CHECK_INT(rl_add(&l, T0 + i, title, url), RL_ADDED);
    }
    CHECK_INT(l.n, RL_MAX);
    CHECK_INT(rl_add(&l, T0 + 99, "newest", "https://a.example/new"), RL_ADDED_DROPPED_OLDEST);
    CHECK_INT(l.n, RL_MAX);
    CHECK_STR(l.items[0].title, "newest");
    CHECK_STR(l.items[RL_MAX - 1].title, "T1");                /* T0 was the oldest and is gone */
    /* re-saving a link in a full list doesn't drop anything */
    CHECK_INT(rl_add(&l, T0 + 100, "T5 again", "https://a.example/5"), RL_ALREADY);
    CHECK_INT(l.n, RL_MAX);
    CHECK_STR(l.items[RL_MAX - 1].title, "T1");
}

static void expiry(void)
{
    rl_list_t l = {0};
    rl_add(&l, T0, "old", "https://a.example/old");
    rl_add(&l, T0 + 10 * DAY, "mid", "https://a.example/mid");
    rl_add(&l, T0 + 14 * DAY, "new", "https://a.example/new");
    uint32_t now = T0 + 15 * DAY;
    CHECK_INT(rl_days_left(&l.items[2], now, 14), 0);          /* 15 days old, kept 14 */
    CHECK_INT(rl_days_left(&l.items[1], now, 14), 9);          /* 5 days old: 9 left */
    CHECK_INT(rl_days_left(&l.items[0], now, 14), 13);         /* 1 day old: 13 left */
    CHECK_INT(rl_days_left(&l.items[0], now - 3600, 14), 14);  /* 23 hours old: part days round up, so still 14 */
    CHECK_INT(rl_prune(&l, now, 14), 1);
    CHECK_INT(l.n, 2);
    CHECK_STR(l.items[1].title, "mid");
    CHECK_INT(rl_prune(&l, now, 14), 0);                       /* idempotent */
    CHECK_INT(rl_prune(&l, T0 + 30 * DAY, 14), 2);             /* everything eventually */
    CHECK_INT(l.n, 0);
    /* exactly at the limit survives; a second over does not */
    rl_add(&l, T0, "edge", "https://a.example/edge");
    CHECK_INT(rl_prune(&l, T0 + 7 * DAY, 7), 0);
    CHECK_INT(rl_prune(&l, T0 + 7 * DAY + 1, 7), 1);
    /* a shorter retention applies to links already held */
    rl_add(&l, T0, "x", "https://a.example/x");
    CHECK_INT(rl_prune(&l, T0 + 4 * DAY, 3), 1);
    /* nonsense retention is treated as one day */
    rl_add(&l, T0, "y", "https://a.example/y");
    CHECK_INT(rl_prune(&l, T0 + 2 * DAY, 0), 1);
}

static void a_clock_that_is_not_set_expires_nothing_and_stamps_later(void)
{
    rl_list_t l = {0};
    CHECK_INT(rl_add(&l, 5000, "no clock", "https://a.example/nc"), RL_ADDED);   /* the clock reads 1970 */
    CHECK_INT(l.items[0].saved, 0);                                              /* "not known yet" */
    CHECK_INT(rl_days_left(&l.items[0], 5000, 14), -1);
    CHECK_INT(rl_prune(&l, 5000, 14), 0);                                        /* still no clock: nothing happens */
    CHECK_INT(l.items[0].saved, 0);
    CHECK_INT(rl_days_left(&l.items[0], T0, 14), -1);                            /* unstamped: unknown */
    CHECK_INT(rl_prune(&l, T0, 14), 0);                                          /* the clock arrives: its life starts */
    CHECK_INT(l.items[0].saved, T0);
    CHECK_INT(rl_days_left(&l.items[0], T0, 14), 14);
    CHECK_INT(rl_prune(&l, T0 + 15 * DAY, 14), 1);
    /* a link saved "in the future" (a clock that was wrong) is kept, not expired */
    rl_add(&l, T0 + 400 * DAY, "future", "https://a.example/f");
    CHECK_INT(rl_prune(&l, T0, 14), 0);
    CHECK_INT(l.n, 1);
}

static void remove_checks_the_saved_time(void)
{
    rl_list_t l = {0};
    rl_add(&l, T0, "A", "https://a.example/a");
    rl_add(&l, T0 + 5, "B", "https://a.example/b");
    CHECK(!rl_remove(&l, 0, T0));                              /* index 0 is B (saved T0+5): a stale page */
    CHECK(!rl_remove(&l, 2, T0));
    CHECK(!rl_remove(&l, -1, T0));
    CHECK_INT(l.n, 2);
    CHECK(rl_remove(&l, 0, T0 + 5));
    CHECK_INT(l.n, 1);
    CHECK_STR(l.items[0].title, "A");
    rl_clear(&l);
    CHECK_INT(l.n, 0);
}

static void flash_form_round_trips(void)
{
    rl_list_t l = {0}, back = {0};
    rl_add(&l, T0, "Caf\xC3\xA9 opens", "https://a.example/cafe?x=1&y=2");
    rl_add(&l, T0 + 3600, "Second", "https://b.example/");
    rl_add(&l, 100, "No clock", "https://c.example/z");        /* saved = 0 */
    uint8_t buf[RL_ENCODED_MAX];
    size_t n = rl_encode(&l, buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf));
    CHECK(rl_decode(buf, n, &back));
    CHECK_INT(back.n, 3);
    for (int i = 0; i < 3; i++) {
        CHECK_STR(back.items[i].title, l.items[i].title);
        CHECK_STR(back.items[i].url, l.items[i].url);
        CHECK_INT(back.items[i].saved, l.items[i].saved);
    }
    /* an empty list round-trips too */
    rl_list_t e = {0};
    n = rl_encode(&e, buf, sizeof(buf));
    CHECK(n > 0 && rl_decode(buf, n, &back) && back.n == 0);
    /* a full list fits RL_ENCODED_MAX */
    rl_list_t full = {0};
    char url[LINK_URL_MAX], title[RL_TITLE_LEN];
    for (int i = 0; i < RL_MAX; i++) {
        snprintf(url, sizeof(url), "https://example.com/%0170d", i);
        memset(title, 'x', sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        rl_add(&full, T0 + i, title, url);
    }
    n = rl_encode(&full, buf, sizeof(buf));
    CHECK(n > 0 && n <= RL_ENCODED_MAX);
    CHECK(rl_decode(buf, n, &back) && back.n == RL_MAX);
    /* a too-small buffer is refused, not overrun */
    uint8_t small[10];
    CHECK_INT(rl_encode(&full, small, sizeof(small)), 0);
}

static void a_damaged_flash_form_is_refused(void)
{
    rl_list_t l = {0}, back = {0};
    rl_add(&l, T0, "Title", "https://a.example/x");
    uint8_t buf[RL_ENCODED_MAX];
    size_t n = rl_encode(&l, buf, sizeof(buf));
    CHECK(rl_decode(buf, n, &back));
    uint8_t m[RL_ENCODED_MAX];
    int refused = 0, tried = 0;
    for (size_t pos = 0; pos < n; pos++) {                     /* every single changed byte fails the CRC or a check */
        memcpy(m, buf, n);
        m[pos] ^= 0x40;
        tried++;
        if (!rl_decode(m, n, &back)) refused++;
    }
    CHECK_INT(refused, tried);
    for (size_t cut = 0; cut < n; cut++) {                     /* every truncation */
        CHECK(!rl_decode(buf, cut, &back));
    }
    CHECK(!rl_decode(NULL, 0, &back));
    /* a perfectly checksummed file that holds an address link_url_ok() refuses must still be refused */
    rl_list_t evil = {0};
    evil.n = 1;
    evil.items[0].saved = T0;
    strcpy(evil.items[0].title, "x");
    strcpy(evil.items[0].url, "javascript:alert(1)");
    n = rl_encode(&evil, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(!rl_decode(buf, n, &back));
}

void test_readlater(void)
{
    RUN(urls_that_are_fine);
    RUN(urls_that_are_refused);
    RUN(add_puts_newest_first_and_cleans_the_title);
    RUN(a_long_title_is_cut_on_a_character_boundary);
    RUN(the_same_link_moves_to_the_top);
    RUN(the_list_is_capped_and_the_oldest_goes);
    RUN(expiry);
    RUN(a_clock_that_is_not_set_expires_nothing_and_stamps_later);
    RUN(remove_checks_the_saved_time);
    RUN(flash_form_round_trips);
    RUN(a_damaged_flash_form_is_refused);
}
