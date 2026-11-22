#pragma once
/*
 * The read-later list on the device: the list model of readlater_store.h in PSRAM, kept in flash (its own NVS entry, not
 * the settings), pruned by age, and shown on the config web page (/links). A link is saved from the card the news-feed
 * and "On this day" pages open when an item is tapped; each lives for the number of days set on the config page.
 *
 * Thread-safe: the LVGL task adds, the web server task lists and removes, the network task housekeeps. The flash write
 * itself runs on a short-lived task with an internal-RAM stack (a flash write disables the cache, which a PSRAM stack
 * can't survive - the same reason config_web.c's settings save has its own task).
 */

#include "readlater_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at boot, after nvs_flash_init(). Loads what was saved; a damaged or missing entry is an empty list. */
void readlater_init(void);

rl_add_result_t readlater_add(const char *title, const char *url, uint16_t keep_days);

/* Copies the list (after pruning it) into *out, which must be a whole rl_list_t; returns the count. */
int readlater_copy(rl_list_t *out, uint16_t keep_days);

/* Removes entry `index` if its saved time is still `saved` (see rl_remove()). */
bool readlater_remove(int index, uint32_t saved, uint16_t keep_days);
void readlater_clear(void);

/* Drops expired links, stamps ones saved before the clock was set, and retries a flash write that failed. Cheap: call
 * it after each calendar sync. */
void readlater_housekeeping(uint16_t keep_days);

#ifdef __cplusplus
}
#endif
