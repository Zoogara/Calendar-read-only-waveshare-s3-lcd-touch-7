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

/* --- display settings dialog (implemented in ui_settings_dialog.c) --- */
void ui_settings_dialog_show(void);

#ifdef __cplusplus
}
#endif
