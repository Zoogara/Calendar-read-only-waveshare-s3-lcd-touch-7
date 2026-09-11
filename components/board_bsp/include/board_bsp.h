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

/* Turns the backlight on/off (it starts on at the end of bsp_display_init). */
esp_err_t bsp_display_backlight(bool on);

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

#ifdef __cplusplus
}
#endif
