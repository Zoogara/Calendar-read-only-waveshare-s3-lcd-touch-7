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

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ha_weather.h"

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
static lv_obj_t *s_bell_label;
static lv_obj_t *s_weather_row;
static lv_obj_t *s_weather_icon;
static lv_obj_t *s_weather_label;
static lv_obj_t *s_date_label;

/* Home Assistant condition (the description sensor's raw state) -> Weather
 * Icons glyph, day and night. The night icons replace the sun with a moon
 * where the font has one. Every codepoint here must also be in
 * gcal_font_weather.c's -r list. Unknown conditions show no icon. */
typedef struct {
    const char *cond;
    const char *day;
    const char *night;
} weather_glyph_t;

static const weather_glyph_t s_weather_glyphs[] = {
    { "clear",            "\xEF\x80\x8D" /* f00d day-sunny */,          "\xEF\x80\xAE" /* f02e night-clear */ },
    { "sunny",            "\xEF\x80\x8D" /* f00d day-sunny */,          "\xEF\x80\xAE" /* f02e night-clear */ },
    { "mostly_sunny",     "\xEF\x80\x8C" /* f00c day-sunny-overcast */, "\xEF\x82\x86" /* f086 night-alt-cloudy */ },
    { "partly_cloudy",    "\xEF\x80\x82" /* f002 day-cloudy */,         "\xEF\x82\x81" /* f081 night-alt-partly-cloudy */ },
    { "cloudy",           "\xEF\x80\x93" /* f013 cloudy */,             "\xEF\x80\x93" },
    { "cyclone",          "\xEF\x81\xB3" /* f073 hurricane */,          "\xEF\x81\xB3" },
    { "tropical_cyclone", "\xEF\x81\xB3" /* f073 hurricane */,          "\xEF\x81\xB3" },
    { "dust",             "\xEF\x81\xA3" /* f063 dust */,               "\xEF\x81\xA3" },
    { "dusty",            "\xEF\x81\xA3" /* f063 dust */,               "\xEF\x81\xA3" },
    { "fog",              "\xEF\x80\x83" /* f003 day-fog */,            "\xEF\x81\x8A" /* f04a night-fog */ },
    { "haze",             "\xEF\x82\xB6" /* f0b6 day-haze */,           "\xEF\x81\x8A" /* f04a night-fog */ },
    { "hazy",             "\xEF\x82\xB6" /* f0b6 day-haze */,           "\xEF\x81\x8A" /* f04a night-fog */ },
    { "frost",            "\xEF\x81\xB6" /* f076 snowflake-cold */,     "\xEF\x81\xB6" },
    { "light_rain",       "\xEF\x80\x8B" /* f00b day-sprinkle */,       "\xEF\x80\xAB" /* f02b night-alt-sprinkle */ },
    { "light_shower",     "\xEF\x80\x89" /* f009 day-showers */,        "\xEF\x80\xA9" /* f029 night-alt-showers */ },
    { "light_showers",    "\xEF\x80\x89" /* f009 day-showers */,        "\xEF\x80\xA9" /* f029 night-alt-showers */ },
    { "shower",           "\xEF\x80\x89" /* f009 day-showers */,        "\xEF\x80\xA9" /* f029 night-alt-showers */ },
    { "showers",          "\xEF\x80\x89" /* f009 day-showers */,        "\xEF\x80\xA9" /* f029 night-alt-showers */ },
    { "heavy_shower",     "\xEF\x80\x9A" /* f01a showers */,            "\xEF\x80\x9A" },
    { "heavy_showers",    "\xEF\x80\x9A" /* f01a showers */,            "\xEF\x80\x9A" },
    { "rain",             "\xEF\x80\x99" /* f019 rain */,               "\xEF\x80\x99" },
    { "snow",             "\xEF\x80\x8A" /* f00a day-snow */,           "\xEF\x80\xAA" /* f02a night-alt-snow */ },
    { "storm",            "\xEF\x80\x90" /* f010 day-thunderstorm */,   "\xEF\x80\xAD" /* f02d night-alt-thunderstorm */ },
    { "storms",           "\xEF\x80\x90" /* f010 day-thunderstorm */,   "\xEF\x80\xAD" /* f02d night-alt-thunderstorm */ },
    { "wind",             "\xEF\x80\xA1" /* f021 windy */,              "\xEF\x80\xA1" },
    { "windy",            "\xEF\x80\xA1" /* f021 windy */,              "\xEF\x80\xA1" },
};

static const char *weather_glyph(const char *cond, bool night)
{
    for (size_t i = 0; i < sizeof(s_weather_glyphs) / sizeof(s_weather_glyphs[0]); i++) {
        if (strcmp(cond, s_weather_glyphs[i].cond) == 0) {
            return night ? s_weather_glyphs[i].night : s_weather_glyphs[i].day;
        }
    }
    return "";
}

/* Home Assistant weather, top right (see components/ha_weather). Base
 * offset from the corner; the same s_jitter nudge as the clock is added
 * on top so it doesn't sit pixel-static for hours. */
#define WEATHER_X (-24)
#define WEATHER_Y 20

/* Date, top left - mirrors the weather line's offset and jitter. Its y is
 * WEATHER_Y plus however far the temperature sits below the top of the
 * weather row (centred against the taller icon), so the two text lines are
 * level; worked out from the fonts in ui_clock_create(). */
#define DATE_X 24
static lv_coord_t s_date_y = WEATHER_Y;

static void date_pressed_cb(lv_event_t *e)
{
    (void)e;
    ui_history_open();
}

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

    /* Small "a reminder's pending" indicator (see ui_reminder.c) - a
     * sibling of s_row, not a child of it, so it stays put in the corner
     * regardless of s_row's own every-10-minutes jitter. Hidden by
     * default; ui_clock_update() shows it only while
     * ui_reminder_has_pending() is true. */
    s_bell_label = lv_label_create(s_cont);
    lv_label_set_text(s_bell_label, "\xEF\x83\xB3" /* U+F0F3 FontAwesome "bell" */);
    lv_obj_set_style_text_font(s_bell_label, &gcal_font_icon_bell, 0);
    lv_obj_align(s_bell_label, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_add_flag(s_bell_label, LV_OBJ_FLAG_HIDDEN);

    /* Weather from Home Assistant: a condition icon then the temperature,
     * e.g. [cloud] "16°C", in a row so they move together - a sibling of
     * s_row like the bell, in the colon's colour. Empty when there's no
     * fresh data (or the feature is off). */
    s_weather_row = lv_obj_create(s_cont);
    lv_obj_remove_style_all(s_weather_row);
    lv_obj_set_size(s_weather_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_weather_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_weather_row, LV_FLEX_FLOW_ROW);
    /* Main-axis START, not END: the row sizes itself to its contents and is
     * pinned to the corner by lv_obj_align() below. END alignment inside a
     * content-sized row pushed the icon past the row's left edge, where the
     * row clipped it - on real hardware the row measured only the
     * temperature's width and the icon never appeared. The temperature is
     * centred vertically against the taller icon; the date is moved down
     * to match (see s_date_y). */
    lv_obj_set_flex_align(s_weather_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_weather_row, 10, 0);
    lv_obj_align(s_weather_row, LV_ALIGN_TOP_RIGHT, WEATHER_X, WEATHER_Y);

    /* Date, top left, e.g. "Sun, 4 Oct" - same font and colour as the
     * weather line opposite it. */
    s_date_y = WEATHER_Y + (lv_font_get_line_height(&gcal_font_weather) -
                            lv_font_get_line_height(&gcal_font_20)) / 2;
    s_date_label = lv_label_create(s_cont);
    lv_obj_set_style_text_font(s_date_label, &gcal_font_20, 0);
    lv_label_set_text(s_date_label, "");
    /* Tap the date for "on this day in history" (ui_history.c). On PRESSED,
     * not CLICKED: the list is open before ui_screensaver.c's next tick
     * can see the touch and wake the calendar (or open the PIN keypad). */
    lv_obj_add_flag(s_date_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_date_label, 24);
    lv_obj_add_event_cb(s_date_label, date_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_align(s_date_label, LV_ALIGN_TOP_LEFT, DATE_X, s_date_y);

    s_weather_icon = lv_label_create(s_weather_row);
    lv_obj_set_style_text_font(s_weather_icon, &gcal_font_weather, 0);
    lv_label_set_text(s_weather_icon, "");

    s_weather_label = lv_label_create(s_weather_row);
    lv_obj_set_style_text_font(s_weather_label, &gcal_font_20, 0);
    lv_label_set_text(s_weather_label, "");

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
     * tone, no per-run colour needed. The bell indicator (see
     * ui_reminder.c) shares this exact colour and dims with it, at
     * night, the same as everything else on this screen - it's meant to
     * be a subtle "by the way" cue, not something that stands out more
     * than the clock itself does. */
    static int32_t s_last_colon_color = -1;
    uint32_t colon_color = ui_darken(UI_COLOR_TEXT_MUTED, pct);
    if ((int32_t)colon_color != s_last_colon_color) {
        lv_obj_set_style_text_color(s_colon_label, ui_color(colon_color), 0);
        lv_obj_set_style_text_color(s_bell_label, ui_color(colon_color), 0);
        lv_obj_set_style_text_color(s_weather_label, ui_color(colon_color), 0);
        lv_obj_set_style_text_color(s_weather_icon, ui_color(colon_color), 0);
        lv_obj_set_style_text_color(s_date_label, ui_color(colon_color), 0);
        s_last_colon_color = (int32_t)colon_color;
    }

    if (ui_reminder_has_pending()) {
        lv_obj_clear_flag(s_bell_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bell_label, LV_OBJ_FLAG_HIDDEN);
    }

    ha_weather_t wx;
    char wx_buf[64] = "";
    const char *wx_icon = "";
    if (ha_weather_get(&wx)) {
        /* Day/night from HA's sun.sun; the clock's own day hours if that
         * wasn't available. */
        bool night = wx.night >= 0 ? (wx.night == 1) : !is_daytime;
        wx_icon = weather_glyph(wx.cond, night);
        /* One decimal place, as Home Assistant reports it. A reading that
         * rounds to zero from below would print as "-0.0"; show "0.0". */
        float t = wx.temp_c;
        if (fabsf(t) < 0.05f) {
            t = 0.0f;
        }
        snprintf(wx_buf, sizeof(wx_buf), "%.1f\xC2\xB0" "C", (double)t);
    }
    static char s_last_wx[64] = {0};
    static char s_last_wx_icon[8] = {0};
    set_label_if_changed(s_weather_label, wx_buf, s_last_wx, sizeof(s_last_wx));
    set_label_if_changed(s_weather_icon, wx_icon, s_last_wx_icon, sizeof(s_last_wx_icon));

    /* "Sun, 4 Oct" - strftime's %a/%b are the C locale's English
     * abbreviations; the day is formatted separately to avoid a leading
     * zero (or %e's leading space). */
    char wday[8], mon[8], date_buf[24];
    strftime(wday, sizeof(wday), "%a", &tm_now);
    strftime(mon, sizeof(mon), "%b", &tm_now);
    snprintf(date_buf, sizeof(date_buf), "%s, %d %s", wday, tm_now.tm_mday, mon);
    static char s_last_date[24] = {0};
    set_label_if_changed(s_date_label, date_buf, s_last_date, sizeof(s_last_date));

    /* Nudge the whole group's position every ~10 minutes - see file
     * header comment. Applied to s_row (all three objects move together)
     * rather than any one label. */
    int jitter_idx = (lv_tick_get() / JITTER_PERIOD_MS) % (sizeof(s_jitter) / sizeof(s_jitter[0]));
    static int s_last_jitter_idx = -1;
    if (jitter_idx != s_last_jitter_idx) {
        lv_obj_align(s_row, LV_ALIGN_CENTER, s_jitter[jitter_idx][0], s_jitter[jitter_idx][1]);
        lv_obj_align(s_date_label, LV_ALIGN_TOP_LEFT,
                     DATE_X + s_jitter[jitter_idx][0], s_date_y + s_jitter[jitter_idx][1]);
        lv_obj_align(s_weather_row, LV_ALIGN_TOP_RIGHT,
                     WEATHER_X + s_jitter[jitter_idx][0], WEATHER_Y + s_jitter[jitter_idx][1]);
        s_last_jitter_idx = jitter_idx;
    }
}
