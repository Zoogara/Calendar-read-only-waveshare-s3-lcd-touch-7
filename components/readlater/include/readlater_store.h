#pragma once
/*
 * The read-later list as a plain data structure (pure C, no ESP-IDF - host-tested): links saved from the news feed and
 * "On this day" pages, newest first, a few days' life each. readlater.c puts this in PSRAM, keeps it in flash and
 * serves it on the config page.
 *
 * The clock matters: a link's age is measured from the wall-clock time it was saved. Saved while the clock isn't set
 * (before the first network time), it is stamped 0 = "age not known yet" and gets its real stamp - starting its life -
 * the first time the list is looked at with a valid clock. Nothing is ever expired on a clock that isn't set.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "link_url.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RL_MAX         20
#define RL_TITLE_LEN   96
#define RL_MIN_VALID_TIME 1577836800u      /* 2020-01-01: any earlier "now" means the clock isn't set */

typedef struct {
    uint32_t saved;                        /* seconds since the epoch, 0 = not yet known */
    char title[RL_TITLE_LEN];              /* UTF-8, plain text, never empty */
    char url[LINK_URL_MAX];                /* passes link_url_ok() */
} rl_link_t;

typedef struct {
    int n;
    rl_link_t items[RL_MAX];               /* newest first */
} rl_list_t;

typedef enum {
    RL_ADDED = 0,
    RL_ALREADY,                            /* the same link was already saved: moved to the top, life restarted */
    RL_ADDED_DROPPED_OLDEST,               /* saved, but the list was full so the oldest went */
    RL_BAD_URL,                            /* not saved: not a plain web address */
} rl_add_result_t;

/* Saves a link at the top. `title` is cleaned (control characters become spaces, cut at a UTF-8 boundary; if nothing
 * is left the address stands in). `now` is the current time, or anything below RL_MIN_VALID_TIME if the clock isn't set. */
rl_add_result_t rl_add(rl_list_t *l, uint32_t now, const char *title, const char *url);

/* Drops links older than max_days, and stamps unstamped ones. Returns how many were dropped. Does nothing, and
 * returns 0, if `now` isn't a valid time. */
int rl_prune(rl_list_t *l, uint32_t now, int max_days);

/* Removes entry `index` provided its saved time is still `saved` (so a page that is out of date can't delete the
 * wrong link after the list has changed). */
bool rl_remove(rl_list_t *l, int index, uint32_t saved);
void rl_clear(rl_list_t *l);

/* Whole days left before the link expires, never negative; -1 if its age isn't known yet or the clock isn't set. */
int rl_days_left(const rl_link_t *k, uint32_t now, int max_days);

/* The form kept in flash: "GCRL", a version byte, a count, each link (saved time, title, address), and a CRC-32 of
 * everything before it. rl_encode returns the length written, or 0 if `cap` is too small; rl_decode refuses anything
 * that isn't exactly that (wrong magic, version or CRC, a length that overruns, an address link_url_ok() rejects). */
#define RL_ENCODED_MAX (4 + 1 + 1 + RL_MAX * (4 + 1 + RL_TITLE_LEN + 1 + LINK_URL_MAX) + 4)
size_t rl_encode(const rl_list_t *l, uint8_t *out, size_t cap);
bool rl_decode(const uint8_t *in, size_t len, rl_list_t *out);

#ifdef __cplusplus
}
#endif
