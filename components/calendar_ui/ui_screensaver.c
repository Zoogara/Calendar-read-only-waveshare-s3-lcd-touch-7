/* Idle/presence-driven display states. Three of them:
 *
 *   - DISPLAY_CALENDAR: the normal, bright calendar. Only ever entered by
 *     an actual touch - presence alone never brings the calendar back.
 *   - DISPLAY_AMBIENT: a big ambient clock (ui_clock.c) replaces the
 *     calendar after the configured idle timeout, colour-cycled through
 *     the user's own calendar colours and dimmed for night hours (see
 *     ui_clock.c) - but only entered if presence is detected at that
 *     moment (and the clock feature is currently enabled - see
 *     s_clock_feature_enabled below). The backlight brightness itself is
 *     driven independently by ambient_brightness_tick() below, off a
 *     BH1750 lux sensor - this state's own "dim" is the clock's colour
 *     choice against a black background, layered on top of whatever the
 *     backlight is currently doing.
 *   - DISPLAY_SLEEP: the backlight goes off and a slowly-regenerating
 *     noise pattern (anti-image-retention, not just a blank screen)
 *     replaces whatever was showing - entered either straight from
 *     DISPLAY_CALENDAR (idle timeout fires with nobody present to see a
 *     clock, or the clock feature's toggled off) or from DISPLAY_AMBIENT
 *     (presence has been continuously absent for PRESENCE_AWAY_SLEEP_MS).
 *     Presence returning wakes the display back to the ambient clock,
 *     never straight to the calendar - only a touch does that, from any
 *     state.
 */
#include "calendar_ui_internal.h"
#include "calendar_ui.h"
#include "board_bsp.h"
#include "presence_sensor.h"
#include "light_sensor.h"

#include <math.h>
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

typedef enum {
    DISPLAY_CALENDAR = 0,
    DISPLAY_AMBIENT,
    DISPLAY_SLEEP,
} display_state_t;

#define SS_H_RES 800
#define SS_V_RES 480

#define SNOW_REGEN_MS     (60U * 1000U)       /* repaint noise this often while asleep */
#define WAKE_BIT          (1 << 0)
#define LONG_SLEEP_RESET_MS (15U * 60U * 1000U) /* away from the calendar >= this long ->
                                                    wake to today/Month instead of resuming
                                                    whatever view/date was showing */
#define PRESENCE_AWAY_SLEEP_MS (5U * 60U * 1000U) /* ambient clock showing + presence
                                                      continuously absent this long ->
                                                      drop to full sleep (backlight off) */

static const char *TAG = "ui_screensaver";

/* Idle time before the calendar gives way to the ambient clock (or
 * straight to sleep, if nobody's present) - configurable via the
 * gear-icon settings dialog (cfg->screen_timeout_s), read once at
 * ui_screensaver_init() time; 0 means never leave the calendar. */
static uint32_t s_idle_timeout_ms = APP_SETTINGS_DEFAULT_SCREEN_TIMEOUT_S * 1000U;

static lv_obj_t *s_clock;
static lv_obj_t *s_canvas;
static void *s_canvas_buf;
static volatile display_state_t s_state = DISPLAY_CALENDAR;
static uint32_t s_prev_idle_ms = 0;
static uint32_t s_last_regen_ms = 0;
static uint32_t s_away_from_calendar_ms = 0;  /* lv_tick_get() when we last left
                                                  DISPLAY_CALENDAR - covers ambient
                                                  and sleep as one continuous "away"
                                                  span, for LONG_SLEEP_RESET_MS */
static uint32_t s_presence_away_since_ms = 0; /* 0 while present; set the instant
                                                  presence is first found absent
                                                  while DISPLAY_AMBIENT */
static EventGroupHandle_t s_wake_event;

/* Ambient light -> backlight brightness (see ambient_brightness_tick()
 * below). s_last_applied_permille starts at 1000 (100.0%) - a safe,
 * visible default before the sensor has produced a reading, and matches
 * "normal room light should already read as full" for the very first
 * tick or two of a cold boot. In tenths of a percent (0-1000), not whole
 * percent, matching bsp_display_set_brightness_permille() and
 * app_settings_t's brightness_min_pct_x10 - see that field's own comment
 * for why whole-percent granularity isn't fine enough at the dim end. */
#define LUX_EMA_ALPHA 0.3f /* smoothing factor, 0..1 - higher tracks faster */
static bool s_light_sensor_ok;
static float s_lux_ema = -1.0f;      /* negative = not yet seeded */
static uint16_t s_last_applied_permille = 1000;
static uint32_t s_diag_ticks_left = 300; /* ~5 min of every-tick logging after boot */

/* Runtime-only on/off switch for DISPLAY_AMBIENT, toggled from the
 * eye-icon button in calendar_ui.c's top bar - deliberately NOT part of
 * app_settings_t/NVS: this is a quick "I don't want the clock right now"
 * toggle, not a persistent preference, so it always starts back at
 * enabled (true) after a reboot rather than needing a settings-dialog
 * round trip (and the reboot every settings change already triggers) to
 * turn back on. When disabled, idle timeout goes straight from
 * DISPLAY_CALENDAR to DISPLAY_SLEEP regardless of presence - i.e. exactly
 * the pre-ambient-clock screensaver behaviour. */
static bool s_clock_feature_enabled = true;

bool ui_screensaver_clock_enabled(void)
{
    return s_clock_feature_enabled;
}

void ui_screensaver_toggle_clock_enabled(void)
{
    s_clock_feature_enabled = !s_clock_feature_enabled;
    ESP_LOGI(TAG, "ambient clock feature %s", s_clock_feature_enabled ? "enabled" : "disabled");
}

static void regen_snow(void)
{
    if (s_canvas_buf == NULL) {
        return;
    }
    esp_fill_random(s_canvas_buf, LV_CANVAS_BUF_SIZE_TRUE_COLOR(SS_H_RES, SS_V_RES));
    lv_obj_invalidate(s_canvas);
}

/* Full-screen invalidate + two synchronous lv_refr_now() passes - used at
 * every state transition below, never on a per-tick basis. Under this
 * panel's direct_mode + avoid_tearing dual-framebuffer setup, a single
 * invalidate only guarantees ONE of the two buffers gets flushed by the
 * time it returns, leaving the other showing stale content until
 * something forces a second full redraw - confirmed on real hardware
 * both for this (rare, one-off) use and, previously, for a similar
 * per-tick case in ui_clock.c that turned out to need a completely
 * different fix (see that file) precisely because doing this every
 * second instead of only at real transitions made the rare tear into a
 * constant, visible one. */
static void full_double_refresh(void)
{
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static void go_ambient(void)
{
    display_state_t prev = s_state;
    s_state = DISPLAY_AMBIENT;
    if (prev == DISPLAY_CALENDAR) {
        s_away_from_calendar_ms = lv_tick_get();
        /* Nothing on the calendar view is visible behind the clock
         * overlay, so this is free real estate: release its rendered
         * content the same way switching views does (see calendar_ui.c's
         * release_view()) instead of leaving a full set of event
         * blocks/badges sitting in RAM for however long the display
         * stays away - could be all night. go_calendar() below undoes
         * this before the calendar is shown again. */
        calendar_ui_release_active_view();
    } else { /* prev == DISPLAY_SLEEP */
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        /* Wake at whatever ambient_brightness_tick() last computed, not a
         * hardcoded 100% - avoids a bright flash before the next tick
         * (at most 1s away) dims it back down in a dark room. */
        bsp_display_set_brightness_permille(s_last_applied_permille);
    }
    s_presence_away_since_ms = 0;
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    ui_clock_update();
    full_double_refresh();
    ESP_LOGI(TAG, "showing ambient clock");
}

static void go_sleep(void)
{
    if (s_canvas == NULL) {
        /* ui_screensaver_init() couldn't allocate the noise canvas
         * (out of PSRAM) - nothing to show for this state, so stay put
         * rather than dereference a NULL s_canvas below. Whatever called
         * us just gets no-op'd; the caller doesn't need to know. */
        return;
    }
    display_state_t prev = s_state;
    s_state = DISPLAY_SLEEP;
    if (prev == DISPLAY_CALENDAR) {
        s_away_from_calendar_ms = lv_tick_get();
        calendar_ui_release_active_view();
    } else { /* prev == DISPLAY_AMBIENT */
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    }
    regen_snow();
    s_last_regen_ms = lv_tick_get();
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    bsp_display_backlight(false);
    full_double_refresh();
    ESP_LOGI(TAG, "presence clear - backlight off");
}

static void go_calendar(void)
{
    display_state_t prev = s_state;
    s_state = DISPLAY_CALENDAR;
    uint32_t away_ms = lv_tick_get() - s_away_from_calendar_ms;
    if (away_ms >= LONG_SLEEP_RESET_MS) {
        ESP_LOGI(TAG, "away for %u ms (>= %u) - waking to today/Month view",
                 (unsigned)away_ms, (unsigned)LONG_SLEEP_RESET_MS);
        calendar_ui_reset_to_today_month();
    } else {
        calendar_ui_restore_active_view();
    }
    if (prev == DISPLAY_AMBIENT) {
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    } else if (prev == DISPLAY_SLEEP) {
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        /* See the matching comment in go_ambient() above. */
        bsp_display_set_brightness_permille(s_last_applied_permille);
    }
    full_double_refresh();
    ESP_LOGI(TAG, "touch detected - showing calendar");
    xEventGroupSetBits(s_wake_event, WAKE_BIT);
}

/* Below this many lux, the backlight sits at exactly min_permille - both
 * to give the BH1750's own dark-end jitter (see below) somewhere flat to
 * land, and as the curve's own zero-point (see compute_brightness_permille):
 * it's no longer a separate clamp bolted onto the curve, the curve is
 * defined relative to this threshold, so the two are mathematically
 * guaranteed to meet with no seam. Confirmed on real hardware
 * (2026-09-09): a BH1750 sitting in a genuinely dark/covered spot doesn't
 * read a steady 0 - it jitters across a few raw counts (0, 1, 2...) from
 * ordinary photodiode/quantisation noise near the sensor's floor, and
 * because the whole auto-dim floor now lives in a deliberately narrow
 * 0.1%-10% "dark zone" (see brightness_min_pct_x10), even that small
 * amount of lux jitter was enough to visibly hunt the backlight up and
 * down. Flattening anything at or below this threshold absorbs that noise
 * instead of chasing it. */
#define LUX_DARK_FLOOR 2.0f

/* How many lux above LUX_DARK_FLOOR it takes for the curve to get up to
 * speed - softens the very start of the ramp so the BH1750's own ~1 lux
 * measurement resolution doesn't itself read as a visible brightness
 * step right where a jump is most noticeable (just above full dark).
 * A plain log(1+x) curve (what this used before 2026-09-09) has its
 * steepest slope exactly at x=0, so the single lux count separating
 * "just above the dark floor" from "one measurement step further" was
 * swinging output by a large fraction of the whole range in one hop -
 * on top of, and separate from, the seam bug LUX_DARK_FLOOR's own
 * comment above describes. Dividing by (and adding) this constant inside
 * the log spreads that initial slope out over several lux instead of
 * one, without moving either endpoint: the curve below is still exactly
 * min_permille at lux == LUX_DARK_FLOOR and exactly 1000 (100%) at
 * lux == brightness_max_lux, whatever this is set to. Purely an internal
 * shaping constant - the two settings-page sliders already define the
 * curve's endpoints (the floor % and the lux for 100%); this only
 * affects how briskly it gets from one to the other, which isn't
 * something that needs its own control. */
#define LUX_CURVE_SOFTEN 5.0f

/* Minimum permille change worth actually writing to the LEDC duty
 * register - a true hysteresis band, not just "skip identical values"
 * (which the log-curve's own rounding already made unlikely to matter
 * anyway). Added alongside LUX_DARK_FLOOR above for the same real-
 * hardware hunting complaint: even with the EMA smoothing raw lux, the
 * *computed* permille can still tick up/down by a step or two near a
 * boundary as the smoothed value drifts fractionally either side of it.
 * A older version of this comment reasoned there was no need for a
 * deadband since a bare LEDC duty write is cheap (no I2C, no hardware
 * wear) - true, but that only covers the electrical cost, not the
 * visible one: a duty change too small to serve any purpose still reads
 * as a flicker at the panel. 3 permille (0.3%) is small enough to stay
 * invisible as a *skipped* step but large enough to swallow the observed
 * hunting. */
#define BRIGHTNESS_DEADBAND_PERMILLE 3

/* Largest permille change applied to the backlight in a single tick -
 * anything beyond this ramps toward the target over several ticks
 * instead of jumping there in one. Confirmed on real hardware
 * (2026-09-09) that a big lighting change (covering/uncovering the
 * sensor, a room light switching on) otherwise snapped the backlight
 * straight to its new target within the same 1-second tick the sensor
 * noticed it in - correct, but visually abrupt, more like a light switch
 * than a fade. 150 permille (15%) means a full floor-to-ceiling swing
 * takes roughly 6 ticks (~6 seconds) to settle, while smaller day-to-day
 * adjustments (most of them well under 150 permille) still apply in a
 * single tick same as before - only genuinely large jumps get spread
 * out. Purely a rate limit on top of BRIGHTNESS_DEADBAND_PERMILLE above,
 * not a replacement for it: the deadband still decides whether to move
 * at all, this only caps how far in one go once it does. */
#define BRIGHTNESS_RAMP_STEP_PERMILLE 150

/* Ambient light -> backlight brightness. Runs every tick this timer
 * fires except during DISPLAY_SLEEP (where the backlight is deliberately
 * off regardless of ambient light) - piggybacks on the same 1-second
 * cadence already used for presence polling/state transitions below
 * rather than a dedicated task. Smoothed via a simple exponential moving
 * average so a hand/shadow briefly crossing the sensor - or its own
 * read-to-read jitter - doesn't visibly flicker the screen, then mapped
 * through a log curve (see compute_brightness_permille) rather than
 * linearly.
 *
 * Works in tenths of a percent (permille, 0-1000) throughout rather than
 * whole percent - confirmed on real hardware that the floor
 * (brightness_min_pct_x10) needs to be adjustable within quite a narrow
 * "dark zone", where a single whole percent is already a big step, so
 * whole-percent granularity couldn't represent it usefully. */
static uint16_t compute_brightness_permille(float lux)
{
    const app_settings_t *cfg = ui_get_cfg();
    /* brightness_min_pct_x10's raw value IS already the permille figure,
     * not something to further multiply by 10 - it's stored in tenths of
     * a percent (V means V/10 percent), and permille is percent*10, so
     * permille = (V/10)*10 = V. An earlier version of this line multiplied
     * by 10 again, silently delivering a floor 10x brighter than the
     * slider displayed (e.g. a displayed "0.1%" floor was actually driving
     * the backlight at 1.0%) - caught only once real hardware testing
     * showed the visible floor didn't match the number on the settings
     * page. */
    uint16_t min_permille = cfg->brightness_min_pct_x10
                                 ? cfg->brightness_min_pct_x10
                                 : APP_SETTINGS_DEFAULT_BRIGHTNESS_MIN_PCT_X10;
    uint16_t max_lux = cfg->brightness_max_lux ? cfg->brightness_max_lux
                                                : APP_SETTINGS_DEFAULT_BRIGHTNESS_MAX_LUX;

    /* Everything below is measured relative to the dark floor, not raw
     * lux - the previous version ran the log curve on raw lux and only
     * clamped to min_permille below LUX_DARK_FLOOR as a bolted-on separate
     * step, so the curve's own value AT lux == LUX_DARK_FLOOR (2.0 lux by
     * default -> already ~22% of the full range with the old formula) was
     * nowhere near min_permille. Crossing that threshold in either
     * direction snapped the backlight between the floor and ~22%+ of full
     * range in a single 1-second tick - the "cliff" seen on real hardware.
     * Measuring from the floor instead makes compute_brightness_permille()
     * mathematically equal to min_permille right at lux == LUX_DARK_FLOOR,
     * so the flat region above and the curve below meet with no seam. */
    float lux_above_floor = lux - LUX_DARK_FLOOR;
    if (lux_above_floor <= 0.0f) {
        return min_permille;
    }

    float range_above_floor = (float)max_lux - LUX_DARK_FLOOR;
    if (range_above_floor < 1.0f) {
        range_above_floor = 1.0f; /* guard divide-by-zero if brightness_max_lux
                                      is ever configured at/below the dark floor */
    }

    /* log1p-style ramp, but with LUX_CURVE_SOFTEN folded in (see that
     * constant's own comment for why a bare log(1+x) isn't gentle enough
     * right at the bottom). t=0 at lux_above_floor=0 (i.e. lux ==
     * LUX_DARK_FLOOR) and t=1 at lux_above_floor == range_above_floor
     * (i.e. lux == brightness_max_lux), so the curve's own endpoints line
     * up exactly with the flat floor below and the 100% ceiling above,
     * with nothing left to reconcile between them. */
    float t = logf(1.0f + lux_above_floor / LUX_CURVE_SOFTEN) /
              logf(1.0f + range_above_floor / LUX_CURVE_SOFTEN);
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    return (uint16_t)(min_permille + t * (float)(1000 - min_permille) + 0.5f);
}

static void ambient_brightness_tick(void)
{
    if (!s_light_sensor_ok) {
        return;
    }

    float lux;
    if (light_sensor_read_lux(&lux) != ESP_OK) {
        return;
    }

    if (s_lux_ema < 0.0f) {
        s_lux_ema = lux; /* seed on the first successful reading instead of
                             easing up from 0 over several seconds */
    } else {
        s_lux_ema = LUX_EMA_ALPHA * lux + (1.0f - LUX_EMA_ALPHA) * s_lux_ema;
    }

    /* target_permille is where the curve says the backlight should end up
     * for the current light level; permille (what actually gets applied
     * this tick) only moves part way there, capped at
     * BRIGHTNESS_RAMP_STEP_PERMILLE - see that constant's own comment.
     * Since this function re-runs every tick, a target that's still far
     * off keeps pulling permille another step closer each time, so the
     * backlight fades smoothly toward wherever the light level currently
     * calls for rather than snapping there in one hop. */
    uint16_t target_permille = compute_brightness_permille(s_lux_ema);
    int32_t delta = (int32_t)target_permille - (int32_t)s_last_applied_permille;
    bool changed = (delta >= BRIGHTNESS_DEADBAND_PERMILLE || -delta >= BRIGHTNESS_DEADBAND_PERMILLE);
    if (changed) {
        int32_t step = delta;
        if (step > BRIGHTNESS_RAMP_STEP_PERMILLE) {
            step = BRIGHTNESS_RAMP_STEP_PERMILLE;
        } else if (step < -BRIGHTNESS_RAMP_STEP_PERMILLE) {
            step = -BRIGHTNESS_RAMP_STEP_PERMILLE;
        }
        uint16_t permille = (uint16_t)((int32_t)s_last_applied_permille + step);
        bsp_display_set_brightness_permille(permille);
        s_last_applied_permille = permille;
    }

    /* First-run visibility: log every reading for a while after boot so
     * the actual sensor response and chosen brightness can be watched
     * (and config_web.c's sliders tuned) against real numbers, then drop
     * back to logging only on an actual change - same "log on change"
     * pattern presence_sensor.c already uses. Also piggybacks internal-RAM
     * free/largest-block onto this same line (rather than a separate log
     * source) while chasing a real crash seen 2026-09-09: a Wi-Fi PHY
     * calibration ESP_ERROR_CHECK(ESP_ERR_NO_MEM) abort a few seconds into
     * boot, in phy_track_pll_init() off the power-save wake path - internal
     * DRAM exhaustion, not a light_sensor/backlight bug as such, but this
     * is the fastest way to see whether adding light_sensor's I2C device +
     * the new LEDC channel is what tipped an already-marginal internal-RAM
     * budget (see the pre-existing TEMPORARY diag block below, tracking a
     * separate unexplained internal-RAM drop) over the edge. Remove once
     * that's confirmed either way. */
    if (s_diag_ticks_left > 0) {
        s_diag_ticks_left--;
        ESP_LOGI(TAG, "[brightness] raw=%.0f lux smoothed=%.0f lux -> target %.1f%% "
                      "applied %.1f%% (min=%.1f%% max_lux=%u) [diag] internal: %u free / %u largest block",
                 lux, s_lux_ema, target_permille / 10.0f, s_last_applied_permille / 10.0f,
                 (ui_get_cfg()->brightness_min_pct_x10
                      ? ui_get_cfg()->brightness_min_pct_x10
                      : APP_SETTINGS_DEFAULT_BRIGHTNESS_MIN_PCT_X10) / 10.0f,
                 ui_get_cfg()->brightness_max_lux,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else if (changed) {
        /* target and applied differing here (rather than logging just one
         * number) is the normal, expected look of a multi-tick ramp in
         * progress, not a bug - see BRIGHTNESS_RAMP_STEP_PERMILLE's own
         * comment. */
        ESP_LOGI(TAG, "[brightness] smoothed=%.0f lux -> target %.1f%% applied %.1f%%",
                 s_lux_ema, target_permille / 10.0f, s_last_applied_permille / 10.0f);
    }
}

/* TEMPORARY - tracking down an internal-RAM drop between refresh cycles
 * that isn't explained by any populate()/fetch code path (all checked and
 * correctly paired) - logs every 30s regardless of display state to see
 * whether the drop is a step (tied to one specific event) or a gradual
 * decline (pointing at Wi-Fi/TLS session state instead). Remove once
 * root-caused. */
static void check_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static uint32_t s_diag_ticks = 0;
    if (++s_diag_ticks % 30 == 0) {
        ESP_LOGI(TAG, "[diag] internal: %u free / %u largest block",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    uint32_t idle_ms = lv_disp_get_inactive_time(NULL);
    /* Inactive time dropping means a fresh touch reset it - the only way
     * back to the calendar from either DISPLAY_AMBIENT or DISPLAY_SLEEP. */
    bool touched = idle_ms < s_prev_idle_ms;
    bool presence = presence_sensor_is_detected();

    if (s_state != DISPLAY_SLEEP) {
        ambient_brightness_tick();
    }

    switch (s_state) {
    case DISPLAY_CALENDAR:
        if (s_idle_timeout_ms > 0 && idle_ms >= s_idle_timeout_ms) {
            if (presence && s_clock_feature_enabled) {
                go_ambient();
            } else {
                go_sleep();
            }
        }
        break;

    case DISPLAY_AMBIENT:
        if (touched) {
            go_calendar();
        } else if (presence) {
            s_presence_away_since_ms = 0;
            ui_clock_update();
        } else {
            if (s_presence_away_since_ms == 0) {
                s_presence_away_since_ms = lv_tick_get();
            }
            if (lv_tick_get() - s_presence_away_since_ms >= PRESENCE_AWAY_SLEEP_MS) {
                go_sleep();
            } else {
                ui_clock_update();
            }
        }
        break;

    case DISPLAY_SLEEP:
        if (touched) {
            go_calendar();
        } else if (presence && s_clock_feature_enabled) {
            go_ambient();
        } else if (lv_tick_get() - s_last_regen_ms >= SNOW_REGEN_MS) {
            regen_snow();
            s_last_regen_ms = lv_tick_get();
        }
        break;
    }

    s_prev_idle_ms = idle_ms;
}

void ui_screensaver_init(void)
{
    s_idle_timeout_ms = ui_get_cfg()->screen_timeout_s * 1000U;

    s_light_sensor_ok = (light_sensor_init(bsp_get_i2c_bus()) == ESP_OK);
    if (!s_light_sensor_ok) {
        ESP_LOGW(TAG, "no ambient light sensor - backlight brightness will stay "
                      "wherever it's already set (see light_sensor.c for the error)");
    }
    /* Baseline for the same [diag] internal free/largest numbers logged
     * every tick in ambient_brightness_tick() below - this one's from
     * before Wi-Fi's own init/connect/power-save cycle has touched
     * anything, so the two together show whether internal RAM was already
     * thin at this point or only got that way once Wi-Fi (net_task, spawned
     * right after calendar_ui_init() returns) started up. See that
     * function's comment for why this is being watched right now. */
    ESP_LOGI(TAG, "[diag] internal RAM at ui_screensaver_init(): %u free / %u largest block",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_wake_event = xEventGroupCreate();

    /* On lv_layer_top() so both float above whatever view/dialog is
     * currently showing, regardless of calendar_ui's own view switching. */
    s_clock = ui_clock_create(lv_layer_top());

    size_t buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(SS_H_RES, SS_V_RES);
    s_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (s_canvas_buf == NULL) {
        ESP_LOGE(TAG, "no memory for screensaver canvas (%u bytes) - sleep state disabled",
                 (unsigned)buf_size);
    } else {
        s_canvas = lv_canvas_create(lv_layer_top());
        lv_canvas_set_buffer(s_canvas, s_canvas_buf, SS_H_RES, SS_V_RES, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(s_canvas, 0, 0);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        /* Must be explicitly clickable - canvases aren't by default, so
         * without this the wake-up touch's hit-test skips straight past
         * the (visually opaque, but touch-transparent) noise overlay and
         * lands on whatever calendar cell/button is underneath, firing
         * its click handler even though the screen was dark.
         * lv_disp_get_inactive_time() still resets from the raw indev
         * activity either way, so wake detection is unaffected. */
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_timer_create(check_timer_cb, 1000, NULL);
}

bool calendar_ui_is_asleep(void)
{
    return s_state != DISPLAY_CALENDAR;
}

bool calendar_ui_wait_wake(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wake_event, WAKE_BIT, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));
    return (bits & WAKE_BIT) != 0;
}

void calendar_ui_request_sync(void)
{
    xEventGroupSetBits(s_wake_event, WAKE_BIT);
}
