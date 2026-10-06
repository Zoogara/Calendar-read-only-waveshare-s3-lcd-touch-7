/* "On this day in history" list - opened by tapping the date on the ambient
 * clock (ui_clock.c), filled from components/onthisday (Wikipedia).
 *
 * A full-screen overlay on lv_layer_top() above the clock, scrollable, built
 * when opened and deleted when closed - its labels (~17 entries x 2) cost
 * internal RAM only while it's showing. A tap on it closes it (a drag
 * scrolls instead - LVGL doesn't send a click after a scroll), and
 * ui_screensaver.c closes it after HISTORY_IDLE_CLOSE_MS untouched; either
 * way it's back to the clock, never the calendar. While it's open,
 * ui_screensaver.c treats touches as the overlay's own (see
 * ui_overlay_is_open()). */
#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "onthisday.h"

#include <stdio.h>
#include <time.h>
#include "esp_log.h"

#define BG_COLOR     0x000000
#define HEAD_COLOR   0xE8EAED
#define YEAR_COLOR   0x8AB4F8
#define TEXT_COLOR   0xBDC1C6

static const char *TAG = "ui_history";

static const char *MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

static lv_obj_t *s_overlay;

/* Same double invalidate + refresh as ui_lock.c / ui_settings_dialog.c
 * after deleting a full-screen lv_layer_top() object (direct_mode dual
 * framebuffers). */
static void delete_overlay(void)
{
    if (s_overlay == NULL) {
        return;
    }
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static void close_async(void *arg)
{
    (void)arg;
    delete_overlay();
    ui_screensaver_keypad_dismissed();   /* the closing tap isn't a new touch on the clock */
    ESP_LOGI(TAG, "closed");
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    lv_async_call(close_async, NULL);
}

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    /* Taps on the text must close the overlay too, so let them through to
     * the scrolling list. */
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

bool ui_history_is_open(void)
{
    return s_overlay != NULL;
}

void ui_history_open(void)
{
    if (s_overlay != NULL) {
        return;
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *heading = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(heading, &gcal_font_20, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(HEAD_COLOR), 0);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 32, 20);

    lv_obj_t *list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_HOR_RES - 64, LV_VER_RES - 72);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 32, 60);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, tap_cb, LV_EVENT_CLICKED, NULL);

    const onthisday_t *otd = onthisday_acquire();
    if (otd == NULL || otd->count == 0) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        lv_label_set_text_fmt(heading, "On this day \xE2\x80\x93 %d %s", lt.tm_mday, MONTHS[lt.tm_mon]);
        add_label(list, &gcal_font_14, TEXT_COLOR,
                  "Today's history hasn't been fetched yet - it's loaded from "
                  "Wikipedia after the next calendar sync.");
    } else {
        lv_label_set_text_fmt(heading, "On this day \xE2\x80\x93 %d %s", otd->day, MONTHS[(otd->month - 1) % 12]);
        char line[120];
        for (int i = 0; i < otd->count; i++) {
            const onthisday_entry_t *e = &otd->entries[i];
            /* "1813 – Battle of the Thames" (en dash, U+2013). */
            if (e->year < 0) {
                snprintf(line, sizeof(line), "%d BC \xE2\x80\x93 %s", -e->year, e->title);
            } else {
                snprintf(line, sizeof(line), "%d \xE2\x80\x93 %s", e->year, e->title);
            }
            lv_obj_t *head = add_label(list, &gcal_font_20, YEAR_COLOR, line);
            if (i > 0) {
                lv_obj_set_style_pad_top(head, 12, 0);
            }
            add_label(list, &gcal_font_14, TEXT_COLOR, e->text);
        }
    }
    onthisday_release();

    /* The opening tap's release mustn't land on the list and close it again. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    ESP_LOGI(TAG, "open");
}

void ui_history_close(void)
{
    if (s_overlay != NULL) {
        delete_overlay();
        ESP_LOGI(TAG, "closed (idle)");
    }
}
