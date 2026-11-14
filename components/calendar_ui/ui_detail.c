/*
 * The event details card: touching an event in the Week, Day or Up next view opens it, showing the
 * title, when, which calendar, and the location and description if the event has them. Month view
 * isn't involved (a tap there opens that day, as before).
 *
 * Built to be cheap and safe on internal RAM, which is this firmware's scarcest resource (see the notes
 * in ui_weather.c and ui_reminder.c):
 *  - ONE card, built once at startup on lv_layer_top() and then only hidden and shown, like the
 *    reminder card - so a tap allocates nothing. (LVGL's malloc assert reboots the device on a failed
 *    allocation, so building a pop-up on demand, right when a sync may have taken the memory, is the
 *    risky way to do this. It also avoids churning the heap, which fragments over days.)
 *  - Its text sits in PSRAM buffers shown with lv_label_set_text_static().
 *  - Events are not given a callback each. A view tags each block, chip or item (ui_detail_tag()): the
 *    event is copied into a PSRAM table and its index kept in the object's user_data; the object bubbles
 *    its CLICKED event to a container that has the one shared handler. No per-event allocation.
 *  - The tagged copies are the event as it was when the view was drawn, so a sync landing afterwards
 *    can't change what a tap shows; the location and description are read from the event store's pool at
 *    tap time and are refused if that pool has been replaced (see event_store_detail_get()).
 *  - While the card is open the network task holds its fetches (calendar_ui_panel_open()), and it
 *    closes after a minute without a touch like the other panels.
 */
#include "calendar_ui_internal.h"
#include "event_store.h"
#include "time_fmt.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ui_detail";

#define DETAIL_MAX_TAGGED 96           /* events tagged per drawn view (a view draws at most 64) */
#define CARD_W            560
#define PAD_LEFT          22           /* the colour stripe is 8px; text starts after it */
#define PAD_RIGHT         16
#define TEXT_W            (CARD_W - PAD_LEFT - PAD_RIGHT - 36)   /* leaves room for the close mark */
#define DESC_MAX_LINES    7
#define MIN_FREE_BLOCK    3072         /* don't show if internal RAM has less than this free in one block */

static gcal_event_t *s_tab;            /* the tagged events (PSRAM) */
static int s_n;

static lv_obj_t *s_card;
static lv_obj_t *s_title, *s_when, *s_cal, *s_loc, *s_desc;
static bool s_open;

/* Text buffers for the labels (one PSRAM block). */
static char *s_title_buf, *s_when_buf, *s_cal_buf, *s_loc_buf, *s_desc_buf;
#define TITLE_SZ GCAL_MAX_SUMMARY
#define WHEN_SZ  96
#define CAL_SZ   64
#define LOC_SZ   (GCAL_MAX_LOCATION + 16)
#define DESC_SZ  GCAL_MAX_DESCRIPTION

static void card_clicked_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    ui_detail_close();
}

static lv_obj_t *make_label(const lv_font_t *font, uint32_t color, int width)
{
    lv_obj_t *l = lv_label_create(s_card);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void ui_detail_init(void)
{
    if (s_card != NULL) {
        return;
    }
    char *bufs = heap_caps_malloc(TITLE_SZ + WHEN_SZ + CAL_SZ + LOC_SZ + DESC_SZ, MALLOC_CAP_SPIRAM);
    s_tab = heap_caps_malloc(DETAIL_MAX_TAGGED * sizeof(gcal_event_t), MALLOC_CAP_SPIRAM);
    if (bufs == NULL || s_tab == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the details card - events won't open it");
        heap_caps_free(bufs);
        heap_caps_free(s_tab);
        s_tab = NULL;
        return;
    }
    s_title_buf = bufs;
    s_when_buf = s_title_buf + TITLE_SZ;
    s_cal_buf = s_when_buf + WHEN_SZ;
    s_loc_buf = s_cal_buf + CAL_SZ;
    s_desc_buf = s_loc_buf + LOC_SZ;
    s_title_buf[0] = s_when_buf[0] = s_cal_buf[0] = s_loc_buf[0] = s_desc_buf[0] = '\0';

    s_card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_card);
    lv_obj_set_width(s_card, CARD_W);
    lv_obj_set_height(s_card, LV_SIZE_CONTENT);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 14);
    lv_obj_set_style_bg_color(s_card, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 10, 0);
    /* A thin grey outline: the card is white on a mostly white calendar, so without it only the stripe and the
     * grid lines stopping would show where it ends. (An outline is just style properties on this one object;
     * a shadow would allocate and redraw a lot, so there isn't one.) */
    lv_obj_set_style_outline_width(s_card, 1, 0);
    lv_obj_set_style_outline_color(s_card, lv_color_hex(0xB4B8BD), 0);
    lv_obj_set_style_outline_opa(s_card, LV_OPA_COVER, 0);
    /* A thick stripe down the left in the event's calendar colour. */
    lv_obj_set_style_border_side(s_card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_card, 8, 0);
    lv_obj_set_style_pad_left(s_card, PAD_LEFT - 8, 0);
    lv_obj_set_style_pad_right(s_card, PAD_RIGHT, 0);
    lv_obj_set_style_pad_top(s_card, 14, 0);
    lv_obj_set_style_pad_bottom(s_card, 14, 0);
    lv_obj_set_style_pad_row(s_card, 6, 0);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card, card_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_title = make_label(&gcal_font_20, UI_COLOR_TEXT, TEXT_W);
    s_when = make_label(&gcal_font_14, UI_COLOR_TEXT_MUTED, TEXT_W);
    s_cal = make_label(&gcal_font_14, UI_COLOR_TEXT_MUTED, TEXT_W);
    s_loc = make_label(&gcal_font_14, UI_COLOR_TEXT, TEXT_W);
    s_desc = make_label(&gcal_font_14, UI_COLOR_TEXT, TEXT_W);

    /* The close mark, out of the flex flow, top right. */
    lv_obj_t *close = lv_label_create(s_card);
    lv_label_set_text(close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close, &gcal_font_20, 0);
    lv_obj_set_style_text_color(close, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_add_flag(close, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -4, -2);
    lv_obj_clear_flag(close, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

void ui_detail_reset(void)
{
    ui_detail_close();     /* a view is being redrawn: whatever the card shows may be gone */
    s_n = 0;
}

void ui_detail_tag(lv_obj_t *obj, const gcal_event_t *ev)
{
    if (s_tab == NULL || obj == NULL || ev == NULL || s_n >= DETAIL_MAX_TAGGED) {
        return;
    }
    s_tab[s_n] = *ev;
    s_n++;
    lv_obj_set_user_data(obj, (void *)(uintptr_t)s_n);          /* index + 1: 0 means "not tagged" */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void set_text(lv_obj_t *label, char *buf, bool show)
{
    if (show) {
        lv_label_set_text_static(label, buf);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool show_index(int i)
{
    if (s_card == NULL || s_tab == NULL || i < 0 || i >= s_n) {
        return false;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < MIN_FREE_BLOCK) {
        /* Not worth risking an LVGL allocation failure (which reboots): skip this tap. */
        ESP_LOGW(TAG, "not showing details: largest internal block is only %u bytes", (unsigned)largest);
        return false;
    }
    const gcal_event_t *ev = &s_tab[i];
    const app_settings_t *cfg = ui_get_cfg();

    snprintf(s_title_buf, TITLE_SZ, "%s", ev->summary);
    tfmt_event_when(cfg->clock_24h, ev->start, ev->end, ev->all_day, s_when_buf, WHEN_SZ);
    snprintf(s_cal_buf, CAL_SZ, "Calendar: %s", ev->calendar_label);

    char loc[GCAL_MAX_LOCATION];
    bool have = event_store_detail_get(ev->detail, ev->detail_gen, loc, sizeof(loc), s_desc_buf, DESC_SZ);
    bool has_loc = have && loc[0] != '\0';
    bool has_desc = have && s_desc_buf[0] != '\0';
    snprintf(s_loc_buf, LOC_SZ, "Location: %s", loc);

    set_text(s_title, s_title_buf, true);
    set_text(s_when, s_when_buf, true);
    set_text(s_cal, s_cal_buf, true);
    set_text(s_loc, s_loc_buf, has_loc);
    set_text(s_desc, s_desc_buf, has_desc);
    if (has_desc) {
        /* Whole lines only, and not more than DESC_MAX_LINES: the rest of a long description is cut off. */
        const lv_font_t *f = &gcal_font_14;
        lv_point_t sz;
        lv_txt_get_size(&sz, s_desc_buf, f, 0, 0, TEXT_W, LV_TEXT_FLAG_NONE);
        int line = lv_font_get_line_height(f);
        int max_h = DESC_MAX_LINES * line;
        lv_obj_set_height(s_desc, sz.y > max_h ? max_h : sz.y);
    }
    lv_obj_set_style_border_color(s_card, ui_color(ev->color), 0);

    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_card);
    s_open = true;
    ESP_LOGI(TAG, "details shown (internal %u free / %u largest)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)largest);

    /* The tap that opened this must not also land on the card as it appears. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    return true;
}

bool ui_detail_show_index(int i)
{
    return show_index(i);
}

bool ui_detail_try_show(lv_obj_t *target)
{
    if (target == NULL) {
        return false;
    }
    int idx = (int)(uintptr_t)lv_obj_get_user_data(target);     /* 0 = not tagged */
    return idx > 0 && show_index(idx - 1);
}

void ui_detail_click_cb(lv_event_t *e)
{
    ui_detail_try_show(lv_event_get_target(e));
}

void ui_detail_close(void)
{
    if (s_card != NULL && s_open) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    s_open = false;
}

bool ui_detail_is_open(void)
{
    return s_open;
}
