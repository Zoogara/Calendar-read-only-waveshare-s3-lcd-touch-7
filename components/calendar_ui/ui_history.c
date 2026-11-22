/* "On this day in history" list - opened by tapping the date on the ambient
 * clock (ui_clock.c), filled from components/onthisday (Wikipedia).
 *
 * A full-screen overlay on lv_layer_top() above the clock, scrollable, built
 * when opened and deleted when closed - its labels (~17 entries x 2) cost
 * internal RAM only while it's showing. Touching an entry opens the link card
 * (ui_link.c: a QR code of the article to scan with a phone, and Save for
 * later); the close button at the top right, a tap on the heading or the empty
 * background, or ui_screensaver.c after HISTORY_IDLE_CLOSE_MS untouched, closes
 * the list (a drag scrolls instead - LVGL doesn't send a click after a
 * scroll); either way it's back to the clock, never the calendar. Each entry's
 * labels carry its position in their user_data (+1; -1 = no link, so a tap does
 * nothing) and bubble to the list's one handler. While it's open,
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

/* "1813 - Battle of the Thames" (en dash), the same line the list shows for an entry. */
static void entry_heading(const onthisday_entry_t *e, char *out, size_t sz)
{
    if (e->year < 0) {
        snprintf(out, sz, "%d BC \xE2\x80\x93 %s", -e->year, e->title);
    } else {
        snprintf(out, sz, "%d \xE2\x80\x93 %s", e->year, e->title);
    }
}

/* One handler for the whole list: a tagged entry opens its link card, an untagged tap (the background) closes the list. */
static void list_tap_cb(lv_event_t *e)
{
    intptr_t tag = (intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (tag < 0) {
        return;                                   /* an entry with no link */
    }
    if (tag > 0) {
        const onthisday_t *otd = onthisday_acquire();
        if (otd != NULL && tag - 1 < otd->count) {
            const onthisday_entry_t *en = &otd->entries[tag - 1];
            char title[120];
            entry_heading(en, title, sizeof(title));
            ui_link_show(en->title[0] ? title : en->text, en->url);   /* copies them */
        }
        onthisday_release();
        return;
    }
    tap_cb(e);
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

    /* The way out: a close button, always (a tap on an entry opens its link card now, so "tap anywhere" is no longer it). */
    lv_obj_t *close_btn = lv_label_create(s_overlay);
    lv_label_set_text(close_btn, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_btn, &gcal_font_20, 0);
    lv_obj_set_style_text_color(close_btn, lv_color_hex(HEAD_COLOR), 0);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -28, 20);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close_btn, 28);
    lv_obj_add_event_cb(close_btn, tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *list = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_HOR_RES - 64, LV_VER_RES - 72);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 32, 60);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, list_tap_cb, LV_EVENT_CLICKED, NULL);

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
        bool any_link = false;
        for (int i = 0; i < otd->count; i++) {
            const onthisday_entry_t *e = &otd->entries[i];
            void *tag = (void *)(intptr_t)(e->url[0] ? i + 1 : -1);
            any_link = any_link || e->url[0];
            entry_heading(e, line, sizeof(line));
            lv_obj_t *head = add_label(list, &gcal_font_20, YEAR_COLOR, line);
            lv_obj_set_user_data(head, tag);
            lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);     /* a label isn't clickable by default: a touch would go to the list */
            if (i > 0) {
                lv_obj_set_style_pad_top(head, 12, 0);
            }
            lv_obj_t *body = add_label(list, &gcal_font_14, TEXT_COLOR, e->text);
            lv_obj_set_user_data(body, tag);
            lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
        }
        if (any_link) {
            lv_obj_t *hint = lv_label_create(s_overlay);
            lv_label_set_text(hint, "Tap an entry for its link");
            lv_obj_set_style_text_font(hint, &gcal_font_14, 0);
            lv_obj_set_style_text_color(hint, lv_color_hex(TEXT_COLOR), 0);
            lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -88, 24);
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
