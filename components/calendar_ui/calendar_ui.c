#include "calendar_ui.h"
#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "board_bsp.h"
#include "event_store.h"
#include "provisioning.h"
#include "ota_update.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "calendar_ui";

static app_settings_t *s_cfg;
static ui_view_t s_view = UI_VIEW_MONTH;
static time_t s_cursor; /* the day currently focused, local midnight */

static lv_obj_t *s_screen;
static lv_obj_t *s_title_label;
static lv_obj_t *s_updated_label;
static lv_obj_t *s_legend_row;
static lv_obj_t *s_nav_btns[4];

static lv_obj_t *s_view_roots[4];
static bool s_sync_failed = false;

/* Guards update_title()'s forced lv_refr_now() calls (see its own
 * comment) against running during calendar_ui_init()'s very first
 * select_view() - forcing a synchronous refresh before LVGL's own redraw
 * timer has ticked even once has been confirmed on real hardware to wedge
 * this panel's dual-framebuffer sync (select_view_force_redraw()'s
 * comment documents the same finding, from a watchdog-hang; seen again
 * 2026-09-09 as a hard "cache disabled but cached memory region accessed"
 * panic instead - same root cause, LVGL's refr_sync_areas()/
 * lv_draw_sw_buffer_copy() reached mid-boot before anything's ready for
 * it, just a different failure mode depending on what else was going on
 * at that exact moment). Set true once, right after calendar_ui_init()'s
 * initial select_view() returns - every update_title() from then on
 * (including the handful still inside calendar_ui_init() via
 * ui_screensaver_init(), if any were ever added) behaves as before. */
static bool s_boot_forced_redraw_ok = false;

static void render_current_view(void);
static void update_title(void);
static void update_legend(void);
static void select_view(ui_view_t v);
static void select_view_force_redraw(void);

/* ---------------- shared context accessors (calendar_ui_internal.h) ---------------- */

app_settings_t *ui_get_cfg(void)
{
    return s_cfg;
}

bool ui_calendar_enabled(uint8_t calendar_index)
{
    if (s_cfg == NULL || calendar_index >= s_cfg->calendar_count) {
        return false;
    }
    return s_cfg->calendars[calendar_index].enabled;
}

void ui_switch_to_day(time_t day_start)
{
    s_cursor = day_start;
    select_view(UI_VIEW_DAY);
    select_view_force_redraw();
}

/* ---------------- nav rail ---------------- */

static const char *NAV_LABELS[4] = {"Month", "Week", "Day", "Up Next"};

static void nav_btn_event_cb(lv_event_t *e)
{
    ui_view_t v = (ui_view_t)(uintptr_t)lv_event_get_user_data(e);
    select_view(v);
    select_view_force_redraw();
}

static void today_btn_event_cb(lv_event_t *e)
{
    (void)e;
    s_cursor = ui_start_of_day(time(NULL));
    render_current_view();
    update_title();
}

/* ---------------- settings / reconfigure / OTA update ---------------- */

/* Short labels, not "Reconfigure"/"Check for Update" - four buttons in
 * one msgbox row have much less width each than the two-button dialogs
 * elsewhere in this file, and longer text overflowed them (same class of
 * issue as the earlier "Reset & Setup" button fix). */
static const char *s_settings_menu_btns[] = {"Display", "Setup", "Update", "Cancel", ""};
static const char *s_reconfigure_btns[] = {"Cancel", "Reset", ""};
static const char *s_ota_confirm_btns[] = {"Cancel", "Update", ""};

/* provisioning_clear() does a real NVS flash write, which needs ESP-IDF to
 * briefly disable the flash cache - and it asserts that whatever task is
 * doing that has its stack in internal RAM, not PSRAM, since PSRAM itself
 * is unreachable while the cache is off. The LVGL task's stack lives in
 * PSRAM (see lvgl_init() in board_bsp.c), and this whole flow starts from
 * an LVGL button click callback running on that task - calling
 * provisioning_clear() directly here crashed
 * (esp_task_stack_is_sane_cache_disabled() assertion) before the erase
 * ever completed, which is why "Reset" appeared to just reboot with the
 * config untouched. Doing the erase+restart on a small dedicated task
 * (default internal-RAM stack) avoids that entirely. */
static void reconfigure_task(void *arg)
{
    (void)arg;
    provisioning_clear();
    esp_restart();
}

static void reconfigure_confirm_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
    lv_msgbox_close(mbox);
    if (btn_id == 1) { /* "Reset" */
        xTaskCreate(reconfigure_task, "reconfigure", 4096, NULL, 5, NULL);
    }
}

/* esp_https_ota's flash write has the same internal-RAM-stack requirement
 * as provisioning_clear() above, hence its own dedicated task rather than
 * calling ota_update_check_and_apply() straight from the LVGL callback. */
static void ota_task(void *arg)
{
    (void)arg;
    esp_err_t err = ota_update_check_and_apply(s_cfg->ota_url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update applied, restarting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGW(TAG, "OTA update failed: %s", esp_err_to_name(err));
    if (bsp_lvgl_lock(2000)) {
        lv_obj_t *mbox = lv_msgbox_create(NULL, "Update failed", esp_err_to_name(err), NULL, true);
        lv_obj_center(mbox);
        bsp_lvgl_unlock();
    }
    vTaskDelete(NULL);
}

static void ota_confirm_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
    lv_msgbox_close(mbox);
    if (btn_id == 1) { /* "Update" */
        xTaskCreate(ota_task, "ota_update", 8192, NULL, 5, NULL);
    }
}

static void updated_label_cb(lv_event_t *e)
{
    (void)e;
    calendar_ui_request_sync();
}

static void settings_menu_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
    lv_msgbox_close(mbox);

    if (btn_id == 0) { /* "Display" */
        ui_settings_dialog_show();
    } else if (btn_id == 1) { /* "Setup" */
        lv_obj_t *confirm = lv_msgbox_create(NULL, "Reconfigure device?",
            "This erases the saved Wi-Fi and calendar settings and restarts "
            "into setup mode. You'll need to reconnect via the Wi-Fi portal "
            "to set it up again.",
            s_reconfigure_btns, false);
        lv_obj_set_width(confirm, 420);
        lv_obj_center(confirm);
        lv_obj_add_event_cb(confirm, reconfigure_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    } else if (btn_id == 2) { /* "Update" */
        if (s_cfg->ota_url[0] == '\0') {
            lv_obj_t *info = lv_msgbox_create(NULL, "No update URL set",
                "Set a firmware update URL from the Wi-Fi setup portal (gear "
                "icon -> Setup) first.", NULL, true);
            lv_obj_center(info);
            return;
        }
        lv_obj_t *confirm = lv_msgbox_create(NULL, "Check for update?",
            "Downloads and installs the firmware at the configured update "
            "URL, then restarts. There's no version check, so only do this "
            "when you know something new has actually been published there.",
            s_ota_confirm_btns, false);
        lv_obj_set_width(confirm, 420);
        lv_obj_center(confirm);
        lv_obj_add_event_cb(confirm, ota_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

/* Toggles the ambient clock feature on/off - see ui_screensaver.c's
 * s_clock_feature_enabled for what this actually changes. Runtime-only
 * (resets to enabled every boot), so the icon always starts fully opaque
 * (enabled) - no need to read the current state at build_top_bar() time.
 * The glyph itself (gcal_font_icon_clock's FontAwesome clock icon)
 * doesn't change - LVGL has no built-in "clock with a slash through it"
 * or similar disabled-clock glyph, so on/off is conveyed by dimming the
 * same icon instead, same idea as a greyed-out toolbar button
 * elsewhere. */
static void clock_toggle_btn_cb(lv_event_t *e)
{
    ui_screensaver_toggle_clock_enabled();
    lv_obj_t *icon = lv_event_get_target(e);
    lv_obj_set_style_text_opa(icon, ui_screensaver_clock_enabled() ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void settings_btn_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Settings", "", s_settings_menu_btns, false);
    lv_obj_set_width(mbox, 540);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, settings_menu_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void build_nav_rail(lv_obj_t *parent)
{
    lv_obj_t *rail = lv_obj_create(parent);
    lv_obj_remove_style_all(rail);
    lv_obj_set_pos(rail, 0, 0);
    lv_obj_set_size(rail, UI_NAV_RAIL_W, 480);
    lv_obj_set_style_bg_color(rail, ui_color(UI_COLOR_NAV_BG), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(rail, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_obj_create(rail);
        lv_obj_set_size(btn, UI_NAV_RAIL_W - 8, 60);
        lv_obj_set_pos(btn, 4, 12 + i * 66);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, nav_btn_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, NAV_LABELS[i]);
        lv_obj_set_style_text_font(lbl, &gcal_font_14, 0);
        lv_obj_center(lbl);

        s_nav_btns[i] = btn;
    }

    lv_obj_t *today_btn = lv_obj_create(rail);
    lv_obj_set_size(today_btn, UI_NAV_RAIL_W - 8, 44);
    lv_obj_align(today_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(today_btn, 8, 0);
    lv_obj_set_style_bg_color(today_btn, ui_color(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(today_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(today_btn, 0, 0);
    lv_obj_add_flag(today_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(today_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(today_btn, today_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *today_lbl = lv_label_create(today_btn);
    lv_label_set_text(today_lbl, "Today");
    lv_obj_set_style_text_color(today_lbl, lv_color_white(), 0);
    lv_obj_center(today_lbl);
}

static void refresh_nav_selection(void)
{
    for (int i = 0; i < 4; i++) {
        bool sel = ((int)s_view == i);
        lv_obj_set_style_bg_color(s_nav_btns[i], sel ? ui_color(UI_COLOR_NAV_SELECTED) : ui_color(UI_COLOR_NAV_BG), 0);
        lv_obj_set_style_bg_opa(s_nav_btns[i], LV_OPA_COVER, 0);
    }
}

/* ---------------- top bar ---------------- */

static void prev_btn_cb(lv_event_t *e)
{
    (void)e;
    switch (s_view) {
        case UI_VIEW_MONTH: s_cursor = ui_add_months(s_cursor, -1); break;
        case UI_VIEW_WEEK:  s_cursor = ui_add_days(s_cursor, -7); break;
        case UI_VIEW_DAY:   s_cursor = ui_add_days(s_cursor, -1); break;
        default: return;
    }
    render_current_view();
    update_title();
}

static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    switch (s_view) {
        case UI_VIEW_MONTH: s_cursor = ui_add_months(s_cursor, 1); break;
        case UI_VIEW_WEEK:  s_cursor = ui_add_days(s_cursor, 7); break;
        case UI_VIEW_DAY:   s_cursor = ui_add_days(s_cursor, 1); break;
        default: return;
    }
    render_current_view();
    update_title();
}

static void build_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, UI_NAV_RAIL_W, 0);
    lv_obj_set_size(bar, UI_CONTENT_W, UI_TOP_BAR_H);
    lv_obj_set_style_bg_color(bar, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev = lv_label_create(bar);
    lv_label_set_text(prev, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(prev, &gcal_font_20, 0);
    lv_obj_align(prev, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
    /* A single glyph's tight bounding box (~14x20px) is a tiny, easy-to-miss
     * touch target - extend the hit area well beyond the visible icon
     * without changing how it looks. */
    lv_obj_set_ext_click_area(prev, 20);
    lv_obj_add_event_cb(prev, prev_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next = lv_label_create(bar);
    lv_label_set_text(next, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(next, &gcal_font_20, 0);
    lv_obj_align(next, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(next, 20);
    lv_obj_add_event_cb(next, next_btn_cb, LV_EVENT_CLICKED, NULL);

    s_title_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_title_label, &gcal_font_20, 0);
    lv_obj_set_style_text_color(s_title_label, ui_color(UI_COLOR_TEXT), 0);
    /* Fixed width, not size-to-content: title text length varies a lot
     * between views ("August 2026" vs "Aug 17 - 23, 2026" vs a specific
     * day), and under the RGB panel's direct_mode/avoid_tearing setup an
     * auto-sized label whose bounding box shrinks left stale pixels from
     * the previous (wider) text behind - a fixed width means the
     * invalidated area is always the same rectangle regardless of text
     * length. 400px leaves clear room before the right-aligned "last
     * synced" label without ever overlapping it. */
    lv_obj_set_width(s_title_label, 400);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_title_label, LV_ALIGN_LEFT_MID, 90, 0);

    s_updated_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_updated_label, &gcal_font_14, 0);
    lv_obj_set_style_text_color(s_updated_label, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_align(s_updated_label, LV_ALIGN_RIGHT_MID, -12, 0);
    /* Tappable: forces an immediate sync instead of waiting for the next
     * periodic refresh. Small text label, so extend the hit area well
     * beyond its tight bounding box (same reasoning as the nav arrows). */
    lv_obj_add_flag(s_updated_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_updated_label, 16);
    lv_obj_add_event_cb(s_updated_label, updated_label_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *settings = lv_label_create(bar);
    lv_label_set_text(settings, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings, &gcal_font_20, 0);
    lv_obj_set_style_text_color(settings, ui_color(UI_COLOR_TEXT_MUTED), 0);
    /* Sits between the (400px-wide, starting at x=90) title and the
     * right-aligned "last synced" label - clear of both. */
    lv_obj_align(settings, LV_ALIGN_RIGHT_MID, -170, 0);
    lv_obj_add_flag(settings, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(settings, 16);
    lv_obj_add_event_cb(settings, settings_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Ambient-clock on/off toggle - a clock icon (gcal_font_icon_clock,
     * a standalone one-glyph font, see its own header comment for why
     * LVGL's built-in symbol set couldn't supply this one) that dims
     * rather than changes shape when the feature's off, since there's no
     * built-in "disabled clock" glyph to swap to. Sits just left of the
     * settings gear, same 40px rhythm as the gap between settings and the
     * "last synced" label. */
    lv_obj_t *clock_toggle = lv_label_create(bar);
    lv_label_set_text(clock_toggle, "\xEF\x80\x97" /* U+F017 FontAwesome "clock" */);
    lv_obj_set_style_text_font(clock_toggle, &gcal_font_icon_clock, 0);
    lv_obj_set_style_text_color(clock_toggle, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_align(clock_toggle, LV_ALIGN_RIGHT_MID, -210, 0);
    lv_obj_add_flag(clock_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(clock_toggle, 16);
    lv_obj_add_event_cb(clock_toggle, clock_toggle_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------- legend ---------------- */

static void legend_chip_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_cfg == NULL || idx >= s_cfg->calendar_count) {
        return;
    }
    s_cfg->calendars[idx].enabled = !s_cfg->calendars[idx].enabled;
    update_legend();
    render_current_view();
}

static void build_legend(lv_obj_t *parent)
{
    s_legend_row = lv_obj_create(parent);
    lv_obj_remove_style_all(s_legend_row);
    lv_obj_set_pos(s_legend_row, UI_NAV_RAIL_W, UI_TOP_BAR_H);
    lv_obj_set_size(s_legend_row, UI_CONTENT_W, UI_LEGEND_H);
    lv_obj_set_style_bg_color(s_legend_row, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_legend_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_legend_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_legend_row, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_set_style_border_width(s_legend_row, 1, 0);
    lv_obj_set_flex_flow(s_legend_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(s_legend_row, 12, 0);
    lv_obj_set_style_pad_column(s_legend_row, 14, 0);
    lv_obj_set_style_pad_top(s_legend_row, 6, 0);
    lv_obj_add_flag(s_legend_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_legend_row, LV_DIR_HOR);
}

static void update_legend(void)
{
    lv_obj_clean(s_legend_row);
    if (s_cfg == NULL) {
        return;
    }
    for (int i = 0; i < s_cfg->calendar_count; i++) {
        app_calendar_cfg_t *c = &s_cfg->calendars[i];
        lv_obj_t *chip = lv_obj_create(s_legend_row);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, 22);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(chip, 5, 0);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(chip, legend_chip_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_set_style_opa(chip, c->enabled ? LV_OPA_COVER : LV_OPA_40, 0);

        lv_obj_t *dot = lv_obj_create(chip);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, ui_color(c->color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_align(dot, LV_ALIGN_LEFT_MID);

        lv_obj_t *lbl = lv_label_create(chip);
        lv_label_set_text(lbl, c->label[0] ? c->label : c->id);
        lv_obj_set_style_text_font(lbl, &gcal_font_14, 0);
        lv_obj_set_style_text_color(lbl, ui_color(UI_COLOR_TEXT), 0);
    }
}

/* ---------------- view switching ---------------- */

static void render_current_view(void)
{
    switch (s_view) {
        case UI_VIEW_MONTH: ui_month_populate(s_view_roots[UI_VIEW_MONTH], s_cursor); break;
        case UI_VIEW_WEEK:  ui_week_populate(s_view_roots[UI_VIEW_WEEK], s_cursor); break;
        case UI_VIEW_DAY:   ui_day_populate(s_view_roots[UI_VIEW_DAY], s_cursor); break;
        case UI_VIEW_UPNEXT: ui_upnext_populate(s_view_roots[UI_VIEW_UPNEXT]); break;
    }
}

static void update_title(void)
{
    char buf[48] = {0};
    switch (s_view) {
        case UI_VIEW_MONTH: ui_month_title(s_cursor, buf, sizeof(buf)); break;
        case UI_VIEW_WEEK:  ui_week_title(s_cursor, buf, sizeof(buf)); break;
        case UI_VIEW_DAY:   ui_day_title(s_cursor, buf, sizeof(buf)); break;
        case UI_VIEW_UPNEXT: ui_upnext_title(buf, sizeof(buf)); break;
    }
    lv_label_set_text(s_title_label, buf);

    time_t last = event_store_last_refresh();
    char lbl[40];
    if (last == 0) {
        snprintf(lbl, sizeof(lbl), "%s%s",
                 s_sync_failed ? LV_SYMBOL_WARNING " " : "", "not yet synced");
    } else {
        struct tm tm;
        localtime_r(&last, &tm);
        char t[16];
        strftime(t, sizeof(t), "%H:%M", &tm);
        snprintf(lbl, sizeof(lbl), "%s%s %s",
                 s_sync_failed ? LV_SYMBOL_WARNING " " : "", "updated", t);
    }
    lv_label_set_text(s_updated_label, lbl);
    lv_obj_set_style_text_color(s_updated_label,
                                 s_sync_failed ? ui_color(UI_COLOR_WARNING) : ui_color(UI_COLOR_TEXT_MUTED), 0);

    /* This panel's direct_mode + avoid_tearing dual-framebuffer setup
     * means a single invalidate only guarantees ONE of the two buffers
     * gets this text change flushed to it - the other keeps showing the
     * previous "updated HH:MM" until something else forces a second full
     * redraw (same root cause as the screensaver wake fix in
     * ui_screensaver.c's wake_up() - see its comment for the long
     * version). Switching views happens to cause enough incidental
     * redraw activity to flush both buffers as a side effect, which is
     * why this looked like "the time only updates after switching to
     * month view" rather than a title-bar-specific bug - staying on the
     * same view after a sync never got that lucky flush.
     *
     * Only forced this way once LVGL's own redraw timer has ticked at
     * least once - see s_boot_forced_redraw_ok's comment. Before that
     * (calendar_ui_init()'s very first select_view()), a plain invalidate
     * is enough: nothing has been shown on screen yet at that point, so
     * there's no stale second buffer to catch up on, and the next regular
     * LVGL timer tick paints the very first frame correctly on its own. */
    lv_obj_invalidate(s_title_label);
    lv_obj_invalidate(s_updated_label);
    if (s_boot_forced_redraw_ok) {
        lv_refr_now(NULL);
        lv_obj_invalidate(s_title_label);
        lv_obj_invalidate(s_updated_label);
        lv_refr_now(NULL);
    }
}

/* Empties whichever view is about to stop being visible, per its own
 * ui_*_release() (see calendar_ui_internal.h) - only the ONE view the
 * user is actually looking at needs its event blocks/badges/chips
 * resident in RAM; the other three were otherwise left holding whatever
 * they'd last been populated with for as long as the app ran, purely
 * because nothing ever told them to let go once they were hidden. */
static void release_view(ui_view_t v)
{
    switch (v) {
        case UI_VIEW_MONTH: ui_month_release(); break;
        case UI_VIEW_WEEK:  ui_week_release(); break;
        case UI_VIEW_DAY:   ui_day_release(); break;
        case UI_VIEW_UPNEXT: ui_upnext_release(); break;
    }
}

static void select_view(ui_view_t v)
{
    if (v != s_view) {
        release_view(s_view);
    }
    s_view = v;
    for (int i = 0; i < 4; i++) {
        lv_obj_add_flag(s_view_roots[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_view_roots[v], LV_OBJ_FLAG_HIDDEN);
    refresh_nav_selection();
    render_current_view();
    update_title();
}

/* Full-screen double invalidate+refresh, same reasoning as
 * ui_screensaver.c's wake_up() (see its comment for the mechanism) -
 * hiding one view root and showing another, under this panel's
 * direct_mode + avoid_tearing dual-framebuffer setup, can leave the OTHER
 * buffer still showing the old view until something else forces a second
 * full redraw. Without this, a nav rail tap could leave a torn/stale
 * frame (old view mixed with new) on screen until some unrelated later
 * redraw happened to settle it.
 *
 * Call after select_view() from an interactive/already-running context
 * ONLY - NOT from calendar_ui_init()'s initial select_view() call. Doing
 * it there hung main_task forever (watchdog-reset-looping, confirmed
 * on-device 2026-09-05): this forces lv_refr_now() to run synchronously
 * before LVGL's own redraw timer has ever ticked once, and something
 * about that very first frame under this panel's dual-buffer setup never
 * satisfies whatever refr_sync_areas()/lv_draw_sw_buffer_copy() was
 * waiting on. */
static void select_view_force_redraw(void)
{
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

/* ---------------- public API ---------------- */

void calendar_ui_init(app_settings_t *cfg)
{
    s_cfg = cfg;
    s_cursor = ui_start_of_day(time(NULL));

    /* Holds the LVGL lock for this entire initial build, not just the
     * later per-call updates that already take it (calendar_ui_refresh()
     * etc., below) - board_bsp.c's lvgl_init() (called before this, from
     * bsp_display_init()) has already started esp_lvgl_port's own
     * background task, which independently calls lv_timer_handler() on a
     * loop from the moment it's created, including LVGL's own periodic
     * layout/redraw pass over whatever object tree exists at that
     * instant. Without this lock, that concurrently-running task and
     * this function - running unlocked, from app_main()'s own task,
     * building the ENTIRE UI (nav rail, top bar, legend, all four views,
     * the clock overlay) - could race: LVGL's own task walking a
     * half-built object whose children or style list isn't linked up
     * yet. Confirmed (2026-09-09) as the real cause of a recurring
     * LoadProhibited crash inside LVGL's layout pass at boot - a
     * different leaf function each time, matching exactly whichever
     * object happened to be mid-construction at the moment the race
     * hit. Heap poisoning (briefly enabled to chase this) never caught a
     * heap-corruption event before it, and it kept recurring even after
     * fixing a real, separate LVGL foot-gun in ui_settings_dialog.c
     * (synchronous lv_obj_del() from inside an object's own descendant's
     * click handler) - both pointed away from corruption/stale-pointer
     * theories and toward this unsynchronized-construction race instead,
     * since this always happened well before any dialog could even have
     * been touched. bsp_lvgl_lock() IS esp_lvgl_port's own mutex
     * (lvgl_port_lock() - see board_bsp.c), and the port task's own
     * lv_timer_handler() call already takes it non-blockingly
     * (lvgl_port_lock(0)) each cycle, so holding it here for the whole
     * build just makes that task skip a few cycles and retry - no
     * deadlock risk. */
    if (!bsp_lvgl_lock(5000)) {
        ESP_LOGE(TAG, "could not get LVGL lock for initial UI build - giving up");
        return;
    }

    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, ui_color(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    build_nav_rail(s_screen);
    build_top_bar(s_screen);
    build_legend(s_screen);

    s_view_roots[UI_VIEW_MONTH] = ui_month_create(s_screen);
    s_view_roots[UI_VIEW_WEEK] = ui_week_create(s_screen);
    s_view_roots[UI_VIEW_DAY] = ui_day_create(s_screen);
    s_view_roots[UI_VIEW_UPNEXT] = ui_upnext_create(s_screen);

    update_legend();
    select_view(UI_VIEW_MONTH);
    s_boot_forced_redraw_ok = true;
    ui_screensaver_init();
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "UI ready");
}

void calendar_ui_refresh(void)
{
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGW(TAG, "could not get LVGL lock for refresh, skipping this cycle");
        return;
    }
    s_sync_failed = false;
    update_legend();
    render_current_view();
    update_title();
    bsp_lvgl_unlock();
}

void calendar_ui_notify_sync_failed(void)
{
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGW(TAG, "could not get LVGL lock for sync-failure notice, skipping");
        return;
    }
    s_sync_failed = true;
    update_title();
    bsp_lvgl_unlock();
}

void calendar_ui_sync_today(void)
{
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGW(TAG, "could not get LVGL lock for today-sync, skipping");
        return;
    }
    s_cursor = ui_start_of_day(time(NULL));
    render_current_view();
    update_title();
    bsp_lvgl_unlock();
}

void calendar_ui_release_active_view(void)
{
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGW(TAG, "could not get LVGL lock to release view for sleep, skipping");
        return;
    }
    release_view(s_view);
    bsp_lvgl_unlock();
}

void calendar_ui_restore_active_view(void)
{
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGW(TAG, "could not get LVGL lock to restore view after wake, skipping");
        return;
    }
    render_current_view();
    bsp_lvgl_unlock();
}

void calendar_ui_reset_to_today_month(void)
{
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGW(TAG, "could not get LVGL lock to reset to today/month after long sleep, skipping");
        return;
    }
    s_cursor = ui_start_of_day(time(NULL));
    select_view(UI_VIEW_MONTH); /* release_view()s the old view itself if
                                    it's not already Month - safe even
                                    though calendar_ui_release_active_view()
                                    already released it before sleep, since
                                    ui_*_release() is idempotent (lv_obj_clean()
                                    on an already-empty container). */
    select_view_force_redraw();
    bsp_lvgl_unlock();
}
