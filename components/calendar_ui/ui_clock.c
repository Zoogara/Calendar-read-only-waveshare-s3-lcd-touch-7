/* Ambient clock: what ui_screensaver.c shows instead of the calendar once
 * the display's gone idle and presence is still detected. A big "HH:MM"
 * rendered in gcal_font_clock, each digit tinted with one of the user's
 * own calendar colours (cycling through whichever calendars are
 * currently enabled, so the ambient display still reads as "this
 * device's" calendar rather than a generic clock face) - loosely
 * modelled on the Google Nest Hub's ambient clock, which does the same
 * "borrow the colours already on screen" trick.
 *
 * There's no backlight PWM on this board (board_bsp.c's backlight is a
 * hard on/off GPIO via the CH422G expander), so the day/night distinction
 * here is done purely in content: colours are blended toward black
 * (ui_darken(), in ui_theme.h) rather than the panel's backlight actually
 * being turned down - vivid during daylight hours, muted at night, reusing
 * the existing view_start_hour/view_end_hour config (the same hours that
 * already decide when the calendar's own Day/Week views consider "the
 * day" to start/end). Presence is NOT handled here any more - once
 * presence has been continuously absent for long enough, ui_screensaver.c
 * takes the display out of DISPLAY_AMBIENT entirely (into its own
 * backlight-off sleep state) rather than asking this file to render an
 * even dimmer clock; see its header comment for why.
 *
 * The digit shapes changing every minute is a deliberate (if incidental)
 * anti-burn-in measure - the old noise-canvas screensaver this ambient
 * clock replaced during calendar_ui's *idle* period solved the same
 * static-image problem by literally repainting random pixels (that
 * canvas is still used for the deeper backlight-off sleep state - see
 * ui_screensaver.c). The clock's on-screen position is also nudged by a
 * few pixels every ~10 minutes for the same reason: a fixed bright region
 * against a fixed black background, held for a while, is exactly the kind
 * of static pattern that risks LC image retention on this panel.
 *
 * Kept deliberately simple: labels are re-set every tick with no manual
 * redraw calls of their own - this panel's direct_mode dual-framebuffer
 * synchronization used to need workarounds scattered through whatever
 * happened to update often, but that turned out to be a genuine bug in
 * ESP-IDF's RGB LCD driver (fixed at the source - see
 * components/esp_lcd/rgb/esp_lcd_panel_rgb.c's own header comment), not
 * something app code ever needed to work around in the first place.
 *
 * Three separate label objects, not one recolored string: when the colon
 * lived inline in the same label/string as the digits, every character
 * shared one baseline, and gcal_font_clock's colon glyph (Montserrat, like
 * most fonts, sits a colon around the x-height rather than spanning the
 * full digit height) read as low and slightly large next to the digits.
 * Splitting it into its own object turns out to fix both complaints for
 * free, with no manual size/position styling needed at all: the colon's
 * generated glyph box is itself only ~76% the height of a digit's (76px
 * vs 128px at this font size - Montserrat's colon is simply a smaller
 * shape to begin with), and s_row's flex cross-axis alignment
 * (LV_FLEX_ALIGN_CENTER) centres each child vertically by *its own* box
 * height rather than by a shared baseline - so the shorter colon object
 * lands vertically centred against the taller digit objects automatically.
 * (An earlier attempt to additionally resize/reposition the colon via
 * lv_obj_set_style_transform_zoom()/translate_y() made it render as
 * completely invisible on real hardware for reasons never fully
 * root-caused - see git history on this file if that's ever worth
 * revisiting - so it was dropped once the zoom-free layout turned out to
 * already look right without it.) */
#include "calendar_ui_internal.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLOCK_H_RES 800
#define CLOCK_V_RES 480

#define DIM_PCT_DAY    0   /* full calendar colour, daylight hours */
#define DIM_PCT_NIGHT  55  /* muted, outside daylight hours */

/* Deterministic small position offsets, cycled every ~10 minutes - see
 * the file header comment. Kept well within the screen margins even at
 * the clock's largest expected size. */
static const int8_t s_jitter[][2] = {
    { 0, 0 }, { 5, -4 }, { -6, 3 }, { 3, 6 }, { -4, -6 }, { 6, 2 },
};
#define JITTER_PERIOD_MS (10U * 60U * 1000U)

static lv_obj_t *s_cont;
static lv_obj_t *s_row;
static lv_obj_t *s_hh_label;
static lv_obj_t *s_colon_label;
static lv_obj_t *s_mm_label;

lv_obj_t *ui_clock_create(lv_obj_t *parent)
{
    s_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(s_cont);
    lv_obj_set_size(s_cont, CLOCK_H_RES, CLOCK_V_RES);
    lv_obj_set_pos(s_cont, 0, 0);
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_HIDDEN);
    /* Must be explicitly clickable, same reasoning as the noise canvas
     * used for the deeper sleep state: without this the wake touch's
     * hit-test would skip past this (visually opaque) overlay and land
     * on whatever calendar element is underneath. */
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_CLICKABLE);

    /* Row container holding "HH", the colon, and "MM" as three separate
     * objects (see file header comment for why) - a flex row keeps them
     * laid out side by side and the whole group centred on screen
     * without having to hand-compute each object's x position as their
     * individual widths change tick to tick. */
    s_row = lv_obj_create(s_cont);
    lv_obj_remove_style_all(s_row);
    lv_obj_set_size(s_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_row, LV_ALIGN_CENTER, 0, 0);

    s_hh_label = lv_label_create(s_row);
    lv_label_set_recolor(s_hh_label, true);
    lv_obj_set_style_text_font(s_hh_label, &gcal_font_clock, 0);

    /* No size styling needed - see file header comment for why the flex
     * row's own cross-axis centring plus the font's naturally-smaller
     * colon glyph already produce the intended size/position with zero
     * extra code (and confirmed on real hardware that a
     * transform_zoom/translate_y here breaks rendering entirely, so don't
     * reach for those again without re-testing on hardware). The one
     * remaining tweak - nudging it up a few pixels from dead-centre,
     * matched to how the eye actually reads a clock face - is done via
     * bottom-only padding rather than a transform: growing the label's
     * own LV_SIZE_CONTENT box downward (asymmetrically) shifts where the
     * row's centring lands the glyph inside it, which is layout, not a
     * post-layout visual shift, so it doesn't hit whatever broke the
     * transform approach. */
    s_colon_label = lv_label_create(s_row);
    lv_obj_set_style_text_font(s_colon_label, &gcal_font_clock, 0);
    lv_obj_set_style_text_color(s_colon_label, ui_color(UI_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_pad_bottom(s_colon_label, 10, 0);
    lv_label_set_text(s_colon_label, ":");

    s_mm_label = lv_label_create(s_row);
    lv_label_set_recolor(s_mm_label, true);
    lv_obj_set_style_text_font(s_mm_label, &gcal_font_clock, 0);

    return s_cont;
}

/* Picks the colour for clock digit `digit_index` (0-3, left to right,
 * skipping the colon) by cycling through the enabled calendars in
 * cfg->calendars[] - so with e.g. 3 calendars enabled, digits use
 * calendar 0, 1, 2, 0. Falls back to UI_COLOR_ACCENT if none are
 * enabled (fresh setup, or everything toggled off from the legend). */
static uint32_t digit_base_color(int digit_index)
{
    app_settings_t *cfg = ui_get_cfg();
    uint32_t enabled_colors[APP_SETTINGS_MAX_CALENDARS];
    int enabled_count = 0;

    for (int i = 0; i < cfg->calendar_count && i < APP_SETTINGS_MAX_CALENDARS; i++) {
        if (cfg->calendars[i].enabled) {
            enabled_colors[enabled_count++] = cfg->calendars[i].color;
        }
    }

    if (enabled_count == 0) {
        return UI_COLOR_ACCENT;
    }
    return enabled_colors[digit_index % enabled_count];
}

/* Only actually touches `label` - and only when `new_text` differs from
 * what it already has - the same "de-dupe before touching LVGL"
 * reasoning that used to matter for framebuffer sync (see file header
 * comment) and still matters for its own sake: the displayed time only
 * changes once a minute, and there's no reason to mark three objects
 * dirty and re-render them every ~1s tick just to set them to the exact
 * text they already have. `last_buf` is the caller's own persistent
 * scratch space (a `static char[]` local to each call site). */
static void set_label_if_changed(lv_obj_t *label, const char *new_text, char *last_buf, size_t last_buf_sz)
{
    if (strcmp(new_text, last_buf) == 0) {
        return;
    }
    lv_label_set_text(label, new_text);
    strncpy(last_buf, new_text, last_buf_sz - 1);
    last_buf[last_buf_sz - 1] = '\0';
}

void ui_clock_update(void)
{
    if (s_cont == NULL) {
        return; /* ui_clock_create() failed or hasn't run yet */
    }

    app_settings_t *cfg = ui_get_cfg();
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    bool is_daytime = (tm_now.tm_hour >= cfg->view_start_hour &&
                        tm_now.tm_hour < cfg->view_end_hour);
    uint8_t pct = is_daytime ? DIM_PCT_DAY : DIM_PCT_NIGHT;

    char digits[4];
    snprintf(digits, sizeof(digits), "%02d", tm_now.tm_hour);
    char mins[4];
    snprintf(mins, sizeof(mins), "%02d", tm_now.tm_min);

    char hh_buf[64];
    snprintf(hh_buf, sizeof(hh_buf), "#%06lx %c##%06lx %c#",
             (unsigned long)ui_darken(digit_base_color(0), pct), digits[0],
             (unsigned long)ui_darken(digit_base_color(1), pct), digits[1]);
    char mm_buf[64];
    snprintf(mm_buf, sizeof(mm_buf), "#%06lx %c##%06lx %c#",
             (unsigned long)ui_darken(digit_base_color(2), pct), mins[0],
             (unsigned long)ui_darken(digit_base_color(3), pct), mins[1]);

    static char s_last_hh[64] = {0};
    static char s_last_mm[64] = {0};
    set_label_if_changed(s_hh_label, hh_buf, s_last_hh, sizeof(s_last_hh));
    set_label_if_changed(s_mm_label, mm_buf, s_last_mm, sizeof(s_last_mm));

    /* Plain text colour, not recolor - the colon is always one solid
     * tone, no per-run colour needed. */
    static int32_t s_last_colon_color = -1;
    uint32_t colon_color = ui_darken(UI_COLOR_TEXT_MUTED, pct);
    if ((int32_t)colon_color != s_last_colon_color) {
        lv_obj_set_style_text_color(s_colon_label, ui_color(colon_color), 0);
        s_last_colon_color = (int32_t)colon_color;
    }

    /* Nudge the whole group's position every ~10 minutes - see file
     * header comment. Applied to s_row (all three objects move together)
     * rather than any one label. */
    int jitter_idx = (lv_tick_get() / JITTER_PERIOD_MS) % (sizeof(s_jitter) / sizeof(s_jitter[0]));
    static int s_last_jitter_idx = -1;
    if (jitter_idx != s_last_jitter_idx) {
        lv_obj_align(s_row, LV_ALIGN_CENTER, s_jitter[jitter_idx][0], s_jitter[jitter_idx][1]);
        s_last_jitter_idx = jitter_idx;
    }
}
