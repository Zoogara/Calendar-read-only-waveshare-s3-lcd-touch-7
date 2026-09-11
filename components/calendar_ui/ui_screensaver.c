/* Idle screensaver: after IDLE_TIMEOUT_MS with no touch, the backlight
 * turns off and a full-screen random noise ("snow") pattern replaces the
 * calendar view instead of leaving it frozen. The pattern is regenerated
 * periodically while asleep - even with the backlight off, a TFT panel
 * left showing one truly static frame for many hours can develop a subtle
 * image-retention "memory" effect from the liquid crystal matrix holding
 * the same voltage pattern the whole time, so this keeps the pixel data
 * (and therefore the per-pixel drive voltages) moving. Any touch wakes it
 * immediately.
 */
#include "calendar_ui_internal.h"
#include "calendar_ui.h"
#include "board_bsp.h"

#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define SS_H_RES 800
#define SS_V_RES 480

#define SNOW_REGEN_MS     (60U * 1000U)       /* repaint noise this often while asleep */
#define WAKE_BIT          (1 << 0)
#define LONG_SLEEP_RESET_MS (15U * 60U * 1000U) /* asleep >= this long -> wake to
                                                    today/Month instead of resuming
                                                    whatever view/date was showing */

static const char *TAG = "ui_screensaver";

/* Idle time before the backlight turns off - configurable via the
 * gear-icon settings dialog (cfg->screen_timeout_s), read once at
 * ui_screensaver_init() time; 0 means never sleep. */
static uint32_t s_idle_timeout_ms = APP_SETTINGS_DEFAULT_SCREEN_TIMEOUT_S * 1000U;

static lv_obj_t *s_canvas;
static void *s_canvas_buf;
static volatile bool s_asleep = false;
static uint32_t s_prev_idle_ms = 0;
static uint32_t s_last_regen_ms = 0;
static uint32_t s_sleep_start_ms = 0;
static EventGroupHandle_t s_wake_event;

static void regen_snow(void)
{
    if (s_canvas_buf == NULL) {
        return;
    }
    esp_fill_random(s_canvas_buf, LV_CANVAS_BUF_SIZE_TRUE_COLOR(SS_H_RES, SS_V_RES));
    lv_obj_invalidate(s_canvas);
}

static void go_to_sleep(void)
{
    s_asleep = true;
    s_sleep_start_ms = lv_tick_get();
    regen_snow();
    s_last_regen_ms = lv_tick_get();
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    bsp_display_backlight(false);
    /* Nothing on the calendar view is visible behind the noise pattern
     * (and the backlight's off besides), so this is free real estate:
     * release its rendered content the same way switching views does
     * (see calendar_ui.c's release_view()) instead of leaving a full set
     * of event blocks/badges sitting in RAM for however long the screen
     * stays asleep - could be all night. calendar_ui_restore_active_view()
     * in wake_up() below undoes this before anything is shown again. */
    calendar_ui_release_active_view();
    ESP_LOGI(TAG, "idle timeout - backlight off");
}

static void wake_up(void)
{
    s_asleep = false;
    uint32_t asleep_ms = lv_tick_get() - s_sleep_start_ms;
    if (asleep_ms >= LONG_SLEEP_RESET_MS) {
        ESP_LOGI(TAG, "asleep for %u ms (>= %u) - waking to today/Month view",
                 (unsigned)asleep_ms, (unsigned)LONG_SLEEP_RESET_MS);
        calendar_ui_reset_to_today_month();
    } else {
        calendar_ui_restore_active_view();
    }
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    /* Explicit full-screen invalidate, not just whatever area hiding the
     * canvas invalidates on its own - under this panel's direct_mode +
     * avoid_tearing dual-framebuffer setup, a partial invalidate can land
     * on only one of the two buffers, leaving the other showing stale
     * content (the snow pattern) until something else forces a second
     * full redraw. Same root cause as the earlier top-bar title tearing
     * fix (see s_title_label's fixed-width comment in calendar_ui.c).
     *
     * A single invalidate still only guarantees ONE of the two buffers
     * gets flushed by the time this function returns (LVGL's normal
     * redraw timer only fires - and only swaps to the other buffer - on
     * its own next tick), which is why the snow pattern occasionally kept
     * showing after wake even with the invalidate above: hiding the
     * canvas already stops it from intercepting touches immediately (it's
     * evaluated synchronously against the HIDDEN flag), but the pixels
     * left behind on the buffer that hadn't been flushed yet stayed
     * visible until something forced a second redraw - i.e. exactly the
     * old "snow doesn't clear until you tap again" bug, just for taps
     * instead of the wake touch itself. Forcing two synchronous
     * lv_refr_now() passes here guarantees both buffers are flushed with
     * the post-wake content before this function returns, so there's no
     * gap where the display and the (already-updated) input routing
     * disagree with each other. */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    bsp_display_backlight(true);
    ESP_LOGI(TAG, "touch detected - backlight on");
    xEventGroupSetBits(s_wake_event, WAKE_BIT);
}

/* TEMPORARY - tracking down an internal-RAM drop between refresh cycles
 * that isn't explained by any populate()/fetch code path (all checked and
 * correctly paired) - logs every 30s regardless of sleep state to see
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

    if (s_asleep) {
        /* Inactive time dropping means a fresh touch reset it. */
        if (idle_ms < s_prev_idle_ms) {
            wake_up();
        } else if (lv_tick_get() - s_last_regen_ms >= SNOW_REGEN_MS) {
            regen_snow();
            s_last_regen_ms = lv_tick_get();
        }
    } else if (s_idle_timeout_ms > 0 && idle_ms >= s_idle_timeout_ms) {
        go_to_sleep();
    }

    s_prev_idle_ms = idle_ms;
}

void ui_screensaver_init(void)
{
    s_idle_timeout_ms = ui_get_cfg()->screen_timeout_s * 1000U;

    s_wake_event = xEventGroupCreate();

    size_t buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(SS_H_RES, SS_V_RES);
    s_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (s_canvas_buf == NULL) {
        ESP_LOGE(TAG, "no memory for screensaver canvas (%u bytes) - screensaver disabled",
                 (unsigned)buf_size);
        return;
    }

    /* On lv_layer_top() so it floats above whatever view/dialog is
     * currently showing, regardless of calendar_ui's own view switching. */
    s_canvas = lv_canvas_create(lv_layer_top());
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, SS_H_RES, SS_V_RES, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    /* Must be explicitly clickable - canvases aren't by default, so
     * without this the wake-up touch's hit-test skips straight past the
     * (visually opaque, but touch-transparent) noise overlay and lands on
     * whatever calendar cell/button is underneath, firing its click
     * handler even though the screen was dark. Capturing the touch here
     * consumes it - lv_disp_get_inactive_time() still resets from the
     * raw indev activity either way, so wake detection is unaffected. */
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

    lv_timer_create(check_timer_cb, 1000, NULL);
}

bool calendar_ui_is_asleep(void)
{
    return s_asleep;
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
