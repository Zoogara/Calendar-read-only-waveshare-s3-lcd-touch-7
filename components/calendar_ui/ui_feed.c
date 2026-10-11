/* News feed list - opened by tapping one of the two feed icons in the bottom
 * corners of the ambient clock (ui_clock.c), filled from components/rss_feed.
 *
 * Same shape as ui_history.c: a full-screen scrollable overlay on
 * lv_layer_top(), built when opened and deleted when closed (so its labels
 * cost internal RAM only while it's showing); a tap closes it (a drag scrolls
 * instead), ui_screensaver.c closes it after PANEL_IDLE_CLOSE_MS untouched,
 * and either way it's back to the clock, never the calendar. Headings and
 * plain text only - no images. */
#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "rss_feed.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#define BG_COLOR     0x000000
#define HEAD_COLOR   0xE8EAED
#define TITLE_COLOR  0x8AB4F8
#define TEXT_COLOR   0xBDC1C6

static const char *TAG = "ui_feed";

static lv_obj_t *s_overlay;

/* Same double invalidate + refresh as ui_history.c after deleting a
 * full-screen lv_layer_top() object (direct_mode dual framebuffers). */
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
    /* Taps on the text must close the overlay too. */
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

bool ui_feed_is_open(void)
{
    return s_overlay != NULL;
}

void ui_feed_open(int idx)
{
    if (s_overlay != NULL) {
        return;
    }
    app_settings_t *cfg = ui_get_cfg();
    if (idx < 0 || idx >= RSS_FEED_COUNT) {
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
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_set_width(heading, LV_HOR_RES - 64);
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

    const rss_feed_t *feed = rss_feed_acquire(idx);
    /* The label set on the config page wins; otherwise the feed's own title. */
    const char *name = cfg->feed_label[idx][0] ? cfg->feed_label[idx]
                       : (feed && feed->name[0]) ? feed->name : "News";
    if (feed != NULL && feed->name[0] && cfg->feed_label[idx][0] &&
        strcmp(feed->name, cfg->feed_label[idx]) != 0) {
        lv_label_set_text_fmt(heading, "%s \xE2\x80\x93 %s", cfg->feed_label[idx], feed->name);
    } else {
        lv_label_set_text(heading, name);
    }
    if (feed == NULL || feed->count == 0) {
        add_label(list, &gcal_font_14, TEXT_COLOR,
                  "Nothing fetched yet. Feeds load in the background after the next "
                  "calendar sync and then about every 20 minutes - if this stays "
                  "empty, check the feed's address on the settings page.");
    } else {
        for (int i = 0; i < feed->count; i++) {
            const rss_item_t *it = &feed->items[i];
            lv_obj_t *head = add_label(list, &gcal_font_20, TITLE_COLOR, it->title);
            if (i > 0) {
                lv_obj_set_style_pad_top(head, 12, 0);
            }
            if (it->text[0]) {
                add_label(list, &gcal_font_14, TEXT_COLOR, it->text);
            }
        }
    }
    rss_feed_release();

    /* The opening tap's release mustn't land on the list and close it again. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    ESP_LOGI(TAG, "open feed %d", idx);
}

void ui_feed_close(void)
{
    if (s_overlay != NULL) {
        delete_overlay();
        ESP_LOGI(TAG, "closed (idle)");
    }
}
