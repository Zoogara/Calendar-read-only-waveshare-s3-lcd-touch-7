#pragma once
/*
 * BH1750FVI ambient light sensor (sold as the "GY-30" breakout module) on
 * the shared I2C bus - same bus as the CH422G expander and GT911 touch
 * controller, see board_bsp.c. Default I2C address 0x23 (ADDR pin
 * low/floating, which is how these modules ship) - doesn't collide with
 * anything else already on this bus (CH422G: 0x24/0x38/0x26, GT911: 0x5D
 * or 0x14).
 *
 * Drives backlight auto-dimming (see board_bsp.c's
 * bsp_display_set_brightness() and calendar_ui/ui_screensaver.c's
 * ambient_brightness_tick()) - mount the sensor away from the screen's
 * own light spill so it reads ambient room light, not the display
 * reflecting off nearby surfaces.
 *
 * Uses Continuous High-Resolution Mode (1 lx resolution, ~120ms
 * conversion time) rather than the One-Time modes: readings are polled
 * roughly once a second by ui_screensaver.c's existing timer regardless,
 * so there's no benefit to the sensor's own low-power one-shot cycle
 * here, and continuous mode means every read gets the latest conversion
 * instead of having to re-trigger and wait each time.
 */

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Adds the BH1750 as a device on `bus` (the same handle
 * bsp_get_i2c_bus() returns) and starts it in Continuous
 * High-Resolution Mode. Call once, after bsp_display_init() has brought
 * the I2C bus up. Logs and returns an error if the sensor doesn't ACK
 * (not wired, wrong address) - light_sensor_read_lux() then always fails
 * too, so auto-dimming just degrades to "leave the backlight wherever it
 * already was" rather than anything crashing. */
esp_err_t light_sensor_init(i2c_master_bus_handle_t bus);

/* Reads the most recent measurement and converts it to lux. Safe to call
 * about once a second (comfortably above the sensor's ~120ms continuous-
 * mode conversion cycle) - calling much faster than that just re-reads a
 * measurement the sensor hasn't updated yet. Returns
 * ESP_ERR_INVALID_STATE if light_sensor_init() wasn't called or failed. */
esp_err_t light_sensor_read_lux(float *out_lux);

#ifdef __cplusplus
}
#endif
