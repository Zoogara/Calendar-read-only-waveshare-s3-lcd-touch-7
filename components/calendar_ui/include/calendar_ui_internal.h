#pragma once
/* Shared internals between calendar_ui.c (shell) and the four view
 * modules (ui_month.c, ui_week.c, ui_day.c, ui_upnext.c). Not part of
 * the component's public API. */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "lvgl.h"
#include "app_settings.h"
#include "gcal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_VIEW_MONTH = 0,
    UI_VIEW_WEEK,
    UI_VIEW_DAY,
    UI_VIEW_UPNEXT,
} ui_view_t;

/* Same glyph set as lv_font_montserrat_14/20 (LVGL's stock compiled
 * fonts) plus two extra glyphs remapped onto U+2610/U+2611 (BALLOT BOX /
 * BALLOT BOX WITH CHECK) - see gcal_font_14.c's header comment for the
 * full story. Used everywhere lv_font_montserrat_14/20 would otherwise
 * be, so any event summary starting with one of those two Unicode
 * checkbox characters (some calendars, e.g. a synced "Tasks" list, use
 * this as an ad hoc convention) renders correctly with no special-casing
 * needed at any call site. */
extern const lv_font_t gcal_font_14;
extern const lv_font_t gcal_font_20;

/* Digits + colon only, at a size meant to be read from across a room -
 * see gcal_font_clock.c's header comment. Used only by ui_clock.c. */
extern const lv_font_t gcal_font_clock;

/* One glyph only - FontAwesome 5 Solid's "clock" icon (U+F017), for the
 * ambient-clock toggle button in calendar_ui.c's top bar. See
 * gcal_font_icon_clock.c's header comment for why this needed its own
 * standalone font rather than reusing gcal_font_20's LV_SYMBOL_* set. */
extern const lv_font_t gcal_font_icon_clock;

/* One glyph only - FontAwesome 5 Solid's "bell" icon (U+F0F3), for the
 * ambient clock's small "a reminder's pending" indicator. See
 * gcal_font_icon_bell.c's header comment for why this needed its own
 * standalone font rather than reusing gcal_font_20's LV_SYMBOL_* set. */
extern const lv_font_t gcal_font_icon_bell;

/* --- shared context (implemented in calendar_ui.c) --- */
app_settings_t *ui_get_cfg(void);
bool ui_calendar_enabled(uint8_t calendar_index);
/* Switches the shell to the day view focused on the given day and
 * re-populates it - used by month/week views' "tap a day" handlers. */
void ui_switch_to_day(time_t day_start);

/* --- date arithmetic helpers (local time, honours the configured
 * POSIX TZ), implemented in ui_time.c --- */
time_t ui_start_of_day(time_t t);
time_t ui_add_days(time_t t, int days);
time_t ui_start_of_week(time_t t);      /* Monday-based, to match AU convention */
time_t ui_start_of_month(time_t t);
time_t ui_add_months(time_t t, int delta);
bool ui_same_local_day(time_t a, time_t b);

/* --- view modules --- */
lv_obj_t *ui_month_create(lv_obj_t *parent);
void ui_month_populate(lv_obj_t *root, time_t cursor);
void ui_month_title(time_t cursor, char *out, size_t out_sz);
/* Empties this view's rendered content (event bars, badges, chips - not
 * the view's own structural chrome, which is created once and reused)
 * without repopulating it - called when switching to a different view,
 * so a view isn't left holding a full set of event objects in RAM the
 * whole time it's hidden. Cheap to undo: the next time this view becomes
 * active, populate() rebuilds it from event_store as it already does. */
void ui_month_release(void);

lv_obj_t *ui_week_create(lv_obj_t *parent);
void ui_week_populate(lv_obj_t *root, time_t cursor);
void ui_week_title(time_t cursor, char *out, size_t out_sz);
void ui_week_release(void);

lv_obj_t *ui_day_create(lv_obj_t *parent);
void ui_day_populate(lv_obj_t *root, time_t cursor);
void ui_day_title(time_t cursor, char *out, size_t out_sz);
void ui_day_release(void);

lv_obj_t *ui_upnext_create(lv_obj_t *parent);
void ui_upnext_populate(lv_obj_t *root);
void ui_upnext_title(char *out, size_t out_sz);
void ui_upnext_release(void);

/* --- screensaver (implemented in ui_screensaver.c) --- */
void ui_screensaver_init(void);
/* Runtime-only (not persisted, always true again after a reboot) on/off
 * switch for the ambient clock - see ui_screensaver.c's header comment
 * and s_clock_feature_enabled. Driven by the eye icon in calendar_ui.c's
 * top bar. */
bool ui_screensaver_clock_enabled(void);
void ui_screensaver_toggle_clock_enabled(void);

/* --- ambient clock (implemented in ui_clock.c) - shown by ui_screensaver.c
 * in place of the calendar once it's gone idle, while presence is still
 * detected --- */
/* Creates the (initially hidden) full-screen clock overlay on `parent`
 * (ui_screensaver.c passes lv_layer_top()). Call once, from
 * ui_screensaver_init(). */
lv_obj_t *ui_clock_create(lv_obj_t *parent);
/* Re-renders the clock for "now" - current time and day/night colour
 * choice (from ui_get_cfg()'s view_start_hour/view_end_hour). Call every
 * ~1s while the clock is the thing being shown - it's cheap to call when
 * nothing's actually changed (see its own comment) so there's no need to
 * gate calls to this on the caller's side. Presence is not this
 * function's concern: ui_screensaver.c decides when to show the clock at
 * all based on presence, this just renders it while it's showing. */
void ui_clock_update(void);

/* --- event reminder pop-over (implemented in ui_reminder.c) - shows a
 * timed event's details starting 15 minutes before it's due --- */
/* Creates the (initially hidden) reminder card on `parent`
 * (ui_screensaver.c passes lv_layer_top()). Call once, from
 * ui_screensaver_init(). */
void ui_reminder_init(lv_obj_t *parent);
/* Scans event_store for a timed event on an enabled calendar starting
 * within the next 15 minutes, and shows/updates/hides the pop-over
 * accordingly. `calendar_visible` should be true only while the calendar
 * itself - not the ambient clock or sleep screen - is what's currently on
 * screen; the pop-over stays hidden otherwise regardless of whether a
 * reminder is pending (see ui_reminder_has_pending() for that). Call
 * every ~1s regardless of display state - cheap, a handful of
 * event_store reads. */
void ui_reminder_tick(bool calendar_visible);
/* True whenever there's a not-yet-dismissed, not-yet-started reminder
 * candidate, independent of whether the pop-over itself is currently
 * allowed to be visible - used by ui_clock.c to decide whether to show
 * its small bell indicator while the ambient clock is up. */
bool ui_reminder_has_pending(void);

/* --- display settings dialog (implemented in ui_settings_dialog.c) --- */
void ui_settings_dialog_show(void);

#ifdef __cplusplus
}
#endif
