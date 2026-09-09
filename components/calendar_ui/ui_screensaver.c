/* Idle/presence-driven display states. Three of them:
 *
 *   - DISPLAY_CALENDAR: the normal, bright calendar. Only ever entered by
 *     an actual touch - presence alone never brings the calendar back.
 *   - DISPLAY_AMBIENT: a big ambient clock (ui_clock.c) replaces the
 *     calendar after the configured idle timeout, colour-cycled through
 *     the user's own calendar colours and dimmed for night hours (see
 *     ui_clock.c) - but only entered if presence is detected at that
 *     moment (and the clock feature is currently enabled - see
 *     s_clock_feature_enabled below). The backlight stays on the whole
 *     time; this board has no PWM dimming, so the clock's own colour
 *     choice against a black background is the only "dim" this state has.
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
        bsp_display_backlight(true);
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
        bsp_display_backlight(true);
    }
    full_double_refresh();
    ESP_LOGI(TAG, "touch detected - showing calendar");
    xEventGroupSetBits(s_wake_event, WAKE_BIT);
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
