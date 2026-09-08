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
 * Known minor quirk (see README's "Bring-up troubleshooting" for the
 * fuller writeup of this panel's other timing-sensitive issue): on real
 * hardware, the once-a-minute text change occasionally shows a single
 * frame of visible tearing before settling on the correct new time. A
 * few different fixes were tried (forcing an extra redraw at various
 * scopes/frequencies, directly mirroring the two physical framebuffers)
 * - each either didn't help or, in the framebuffer-mirroring case,
 * occasionally reverted a correct frame back to the previous minute,
 * which is worse than an occasional flash. The current code (a plain,
 * de-duplicated lv_label_set_text()/lv_obj_align() plus one scoped
 * double-refresh, see the end of ui_clock_update()) is the best trade-off
 * found: rare, brief, and never wrong - just not perfectly clean. */
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
static lv_obj_t *s_label;

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

    s_label = lv_label_create(s_cont);
    lv_label_set_recolor(s_label, true);
    lv_obj_set_style_text_font(s_label, &gcal_font_clock, 0);
    /* Fixed width, not size-to-content - same reasoning as s_title_label
     * in calendar_ui.c: gcal_font_clock is proportional (a "1" is
     * narrower than a "0"), so the rendered "HH:MM" is a slightly
     * different pixel width every time the minute changes. Under this
     * panel's direct_mode/avoid_tearing dual-framebuffer setup, an
     * auto-sized label whose bounding box changes shape leaves stale
     * pixels behind on whichever framebuffer wasn't just redrawn. A
     * fixed-size box keeps the invalidated rectangle identical every
     * time the text changes, regardless of which digits are showing. */
    lv_obj_set_width(s_label, CLOCK_H_RES - 80);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_label, LV_ALIGN_CENTER, 0, 0);

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

    uint32_t colon_color = ui_darken(UI_COLOR_TEXT_MUTED, pct);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "#%06lx %c##%06lx %c#"
             "#%06lx :#"
             "#%06lx %c##%06lx %c#",
             (unsigned long)ui_darken(digit_base_color(0), pct), digits[0],
             (unsigned long)ui_darken(digit_base_color(1), pct), digits[1],
             (unsigned long)colon_color,
             (unsigned long)ui_darken(digit_base_color(2), pct), mins[0],
             (unsigned long)ui_darken(digit_base_color(3), pct), mins[1]);
    /* Only actually touch the label - and force the redraw below - when
     * its rendered content would change. The displayed time only changes
     * once a minute (the dim level only twice a day, at the day/night
     * boundary), but this function is called every ~1s to stay responsive
     * to the ambient/sleep transition. Calling lv_label_set_text() with
     * the same string it already has still unconditionally marks the
     * object dirty, and doing that every tick - instead of only on the
     * ~1-in-60 ticks where something really changed - was confirmed on
     * real hardware to be the source of a once-a-second flash this
     * screen used to have. */
    static char s_last_buf[128] = {0};
    bool text_changed = strcmp(buf, s_last_buf) != 0;
    if (text_changed) {
        lv_label_set_text(s_label, buf);
        strncpy(s_last_buf, buf, sizeof(s_last_buf) - 1);
        s_last_buf[sizeof(s_last_buf) - 1] = '\0';
    }

    /* Nudge position every ~10 minutes - see file header comment. Same
     * "only touch it when it actually changes" reasoning as above -
     * lv_obj_align() forces a layout/invalidate pass every time it's
     * called, whether or not the position actually moved. */
    int jitter_idx = (lv_tick_get() / JITTER_PERIOD_MS) % (sizeof(s_jitter) / sizeof(s_jitter[0]));
    static int s_last_jitter_idx = -1;
    bool jitter_changed = jitter_idx != s_last_jitter_idx;
    if (jitter_changed) {
        lv_obj_align(s_label, LV_ALIGN_CENTER, s_jitter[jitter_idx][0], s_jitter[jitter_idx][1]);
        s_last_jitter_idx = jitter_idx;
    }

    if (text_changed || jitter_changed) {
        /* Scoped to just this label, not lv_scr_act() - go_ambient()/
         * go_calendar()/go_sleep() in ui_screensaver.c do the full-screen
         * version of this same dance, needed there because those
         * transitions actually change the whole screen. This one only
         * needs to keep this label in sync across both of this panel's
         * ping-ponged framebuffers on the (rare - at most once a minute)
         * ticks where it actually changed. Confirmed on real hardware
         * this doesn't eliminate every occasional single-frame flash on a
         * real change, but it does reliably get the new content on
         * screen - a direct memcpy between the two framebuffers, tried as
         * an alternative to a second lv_refr_now() here, turned out to
         * sometimes copy the wrong direction and revert a just-rendered
         * frame back to the previous minute, which is worse than an
         * occasional flash - reverted. */
        lv_obj_invalidate(s_label);
        lv_refr_now(NULL);
        lv_obj_invalidate(s_label);
        lv_refr_now(NULL);
    }
}
