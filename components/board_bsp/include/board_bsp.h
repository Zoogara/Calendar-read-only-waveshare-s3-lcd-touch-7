#pragma once
/*
 * Board support for the Waveshare ESP32-S3-Touch-LCD-7:
 *   - 800x480 RGB565 parallel IPS panel (ST7262 controller, driven
 *     directly by the ESP32-S3's LCD_CAM peripheral - no SPI/QSPI init
 *     sequence needed, just timings)
 *   - GT911 capacitive touch over I2C
 *   - CH422G I2C IO expander for backlight enable + touch reset + LCD
 *     reset + TF-card chip-select
 *   - LVGL wired up on top via esp_lvgl_port
 *
 * Reference: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7
 * Pin mapping and panel timings below were taken from that page; if the
 * picture is shifted/rolling or colours look swapped on real hardware,
 * that's almost always a timing or data-bit-order tweak, not a logic bug -
 * see README "Bring-up troubleshooting".
 */

#include "esp_err.h"
#include "lvgl.h"
#include "ch422g.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up I2C (shared by CH422G + GT911), the RGB LCD panel, the touch
 * controller, and LVGL (display + input device + tick + task). Safe to
 * call once, early in app_main, before anything touches lv_* APIs.
 *
 * Returns a lock that MUST be held (bsp_lvgl_lock/bsp_lvgl_unlock) around
 * any lv_* call made from outside the LVGL task (e.g. when the Google
 * Calendar fetch task pushes new events into the UI).
 */
esp_err_t bsp_display_init(void);

/* Turns the backlight on/off (it starts on at the end of bsp_display_init).
 * A thin wrapper over bsp_display_set_brightness(on ? 100 : 0). */
esp_err_t bsp_display_backlight(bool on);

/* Sets backlight brightness as a whole percentage (0-100, clamped) - a
 * thin wrapper over bsp_display_set_brightness_permille(percent * 10) for
 * callers that don't need finer-than-1% control (bsp_display_backlight()'s
 * plain on/off, mainly). See that function's own comment for the
 * mechanism (CH422G gate vs LEDC PWM duty) - it's identical here, just at
 * coarser granularity. */
esp_err_t bsp_display_set_brightness(uint8_t percent);

/* Sets backlight brightness in tenths of a percent (0-1000, clamped, i.e.
 * 0.0%-100.0% in 0.1% steps). Finer than bsp_display_set_brightness()'s
 * whole-percent parameter can express - needed because the ambient
 * auto-dimming floor (app_settings_t's brightness_min_pct_x10) is
 * confirmed on real hardware to need adjustment within a fairly narrow
 * "dark zone" where a single whole percent is already a meaningful chunk
 * of the usable range, so ui_screensaver.c's ambient_brightness_tick()
 * uses this instead of the whole-percent version.
 *
 * 0 fully powers the backlight down via the CH422G's enable gate (EXIO2)
 * rather than just driving the PWM duty to 0 - the test point this drives
 * (GPIO16, via LEDC PWM) is a *dimming* input to the backlight boost
 * driver, not an independent supply, so it only has any effect while that
 * gate is also on; leaving the gate on at 0 duty would still draw
 * quiescent current for no visible benefit. Turning the backlight back on
 * from fully off re-enables the gate and waits briefly for the boost
 * driver to reach regulation before applying a duty, so calls after the
 * first one at a given power state are cheap (a single LEDC register
 * write, no I2C). See ch422g.h for the EXIO pin map and board_bsp.c's
 * lcd_reset_pulse()/touch reset for the same settle-delay pattern
 * elsewhere on this board. */
esp_err_t bsp_display_set_brightness_permille(uint16_t permille);

/* Must be held for any lv_* call made outside of LVGL's own task/timer
 * context. Re-entrant-safe is NOT assumed - don't nest. */
bool bsp_lvgl_lock(uint32_t timeout_ms);
void bsp_lvgl_unlock(void);

/* Returns the CH422G handle bsp_display_init() created. Other board-level
 * drivers that need the same expander - currently just sd_card, for
 * SD_CS (CH422G_EXIO_SD_CS) - reuse this handle instead of calling
 * ch422g_init() a second time, which would fail (i2c_master_bus_add_device
 * refuses to add a second device at an address already on the bus).
 * Valid only after bsp_display_init() has returned ESP_OK. */
ch422g_handle_t bsp_get_expander(void);

/* Returns the I2C bus handle bsp_display_init() created (shared by the
 * CH422G expander and GT911 touch controller). Other drivers that need
 * the same bus - currently just light_sensor, for the BH1750 ambient
 * light sensor - reuse this handle instead of calling
 * i2c_new_master_bus() a second time on the same SDA/SCL pins. Valid
 * only after bsp_display_init() has returned ESP_OK. */
i2c_master_bus_handle_t bsp_get_i2c_bus(void);

#ifdef __cplusplus
}
#endif
