/* The link card: tapping an item on the news-feed or "On this day" page opens it. It shows the item's address as a QR
 * code - scan it with a phone and the article opens there - with the title, the address in small type, and two
 * buttons: Save for later (adds it to the read-later list, shown on the config page's /links) and Close.
 *
 * Built once at startup on lv_layer_top() and then only hidden and shown, like the event details card (ui_detail.c),
 * for the same reason: a tap allocates nothing (LVGL's malloc assert reboots the device if an allocation fails, and
 * a tap is exactly when a sync may have taken the memory). The text is shown with lv_label_set_text_static() from
 * buffers kept here, and the QR code is drawn straight from the encoder's module grid by one custom-draw object -
 * no object per module, no canvas. The encoder is Nayuki's QR Code generator, which LVGL bundles; its two work
 * buffers (408 bytes each at the largest size used) live in PSRAM.
 *
 * Version 10 (57 x 57 modules) at medium error correction holds 213 bytes, comfortably more than the 199 bytes a
 * link here can have, and at 4 px a module (the smallest used) it is 260 px across on this 800x480 panel - easily
 * read by a phone camera. A link that can't be encoded shows a message instead and can still be saved.
 *
 * While the card is open calendar_ui_panel_open() is true (syncs wait), ui_screensaver.c closes it after a minute
 * untouched or when the display goes to the clock or sleep, and a tap outside the card closes it. Closing returns to
 * the list underneath; the list's own close button then goes back to the clock. */
#include "calendar_ui_internal.h"
#include "readlater.h"
#include "ui_theme.h"
#include "src/extra/libs/qrcode/qrcodegen.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ui_link";

#define QR_MAX_VERSION 10
#define QR_BUF_LEN     qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)
#define QR_BOX         300                 /* the square the code is centred in, quiet zone included */
#define QUIET_MODULES  4

#define CARD_W         640
#define CARD_H         400
#define CARD_BG        0x202124
#define HEAD_COLOR     0xE8EAED
#define TEXT_COLOR     0xBDC1C6
#define OK_COLOR       0x81C995
#define WARN_COLOR     0xF9AB00

static uint8_t *s_qr, *s_tmp;              /* PSRAM, QR_BUF_LEN each */
static int s_qr_size;                      /* modules per side; 0 = no code */

static char s_title[RL_TITLE_LEN];
static char s_url[LINK_URL_MAX];
static char s_status[64];
static char s_save_text[24];

static lv_obj_t *s_scrim, *s_card, *s_qr_obj, *s_title_lbl, *s_url_lbl, *s_status_lbl, *s_save_btn, *s_save_lbl;
static bool s_open;
static bool s_saved;

/* ---- the code, drawn from the module grid ---- */
static void qr_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 0;
    d.border_width = 0;
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = lv_color_white();
    lv_draw_rect(ctx, &d, &box);                           /* the white square, quiet zone included */
    if (s_qr_size <= 0) {
        return;
    }
    int px = QR_BOX / (s_qr_size + 2 * QUIET_MODULES);
    if (px > 8) px = 8;
    if (px < 1) px = 1;
    const int span = px * s_qr_size;
    const int x0 = box.x1 + (lv_area_get_width(&box) - span) / 2;
    const int y0 = box.y1 + (lv_area_get_height(&box) - span) / 2;
    d.bg_color = lv_color_black();
    for (int y = 0; y < s_qr_size; y++) {
        int x = 0;
        while (x < s_qr_size) {
            if (!qrcodegen_getModule(s_qr, x, y)) {
                x++;
                continue;
            }
            int start = x;
            while (x < s_qr_size && qrcodegen_getModule(s_qr, x, y)) {
                x++;                                       /* one rectangle per run of dark modules in a row */
            }
            lv_area_t r = {.x1 = x0 + start * px, .y1 = y0 + y * px, .x2 = x0 + x * px - 1, .y2 = y0 + (y + 1) * px - 1};
            lv_draw_rect(ctx, &d, &r);
        }
    }
}

/* ---- closing ---- */
static void hide_card(void)
{
    if (!s_open) {
        return;
    }
    s_open = false;
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    /* a full-screen object going away: the same double invalidate + refresh the other overlays do, because of the
     * dual frame buffers (see ui_history.c's delete_overlay()) */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static void close_async(void *arg)
{
    (void)arg;
    hide_card();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    lv_async_call(close_async, NULL);
}

/* A tap on the card itself does nothing - only the scrim around it (and the buttons) close or save. */
static void card_tap_cb(lv_event_t *e)
{
    (void)e;
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (s_saved) {
        return;
    }
    const app_settings_t *cfg = ui_get_cfg();
    uint16_t days = cfg->read_later_days ? cfg->read_later_days : APP_SETTINGS_DEFAULT_READ_LATER_DAYS;
    rl_add_result_t r = readlater_add(s_title, s_url, days);
    uint32_t colour = OK_COLOR;
    switch (r) {
    case RL_ADDED:
        snprintf(s_status, sizeof(s_status), "Saved - kept for %u day%s", (unsigned)days, days == 1 ? "" : "s");
        break;
    case RL_ADDED_DROPPED_OLDEST:
        snprintf(s_status, sizeof(s_status), "Saved - the list was full, the oldest went");
        break;
    case RL_ALREADY:
        snprintf(s_status, sizeof(s_status), "Already saved - kept for %u more day%s", (unsigned)days, days == 1 ? "" : "s");
        break;
    default:
        snprintf(s_status, sizeof(s_status), "Couldn't save that link");
        colour = WARN_COLOR;
        break;
    }
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(colour), 0);
    lv_label_set_text_static(s_status_lbl, s_status);
    if (r != RL_BAD_URL) {
        s_saved = true;
        snprintf(s_save_text, sizeof(s_save_text), LV_SYMBOL_OK " Saved");
        lv_label_set_text_static(s_save_lbl, s_save_text);
        lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
    }
    ESP_LOGI(TAG, "save: result %d (%u days)", (int)r, (unsigned)days);
}

/* ---- building ---- */
static lv_obj_t *add_button(lv_obj_t *parent, const char *text, uint32_t colour, int x, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 130, 44);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, x, -20);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &gcal_font_14, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    if (label_out != NULL) {
        *label_out = l;
    }
    return b;
}

void ui_link_init(void)
{
    s_qr = heap_caps_malloc(QR_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tmp = heap_caps_malloc(QR_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_qr == NULL || s_tmp == NULL) {
        ESP_LOGE(TAG, "no memory for the QR buffers - link card disabled");
        heap_caps_free(s_qr);
        heap_caps_free(s_tmp);
        s_qr = s_tmp = NULL;
        return;
    }

    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_70, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scrim, close_cb, LV_EVENT_CLICKED, NULL);

    s_card = lv_obj_create(s_scrim);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 10, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0xB4B8BD), 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_card, card_tap_cb, LV_EVENT_CLICKED, NULL);

    s_qr_obj = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_qr_obj);
    lv_obj_set_size(s_qr_obj, QR_BOX, QR_BOX);
    lv_obj_align(s_qr_obj, LV_ALIGN_TOP_LEFT, 20, 24);
    lv_obj_clear_flag(s_qr_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_qr_obj, qr_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    const int tx = 20 + QR_BOX + 20, tw = CARD_W - tx - 20;
    /* title and address in a column, so the address sits right under however many lines the title takes (the longest
     * titles are cut at the column's height) */
    lv_obj_t *col = lv_obj_create(s_card);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, tw, 5 * 24 + 10 + 4 * 17);
    lv_obj_align(col, LV_ALIGN_TOP_LEFT, tx, 24);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_title_lbl = lv_label_create(col);
    lv_obj_set_width(s_title_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(s_title_lbl, 5 * 24, 0);
    lv_obj_set_style_text_font(s_title_lbl, &gcal_font_20, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(HEAD_COLOR), 0);

    s_url_lbl = lv_label_create(col);
    lv_obj_set_width(s_url_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(s_url_lbl, 4 * 17, 0);
    lv_obj_set_style_text_font(s_url_lbl, &gcal_font_14, 0);
    lv_obj_set_style_text_color(s_url_lbl, lv_color_hex(TEXT_COLOR), 0);

    s_status_lbl = lv_label_create(s_card);
    lv_obj_set_width(s_status_lbl, tw);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_status_lbl, &gcal_font_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(OK_COLOR), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_LEFT, tx, -78);

    s_save_btn = add_button(s_card, "Save for later", 0x1A73E8, tx, save_cb, &s_save_lbl);
    add_button(s_card, "Close", 0x3C4043, tx + 130 + 12, close_cb, NULL);

    lv_obj_t *hint = lv_label_create(s_card);
    lv_label_set_text(hint, "Scan with a phone camera");
    lv_obj_set_style_text_font(hint, &gcal_font_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(TEXT_COLOR), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 20 + (QR_BOX - 150) / 2, -28);

    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
}

bool ui_link_is_open(void)
{
    return s_open;
}

void ui_link_show(const char *title, const char *url)
{
    if (s_scrim == NULL || url == NULL || !link_url_ok(url)) {
        return;                                            /* disabled (no memory), or not a plain web address */
    }
    snprintf(s_title, sizeof(s_title), "%s", title && title[0] ? title : url);
    snprintf(s_url, sizeof(s_url), "%s", url);
    s_saved = false;
    s_status[0] = '\0';
    snprintf(s_save_text, sizeof(s_save_text), "Save for later");
    lv_obj_clear_state(s_save_btn, LV_STATE_DISABLED);

    bool ok = qrcodegen_encodeText(s_url, s_tmp, s_qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                                   qrcodegen_Mask_AUTO, true);
    s_qr_size = ok ? qrcodegen_getSize(s_qr) : 0;
    if (!ok) {
        snprintf(s_status, sizeof(s_status), "Too long to show as a code - you can still save it");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(WARN_COLOR), 0);
    } else {
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(OK_COLOR), 0);
    }
    lv_label_set_text_static(s_title_lbl, s_title);
    lv_label_set_text_static(s_url_lbl, s_url);
    lv_label_set_text_static(s_status_lbl, s_status);
    lv_label_set_text_static(s_save_lbl, s_save_text);
    lv_obj_invalidate(s_qr_obj);

    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scrim);                       /* above the list that was opened after this was built */
    s_open = true;
    /* The opening tap's release mustn't land on the scrim and close the card again. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_indev_wait_release(indev);
    }
    ESP_LOGI(TAG, "open: %s code of %d modules", ok ? "QR" : "no", s_qr_size);
}

void ui_link_close(void)
{
    hide_card();
}
