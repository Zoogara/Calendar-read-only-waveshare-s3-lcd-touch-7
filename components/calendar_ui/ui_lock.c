/* Screen-lock PIN keypad. Shown over the ambient clock when the display is
 * locked and someone touches it (see ui_screensaver.c, which owns the
 * locked state and decides when to open and close this).
 *
 * Kept as cheap as possible on internal RAM, where LVGL objects live:
 * everything is created when the keypad opens and deleted when it closes,
 * so it costs nothing the rest of the time, and the twelve keys are a
 * single lv_btnmatrix object rather than twelve buttons.
 *
 * The PIN is exactly 4 digits (app_settings_t.lock_pin) and is checked as
 * soon as the 4th digit is entered - there's no OK key. After
 * MAX_WRONG_TRIES wrong PINs in a row the keypad ignores input for
 * LOCKOUT_MS, so the 10,000 possible PINs can't be run through quickly.
 *
 * Deleting the keypad from inside its own button handler is the LVGL
 * foot-gun ui_settings_dialog.c already documents, so every close that
 * starts from a key press goes through lv_async_call(). */
#include "calendar_ui_internal.h"
#include "ui_theme.h"

#include <string.h>
#include "esp_log.h"

#define PIN_LEN         4
#define MAX_WRONG_TRIES 5
#define LOCKOUT_MS      (30U * 1000U)

#define KEY_BG      0x303134
#define KEY_TEXT    0xE8EAED
#define DOT_EMPTY   0x5F6368
#define DOT_FILLED  0xE8EAED

static const char *TAG = "ui_lock";

static const char *s_keymap[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_CLOSE, "0", LV_SYMBOL_BACKSPACE, "",
};

static lv_obj_t *s_overlay;
static lv_obj_t *s_dots[PIN_LEN];
static lv_obj_t *s_msg;
static char s_entry[PIN_LEN + 1];
static int s_entry_len;

/* Survive the keypad closing and reopening, so closing it doesn't reset
 * the wrong-PIN count or a lockout in progress. */
static int s_wrong_tries;
static uint32_t s_lockout_until;   /* lv_tick_get() value, 0 = none */

static void update_dots(void)
{
    for (int i = 0; i < PIN_LEN; i++) {
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i < s_entry_len ? DOT_FILLED : DOT_EMPTY), 0);
    }
}

static void clear_entry(void)
{
    memset(s_entry, 0, sizeof(s_entry));
    s_entry_len = 0;
    update_dots();
}

/* Same double invalidate + refresh ui_settings_dialog.c does after
 * deleting a full-screen lv_layer_top() object - under this panel's
 * direct_mode dual-framebuffer setup a single refresh leaves the other
 * buffer still showing the deleted overlay. */
static void delete_overlay(void)
{
    if (s_overlay == NULL) {
        return;
    }
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    s_msg = NULL;
    memset(s_dots, 0, sizeof(s_dots));
    memset(s_entry, 0, sizeof(s_entry));
    s_entry_len = 0;
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static void close_async(void *arg)
{
    (void)arg;
    delete_overlay();
}

static void unlock_async(void *arg)
{
    (void)arg;
    delete_overlay();
    ui_screensaver_unlock();
}

static bool locked_out(void)
{
    if (s_lockout_until == 0) {
        return false;
    }
    int32_t remaining = (int32_t)(s_lockout_until - lv_tick_get());
    if (remaining <= 0) {
        s_lockout_until = 0;
        s_wrong_tries = 0;
        lv_label_set_text(s_msg, "");
        return false;
    }
    lv_label_set_text_fmt(s_msg, "Too many tries - wait %d s", (int)((remaining + 999) / 1000));
    return true;
}

static void check_pin(void)
{
    if (strcmp(s_entry, ui_get_cfg()->lock_pin) == 0) {
        s_wrong_tries = 0;
        ESP_LOGI(TAG, "unlocked");
        lv_async_call(unlock_async, NULL);
        return;
    }
    clear_entry();
    if (++s_wrong_tries >= MAX_WRONG_TRIES) {
        s_lockout_until = lv_tick_get() + LOCKOUT_MS;
        if (s_lockout_until == 0) {
            s_lockout_until = 1;
        }
        ESP_LOGW(TAG, "%d wrong PINs - keypad disabled for %u s", s_wrong_tries, LOCKOUT_MS / 1000);
        locked_out();
    } else {
        lv_label_set_text(s_msg, "Wrong PIN");
    }
}

static void key_cb(lv_event_t *e)
{
    lv_obj_t *matrix = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(matrix);
    if (id == LV_BTNMATRIX_BTN_NONE) {
        return;
    }
    const char *txt = lv_btnmatrix_get_btn_text(matrix, id);
    if (txt == NULL) {
        return;
    }

    if (strcmp(txt, LV_SYMBOL_CLOSE) == 0) {
        lv_async_call(close_async, NULL);
        return;
    }
    if (locked_out()) {
        return;
    }
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        if (s_entry_len > 0) {
            s_entry[--s_entry_len] = '\0';
            update_dots();
        }
        return;
    }
    if (txt[0] >= '0' && txt[0] <= '9' && s_entry_len < PIN_LEN) {
        lv_label_set_text(s_msg, "");
        s_entry[s_entry_len++] = txt[0];
        update_dots();
        if (s_entry_len == PIN_LEN) {
            check_pin();
        }
    }
}

bool ui_lock_keypad_is_open(void)
{
    return s_overlay != NULL;
}

void ui_lock_keypad_open(void)
{
    if (s_overlay != NULL) {
        return;
    }

    /* Full-screen, opaque and clickable, on top of the ambient clock, so a
     * touch anywhere lands here rather than on anything underneath. */
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title, &gcal_font_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(KEY_TEXT), 0);
    lv_label_set_text(title, "Enter PIN");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    for (int i = 0; i < PIN_LEN; i++) {
        s_dots[i] = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 16, 16);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_align(s_dots[i], LV_ALIGN_TOP_MID, (i - (PIN_LEN - 1) / 2.0f) * 32, 66);
    }

    s_msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_msg, &gcal_font_14, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(UI_COLOR_WARNING), 0);
    lv_label_set_text(s_msg, "");
    lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, 94);

    lv_obj_t *keys = lv_btnmatrix_create(s_overlay);
    lv_btnmatrix_set_map(keys, s_keymap);
    lv_obj_set_size(keys, 320, 330);
    lv_obj_align(keys, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_set_style_bg_opa(keys, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keys, 0, 0);
    lv_obj_set_style_pad_all(keys, 0, 0);
    lv_obj_set_style_pad_gap(keys, 12, 0);
    lv_obj_set_style_bg_color(keys, lv_color_hex(KEY_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keys, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keys, lv_color_hex(KEY_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keys, &gcal_font_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(keys, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keys, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(keys, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(keys, key_cb, LV_EVENT_VALUE_CHANGED, NULL);

    clear_entry();
    locked_out();   /* shows the countdown if a lockout is still running */
    ESP_LOGI(TAG, "keypad open");
}

void ui_lock_keypad_close(void)
{
    if (s_overlay != NULL) {
        delete_overlay();
        ESP_LOGI(TAG, "keypad closed");
    }
}
