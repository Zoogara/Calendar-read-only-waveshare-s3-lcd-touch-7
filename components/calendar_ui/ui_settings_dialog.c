/* On-device "Display Settings" panel (gear icon -> Display) - lets the
 * screen timeout, calendar refresh interval, the week/day views' hour
 * range, and the runtime config web server's password (see
 * components/provisioning/config_web.c) be changed without going through
 * the Wi-Fi setup portal. The first four are a fixed set of choices (a
 * touchscreen with no keyboard isn't a great place for free-form numeric
 * entry) shown in dropdowns; the password needs free-form text, so it
 * gets its own on-screen-keyboard sub-dialog. Saving writes the whole
 * settings struct back to NVS and restarts, the same "change something,
 * restart to apply" pattern as reconfigure/OTA. */
#include "calendar_ui_internal.h"
#include "ui_theme.h"
#include "provisioning.h"

#include <string.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    const char *label;
    uint32_t value;
} option_t;

static const option_t TIMEOUT_OPTIONS[] = {
    {"1 min", 60}, {"2 min", 120}, {"5 min", 300}, {"10 min", 600},
    {"15 min", 900}, {"30 min", 1800}, {"Never", 0},
};
static const option_t REFRESH_OPTIONS[] = {
    {"1 min", 60}, {"2 min", 120}, {"5 min", 300}, {"10 min", 600},
    {"15 min", 900}, {"30 min", 1800}, {"60 min", 3600},
};
static const option_t START_HOUR_OPTIONS[] = {
    {"4 AM", 4}, {"5 AM", 5}, {"6 AM", 6}, {"7 AM", 7}, {"8 AM", 8}, {"9 AM", 9}, {"10 AM", 10},
};
static const option_t END_HOUR_OPTIONS[] = {
    {"5 PM", 17}, {"6 PM", 18}, {"7 PM", 19}, {"8 PM", 20},
    {"9 PM", 21}, {"10 PM", 22}, {"11 PM", 23}, {"12 AM", 24},
};

#define N_OPTS(arr) (int)(sizeof(arr) / sizeof((arr)[0]))

static lv_obj_t *s_panel;
static lv_obj_t *s_dd_timeout;
static lv_obj_t *s_dd_refresh;
static lv_obj_t *s_dd_start;
static lv_obj_t *s_dd_end;
static lv_obj_t *s_pw_status_lbl;

/* Staged password edit, applied to cfg only when the main dialog's own
 * Save & Restart is pressed - same "not touched until Save" behaviour as
 * the dropdowns above, so cancelling the main dialog after changing the
 * password discards it. */
static char s_pending_password[64];
static bool s_pending_password_set;

static lv_obj_t *s_pw_dialog;
static lv_obj_t *s_pw_textarea;

static void build_option_string(const option_t *opts, int n, char *out, size_t out_sz)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            strncat(out, "\n", out_sz - strlen(out) - 1);
        }
        strncat(out, opts[i].label, out_sz - strlen(out) - 1);
    }
}

static int index_for_value(const option_t *opts, int n, uint32_t value)
{
    for (int i = 0; i < n; i++) {
        if (opts[i].value == value) {
            return i;
        }
    }
    return 0;
}

static lv_obj_t *add_row(lv_obj_t *parent, int y, const char *label_text,
                          const option_t *opts, int n_opts, uint32_t current_value)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &gcal_font_14, 0);
    lv_obj_set_style_text_color(lbl, ui_color(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(lbl, 24, y + 10);

    lv_obj_t *dd = lv_dropdown_create(parent);
    char opt_str[180];
    build_option_string(opts, n_opts, opt_str, sizeof(opt_str));
    lv_dropdown_set_options(dd, opt_str);
    lv_dropdown_set_selected(dd, index_for_value(opts, n_opts, current_value));
    lv_obj_set_pos(dd, 280, y);
    lv_obj_set_width(dd, 150);
    lv_obj_set_style_text_font(dd, &gcal_font_14, 0);

    return dd;
}

static void update_pw_status_label(void)
{
    /* Short text, not "Configured"/"Not set" - the fixed gap between this
     * label and the "Change" button next to it isn't wide enough for the
     * longer wording, which ran under the button. */
    const char *pw = s_pending_password_set ? s_pending_password : ui_get_cfg()->config_web_password;
    lv_label_set_text(s_pw_status_lbl, pw[0] ? "Set" : "Not set");
}

static void pw_confirm(void)
{
    strncpy(s_pending_password, lv_textarea_get_text(s_pw_textarea), sizeof(s_pending_password) - 1);
    s_pending_password_set = true;
    update_pw_status_label();
    lv_obj_del(s_pw_dialog);
    s_pw_dialog = NULL;
}

static void pw_dismiss(void)
{
    lv_obj_del(s_pw_dialog);
    s_pw_dialog = NULL;
}

/* READY fires when the keyboard's own checkmark/enter key is tapped,
 * CANCEL when its X is tapped - kept as a secondary path, but the keys
 * that do this aren't obviously labelled on a small on-screen keyboard,
 * so explicit "Cancel"/"Done" buttons next to the text field (below) are
 * the primary, unambiguous way out. */
static void pw_keyboard_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        pw_confirm();
    } else {
        pw_dismiss();
    }
}

static void pw_done_btn_cb(lv_event_t *e)
{
    (void)e;
    pw_confirm();
}

static void pw_cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    pw_dismiss();
}

/* Full-screen (not just the settings panel's size) so the keyboard has
 * room - shown on top of the settings panel rather than replacing it. */
static void password_btn_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *dlg = lv_obj_create(lv_layer_top());
    /* remove_style_all() must come before set_size()/set_pos() below, not
     * after - it clears local style properties, and width/height/x/y are
     * stored as local style properties in LVGL8, so calling it afterward
     * silently wipes the size/position right back out to the default
     * lv_obj size. That's what was actually behind the "keyboard renders
     * as 4 keys in the top-left" report: this container collapsed to
     * LVGL's tiny default size, which then clipped the (correctly-sized)
     * keyboard child down to whatever sliver of it fell inside that
     * shrunken area. Every other place in this codebase already does
     * create -> remove_style_all -> set_pos/set_size in that order. */
    lv_obj_remove_style_all(dlg);
    lv_obj_set_size(dlg, 800, 480);
    lv_obj_set_pos(dlg, 0, 0);
    lv_obj_set_style_bg_color(dlg, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(dlg);
    lv_label_set_text(lbl, "Config web password");
    lv_obj_set_style_text_font(lbl, &gcal_font_14, 0);
    lv_obj_set_pos(lbl, 16, 10);

    lv_obj_t *ta = lv_textarea_create(dlg);
    lv_textarea_set_one_line(ta, true);
    /* Shown in the clear rather than masked - this is entered standing at
     * the device itself, same physical-access trust level as the Wi-Fi
     * password/service-account key in the setup portal, and plain text
     * makes it much easier to catch a typo on a small on-screen keyboard. */
    lv_textarea_set_password_mode(ta, false);
    lv_textarea_set_max_length(ta, sizeof(s_pending_password) - 1);
    lv_textarea_set_text(ta, s_pending_password_set ? s_pending_password : ui_get_cfg()->config_web_password);
    lv_obj_set_size(ta, 560, 44);
    lv_obj_set_pos(ta, 20, 44);
    s_pw_textarea = ta;

    lv_obj_t *cancel_btn = lv_obj_create(dlg);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, 90, 44);
    lv_obj_set_pos(cancel_btn, 590, 44);
    lv_obj_set_style_bg_color(cancel_btn, ui_color(UI_COLOR_NAV_BG), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_border_color(cancel_btn, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cancel_btn, pw_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    lv_obj_t *done_btn = lv_obj_create(dlg);
    lv_obj_remove_style_all(done_btn);
    lv_obj_set_size(done_btn, 90, 44);
    lv_obj_set_pos(done_btn, 690, 44);
    lv_obj_set_style_bg_color(done_btn, ui_color(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(done_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(done_btn, 6, 0);
    lv_obj_add_flag(done_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(done_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(done_btn, pw_done_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_lbl = lv_label_create(done_btn);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_set_style_text_color(done_lbl, lv_color_white(), 0);
    lv_obj_center(done_lbl);

    lv_obj_t *kb = lv_keyboard_create(dlg);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, 800, 370);
    lv_obj_set_pos(kb, 0, 110);
    lv_obj_add_event_cb(kb, pw_keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, pw_keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    s_pw_dialog = dlg;
}

/* provisioning_save() does a real NVS flash write, which needs a task
 * with an internal-RAM stack, not the LVGL task's PSRAM one - same
 * constraint as reconfigure_task/ota_task in calendar_ui.c. */
static void settings_save_task(void *arg)
{
    (void)arg;
    provisioning_save(ui_get_cfg());
    esp_restart();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_del(s_panel);
    s_panel = NULL;
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    app_settings_t *cfg = ui_get_cfg();

    cfg->screen_timeout_s = TIMEOUT_OPTIONS[lv_dropdown_get_selected(s_dd_timeout)].value;
    cfg->refresh_interval_s = REFRESH_OPTIONS[lv_dropdown_get_selected(s_dd_refresh)].value;
    cfg->view_start_hour = (uint8_t)START_HOUR_OPTIONS[lv_dropdown_get_selected(s_dd_start)].value;
    cfg->view_end_hour = (uint8_t)END_HOUR_OPTIONS[lv_dropdown_get_selected(s_dd_end)].value;
    if (s_pending_password_set) {
        strncpy(cfg->config_web_password, s_pending_password, sizeof(cfg->config_web_password) - 1);
    }

    lv_obj_del(s_panel);
    s_panel = NULL;

    xTaskCreate(settings_save_task, "settings_save", 4096, NULL, 5, NULL);
}

void ui_settings_dialog_show(void)
{
    app_settings_t *cfg = ui_get_cfg();
    s_pending_password_set = false;
    s_pending_password[0] = '\0';

    lv_obj_t *panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel, 480, 440);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Swallow taps so they don't fall through to the calendar underneath
     * while this dialog is open. */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Display Settings");
    lv_obj_set_style_text_font(title, &gcal_font_20, 0);
    lv_obj_set_pos(title, 24, 7);

    s_dd_timeout = add_row(panel, 45, "Screen timeout",
                            TIMEOUT_OPTIONS, N_OPTS(TIMEOUT_OPTIONS), cfg->screen_timeout_s);
    s_dd_refresh = add_row(panel, 91, "Refresh interval",
                            REFRESH_OPTIONS, N_OPTS(REFRESH_OPTIONS), cfg->refresh_interval_s);
    s_dd_start = add_row(panel, 137, "Week/day view start",
                          START_HOUR_OPTIONS, N_OPTS(START_HOUR_OPTIONS), cfg->view_start_hour);
    s_dd_end = add_row(panel, 183, "Week/day view end",
                        END_HOUR_OPTIONS, N_OPTS(END_HOUR_OPTIONS), cfg->view_end_hour);

    lv_obj_t *pw_lbl = lv_label_create(panel);
    lv_label_set_text(pw_lbl, "Config web password");
    lv_obj_set_style_text_font(pw_lbl, &gcal_font_14, 0);
    lv_obj_set_style_text_color(pw_lbl, ui_color(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(pw_lbl, 24, 239);

    s_pw_status_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(s_pw_status_lbl, &gcal_font_14, 0);
    lv_obj_set_style_text_color(s_pw_status_lbl, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(s_pw_status_lbl, 280, 239);
    update_pw_status_label();

    lv_obj_t *pw_btn = lv_obj_create(panel);
    lv_obj_set_size(pw_btn, 80, 34);
    lv_obj_set_pos(pw_btn, 350, 231);
    lv_obj_set_style_radius(pw_btn, 6, 0);
    lv_obj_set_style_bg_color(pw_btn, ui_color(UI_COLOR_NAV_BG), 0);
    lv_obj_set_style_bg_opa(pw_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pw_btn, 1, 0);
    lv_obj_set_style_border_color(pw_btn, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_add_flag(pw_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pw_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pw_btn, password_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pw_btn_lbl = lv_label_create(pw_btn);
    lv_label_set_text(pw_btn_lbl, "Change");
    lv_obj_center(pw_btn_lbl);

    lv_obj_t *note = lv_label_create(panel);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 406);
    lv_label_set_text(note, "Saving restarts the device to apply these. The config web "
                             "server (everything except Wi-Fi, from a browser on your "
                             "network) only runs once a password is set here.");
    lv_obj_set_style_text_font(note, &gcal_font_14, 0);
    lv_obj_set_style_text_color(note, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_pos(note, 24, 283);

    /* Cancel (140) + Save (190) + a 20px gap = 350, centered in the 480-
     * wide panel means 65px margin on each side. */
    lv_obj_t *cancel_btn = lv_obj_create(panel);
    lv_obj_set_size(cancel_btn, 140, 50);
    lv_obj_set_pos(cancel_btn, 65, 336);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_set_style_bg_color(cancel_btn, ui_color(UI_COLOR_NAV_BG), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_border_color(cancel_btn, ui_color(UI_COLOR_GRID_LINE), 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    lv_obj_t *save_btn = lv_obj_create(panel);
    lv_obj_set_size(save_btn, 190, 50);
    lv_obj_set_pos(save_btn, 225, 336);
    lv_obj_set_style_radius(save_btn, 6, 0);
    lv_obj_set_style_bg_color(save_btn, ui_color(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(save_btn, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save & Restart");
    lv_obj_set_style_text_color(save_lbl, lv_color_white(), 0);
    lv_obj_center(save_lbl);

    s_panel = panel;
}
