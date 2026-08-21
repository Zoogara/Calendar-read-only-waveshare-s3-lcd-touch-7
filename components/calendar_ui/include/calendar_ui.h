#pragma once
/* Public entry points for the calendar UI. Everything else in this
 * component is internal (see calendar_ui_internal.h). */

#include <stdbool.h>
#include <stdint.h>

#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the whole UI shell (nav rail, top bar, legend, and all four
 * views) on the active LVGL display. Must be called after
 * bsp_display_init(). cfg is kept by reference (not copied) - its
 * "enabled" flags are mutated live when the user taps a legend chip. */
void calendar_ui_init(app_settings_t *cfg);

/* Re-renders the currently active view (and legend) from whatever is now
 * in the event_store. Call this after every gcal_refresh_all(). Takes
 * the LVGL lock itself, so it's safe to call from the background refresh
 * task. */
void calendar_ui_refresh(void);

/* Marks the last sync attempt as failed - shows a warning glyph and tints
 * the "last synced" label red until the next successful calendar_ui_refresh()
 * clears it. Lets a silent background refresh failure (see gcal_refresh_all())
 * be visible on the device itself instead of only showing up as a stale
 * timestamp. Takes the LVGL lock itself, so it's safe to call from the
 * background refresh task. */
void calendar_ui_notify_sync_failed(void);

/* Re-anchors the "today"/current-day cursor to the system clock and
 * re-renders. calendar_ui_init() runs before Wi-Fi/SNTP, when the ESP32's
 * clock (no battery-backed RTC on this board) is still wrong - without
 * this, the UI stays anchored to whatever bogus date time(NULL) returned
 * at boot even after the clock and event data are both correct. Call this
 * once, right after the first successful SNTP sync. Takes the LVGL lock
 * itself, so it's safe to call from the background network task. */
void calendar_ui_sync_today(void);

/* True while the display is in its idle "screensaver" state (backlight
 * off, anti-image-retention noise pattern shown instead of the calendar -
 * see ui_screensaver.c). The background refresh task uses this to skip
 * calendar syncs while the screen is off. */
bool calendar_ui_is_asleep(void);

/* Blocks the calling task until the screensaver wakes (a touch is
 * detected) or timeout_ms elapses, whichever comes first. Returns true if
 * woken by touch, false on timeout. Meant to replace a plain vTaskDelay()
 * between refresh cycles, so a wake immediately triggers a fresh sync
 * instead of waiting out the rest of the interval. */
bool calendar_ui_wait_wake(uint32_t timeout_ms);

/* Wakes calendar_ui_wait_wake() immediately, the same as a screensaver
 * wake touch would - used to let the UI (e.g. tapping the "last synced"
 * label) force an immediate refresh instead of waiting out the rest of
 * the normal interval. Safe to call from the LVGL task. */
void calendar_ui_request_sync(void);

/* Empties the currently active view's rendered content (event blocks,
 * badges, chips - not its structural chrome) without touching
 * event_store - call right before the screen goes to sleep, since
 * nothing's visible behind the screensaver's noise pattern anyway.
 * Pair with calendar_ui_restore_active_view() on wake. Takes the LVGL
 * lock itself (reentrant-safe to call from an LVGL timer callback). */
void calendar_ui_release_active_view(void);

/* Re-populates the currently active view from whatever's now in
 * event_store, undoing calendar_ui_release_active_view() - call right
 * after waking, before the display is shown again. Takes the LVGL lock
 * itself. */
void calendar_ui_restore_active_view(void);

#ifdef __cplusplus
}
#endif
