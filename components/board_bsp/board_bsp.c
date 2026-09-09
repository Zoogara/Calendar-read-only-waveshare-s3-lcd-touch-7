#include "board_bsp.h"
#include "ch422g.h"

#include <string.h>
#include "esp_log.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_bsp";

/* ---- I2C (shared bus: CH422G expander + GT911 touch) ---- */
#define I2C_SDA_GPIO   8
#define I2C_SCL_GPIO   9
#define I2C_PORT       0

/* ---- Touch (GT911) ---- */
#define TOUCH_IRQ_GPIO 4

/* ---- RGB LCD panel timing ---- */
#define LCD_H_RES      800
#define LCD_V_RES      480
/* The real bring-up bug that caused the panel to show its own built-in
 * colour-bar test pattern (regardless of timing) was a missing LCD reset
 * pulse - see CH422G_EXIO_LCD_RST / lcd_reset_pulse() - not these timing
 * values. With that fixed, these values (from a community report for this
 * exact panel/pinout - lvgl_micropython repo, discussion #333) tested rock
 * solid on real hardware, with no jitter, over a working ESPHome
 * `rpi_dpi_rgb` config for this same board (16MHz pclk, hsync pulse/back/
 * front = 4/8/8, vsync pulse/back/front = 4/16/16, pclk_active_neg = true)
 * which had occasional jitter. That community report also notes >=13.0MHz
 * causes a slow rightward creep on this panel - stay under it. */
#define LCD_PCLK_HZ    (12900 * 1000)

/* Data-bit order matches the ESP32-S3 <-> LCD signal table on Waveshare's
 * wiki: index 0 = blue LSB (panel pin "B3") ... index 15 = red MSB
 * (panel pin "R7"), i.e. B3..B7, G2..G7, R3..R7 for RGB565. */
static const int s_lcd_data_gpios[16] = {
    14, 38, 18, 17, 10,   /* B3, B4, B5, B6, B7 */
    39, 0, 45, 48, 47, 21, /* G2, G3, G4, G5, G6, G7 */
    1, 2, 42, 41, 40,     /* R3, R4, R5, R6, R7 */
};
#define LCD_HSYNC_GPIO 46
#define LCD_VSYNC_GPIO 3
#define LCD_DE_GPIO    5
#define LCD_PCLK_GPIO  7

/* ---- Backlight dimming ----
 * The CH422G's EXIO2 (CH422G_EXIO_LCD_BL) only gates power to the
 * backlight boost driver on/off - confirmed on real hardware that a
 * separate test point on the driver is a genuine PWM *dimming* input,
 * unconnected to any ESP32-S3 pin from the factory. GPIO16 is free (not
 * used anywhere else in s_lcd_data_gpios/the touch or SD wiring above),
 * so it's wired to that test point to drive dimming via LEDC, with the
 * CH422G gate left in place exactly as the board ships (no need to
 * isolate/cut anything) and still used for a hard, zero-current off. */
#define BACKLIGHT_PWM_GPIO     16
#define BACKLIGHT_LEDC_MODE    LEDC_LOW_SPEED_MODE /* ESP32-S3 LEDC has no high-speed mode */
#define BACKLIGHT_LEDC_TIMER   LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
/* 1220Hz + 14-bit (0-16383 duty steps) rather than the original 5kHz +
 * 10-bit: matches ESPHome's own LEDC output platform recommendation for
 * LED/backlight dimming specifically (their default is 1kHz, but they
 * recommend ~1220Hz because that's the frequency where the ESP32
 * family's LEDC timer can hit its own maximum duty resolution against an
 * 80MHz APB clock - 14 bits on S2/S3/C3, SOC_LEDC_TIMER_BIT_WIDTH -
 * rather than the timer needing to trade resolution away to hit a
 * higher frequency). Still well above visible flicker and low enough not
 * to fight the boost driver's own switching, same reasoning as the
 * original 5kHz choice - the actual motivation for the change is the 16x
 * finer duty steps (16384 vs 1024) it buys at the low end, where this
 * board's measured hard cutoff (see brightness_min_pct_x10's comment in
 * app_settings.h) leaves very little room to work with at 10-bit. */
#define BACKLIGHT_LEDC_RES     LEDC_TIMER_14_BIT
#define BACKLIGHT_LEDC_FREQ_HZ 1220

static i2c_master_bus_handle_t s_i2c_bus;
static ch422g_handle_t s_expander;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_disp_t *s_disp;
static lv_indev_t *s_indev;
static bool s_bl_powered; /* current state of the CH422G backlight-enable gate */

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

/* esp_lcd's RGB panel driver has no reset_gpio_num field of its own - this
 * board's LCD reset line is routed through the CH422G expander instead
 * (EXIO3, active low), so it has to be pulsed manually before the panel is
 * configured. Timing mirrors the touch controller's reset pulse below. */
static void lcd_reset_pulse(void)
{
    ch422g_set_level(s_expander, CH422G_EXIO_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    ch422g_set_level(s_expander, CH422G_EXIO_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t lcd_panel_init(void)
{
    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            /* See LCD_PCLK_HZ comment above. */
            .hsync_pulse_width = 2,
            .hsync_back_porch = 4,
            .hsync_front_porch = 4,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 4,
            .vsync_front_porch = 4,
            .flags.pclk_active_neg = false,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .psram_trans_align = 64,
        /* NOTE: a bounce-buffer (small internal-SRAM staging buffers copied
         * from the PSRAM frame buffer in the DMA EOF ISR) was tried here to
         * fight jitter, but ESP-IDF's own docs are explicit that this mode
         * "CAN NOT work if we disable the cache of the external memory, via
         * e.g. OTA or NVS write to the main flash" - and on real hardware it
         * reliably panics ("Cache disabled but cached memory region
         * accessed") the moment Wi-Fi's NVS write happens at boot. Fixing
         * that properly needs CONFIG_SPIRAM_FETCH_INSTRUCTIONS +
         * CONFIG_SPIRAM_RODATA (PSRAM XIP) so flash writes never disable the
         * PSRAM cache - a bigger, riskier change than jitter alone
         * justifies. Straight double-buffering in PSRAM (below) is what the
         * RGB LCD driver docs call the simplest anti-tearing option; PSRAM
         * bandwidth contention is mitigated instead by keeping Wi-Fi's
         * buffers out of PSRAM (see CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP in
         * sdkconfig.defaults) and, if jitter persists, by lowering
         * LCD_PCLK_HZ. */
        .hsync_gpio_num = LCD_HSYNC_GPIO,
        .vsync_gpio_num = LCD_VSYNC_GPIO,
        .de_gpio_num = LCD_DE_GPIO,
        .pclk_gpio_num = LCD_PCLK_GPIO,
        .disp_gpio_num = -1, /* no separate "display enable" pin - backlight
                                 is switched via the CH422G instead */
        .flags.fb_in_psram = true,
    };
    memcpy(panel_cfg.data_gpio_nums, s_lcd_data_gpios, sizeof(s_lcd_data_gpios));

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    return ESP_OK;
}

static esp_err_t backlight_pwm_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_RES,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BACKLIGHT_PWM_GPIO,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "backlight PWM ready (GPIO%d, %dHz, %d-bit)",
             BACKLIGHT_PWM_GPIO, BACKLIGHT_LEDC_FREQ_HZ, BACKLIGHT_LEDC_RES);
    return ESP_OK;
}

static esp_err_t touch_init(void)
{
    /* GT911 reset is wired through the CH422G, not a native GPIO, so we
     * pulse it ourselves instead of handing rst_gpio_num to the touch
     * driver. GT911 datasheet reset timing: hold reset low >=10ms then
     * release. */
    ch422g_set_level(s_expander, CH422G_EXIO_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    ch422g_set_level(s_expander, CH422G_EXIO_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    /* Current (IDF ~5.3+) esp_lcd_new_panel_io_i2c() takes the
     * i2c_master_bus_handle_t straight from i2c_new_master_bus() - no
     * cast needed. If you're on IDF 5.2, this call took an
     * esp_lcd_i2c_bus_handle_t/legacy i2c_port_t instead; check
     * esp_lcd_panel_io_i2c.h for your installed version if this doesn't
     * compile. */
    esp_err_t err = esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch panel IO init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,   /* handled above via CH422G */
        .int_gpio_num = TOUCH_IRQ_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_touch);
}

static esp_err_t lvgl_init(void)
{
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* Default is 7168, which calendar_ui's view-populate functions (nav
     * button click -> event callback -> render_current_view -> e.g.
     * ui_month_populate/ui_week_populate) came close to or exceeded even
     * after moving their large gcal_event_t buffers off the stack (see
     * ui_month.c and friends) - this task also does all touch input
     * handling, so overflowing it silently corrupted rendering AND made
     * touch stop responding at the same time. Week view in particular
     * builds roughly 3x the widgets Month view does (day headers, hourly
     * grid lines, per-day columns, event blocks), so it needs more margin
     * still. Putting this task's stack in PSRAM (CONFIG_SPIRAM_ALLOW_STACK_
     * EXTERNAL_MEMORY is already enabled for this board) instead of the
     * default internal RAM means a generous stack size here doesn't eat
     * into the same scarce internal-RAM pool mbedtls needs for TLS
     * handshakes - internal RAM was already down to ~50KB free with the
     * previous internal-RAM 16KB stack, which is almost certainly why
     * mbedtls_ssl_setup started intermittently failing with ALLOC_FAILED. */
    lvgl_cfg.task_stack = 32768;
    lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = NULL,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * 100, /* only used to size a draw buffer
                                            when avoid_tearing is off; with it
                                            on (below), esp_lvgl_port instead
                                            draws straight into the panel's
                                            own two PSRAM frame buffers */
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            /* Required for avoid_tearing (below) to do anything at all:
             * esp_lvgl_port's flush function only actually waits on the
             * VSYNC-signalled semaphore when drv->direct_mode or
             * drv->full_refresh is set (see lvgl_port_flush_callback in
             * esp_lvgl_port_disp.c) - without this, avoid_tearing=true
             * still routes draws into the panel's own frame buffers, but
             * every flush returns immediately with no synchronization to
             * the RGB DMA's scan position, which is a no-op in practice
             * and produces the exact same tearing as avoid_tearing=false
             * (confirmed on real hardware: fast double-taps only visibly
             * registered every other tap, and LVGL's scroll animation on
             * Week view tore badly, both because flushes were racing the
             * DMA read with no sync point at all).
             *
             * full_refresh (redraw the whole screen every flush) was tried
             * as an alternative to direct_mode and did stop a torn strip
             * across the top ~1/8 of the screen on every navigation - but
             * esp_lvgl_port runs touch input and rendering on the same
             * task (taskLVGL), and full_refresh's much slower redraw+VSYNC-
             * wait per flush meant a tap's press/release could land
             * entirely inside that blocked window and never get read,
             * making navigation need 2+ taps per actual step. direct_mode
             * (Espressif's own reference example default for this exact
             * avoid_tearing + double-buffered-RGB-panel combination,
             * esp_lvgl_port's examples/rgb_lcd) only redraws changed areas,
             * so its flushes are fast enough not to swallow touches - the
             * top-strip tear it left behind turned out to correlate with
             * calendar_ui.c's title label (UI_TOP_BAR_H=56px is almost
             * exactly "top 1/8"), which auto-sizes to its text and was
             * changing length on every navigation ("August 2026" vs "Aug
             * 17 - 23, 2026" etc.) - fixed at the source by giving it a
             * fixed width instead (see build_top_bar in calendar_ui.c). */
            .direct_mode = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = false,        /* full PSRAM frame buffers, see lcd_panel_init */
            /* With this, esp_lvgl_port draws directly into the RGB panel's
             * own two PSRAM frame buffers (from num_fbs=2 in
             * lcd_panel_init) instead of writing into whichever buffer is
             * currently on screen - see the direct_mode comment above for
             * why direct_mode must also be set for this to actually
             * synchronize to VSYNC rather than being a no-op. */
            .avoid_tearing = true,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed - if this doesn't "
                      "compile against your esp_lvgl_port version, fall "
                      "back to plain lvgl_port_add_disp() with the same "
                      "disp_cfg (see README troubleshooting)");
        return ESP_FAIL;
    }
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = s_touch,
    };
    s_indev = lvgl_port_add_touch(&touch_cfg);
    if (s_indev == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(ch422g_init(s_i2c_bus, &s_expander));
    lcd_reset_pulse();
    ESP_ERROR_CHECK(lcd_panel_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(lvgl_init());
    ESP_ERROR_CHECK(backlight_pwm_init());
    ESP_ERROR_CHECK(bsp_display_backlight(true));
    ESP_LOGI(TAG, "display + touch + LVGL ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

esp_err_t bsp_display_set_brightness_permille(uint16_t permille)
{
    if (permille > 1000) {
        permille = 1000;
    }

    if (permille == 0) {
        /* Duty to 0 before cutting power, not after - avoids a brief
         * window where the gate is live but the PWM channel is still
         * mid-cycle from whatever duty was last set. */
        ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, 0);
        ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
        esp_err_t err = ch422g_set_level(s_expander, CH422G_EXIO_LCD_BL, false);
        if (err == ESP_OK) {
            s_bl_powered = false;
        }
        return err;
    }

    if (!s_bl_powered) {
        esp_err_t err = ch422g_set_level(s_expander, CH422G_EXIO_LCD_BL, true);
        if (err != ESP_OK) {
            return err;
        }
        s_bl_powered = true;
        /* Let the boost driver reach regulation before also asking it to
         * track a PWM duty - same settle-delay idea as lcd_reset_pulse()/
         * touch_init()'s reset pulses above, just much shorter since
         * there's no reset line involved, only power settling. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    uint32_t max_duty = (1u << BACKLIGHT_LEDC_RES) - 1;
    uint32_t duty = (max_duty * permille) / 1000;
    esp_err_t err = ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
    }
    return err;
}

esp_err_t bsp_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return bsp_display_set_brightness_permille((uint16_t)percent * 10);
}

esp_err_t bsp_display_backlight(bool on)
{
    return bsp_display_set_brightness(on ? 100 : 0);
}

bool bsp_lvgl_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_lvgl_unlock(void)
{
    lvgl_port_unlock();
}

ch422g_handle_t bsp_get_expander(void)
{
    return s_expander;
}

i2c_master_bus_handle_t bsp_get_i2c_bus(void)
{
    return s_i2c_bus;
}
