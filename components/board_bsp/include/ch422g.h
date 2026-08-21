#pragma once
/*
 * Minimal driver for the WCH CH422G I2C IO expander used on the
 * ESP32-S3-Touch-LCD-7 to provide the extra output lines the RGB LCD bus
 * doesn't leave GPIOs for (LCD backlight enable, GT911 touch reset,
 * TF-card CS, USB/CAN pin-mux select).
 *
 * The CH422G doesn't work like a typical PCF8574 (one I2C address +
 * register offsets); it exposes each internal register as its own I2C
 * "device address" on the bus. The addresses/values below come from the
 * public ESPHome CH422G component (community-reverse-engineered, since
 * WCH's datasheet is Chinese-only and light on I2C protocol detail) -
 * cross-check against Waveshare's own demo repo for this board if a pin
 * doesn't behave as expected, in particular the EXIOn -> bit-position
 * mapping, which is assumed to be bit N = EXIOn.
 *
 * We only ever use EXIO0-7 (the "IO" range); EXIO8-11 (open-drain only,
 * different register) aren't wired to anything on this board.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pin numbers used on the ESP32-S3-Touch-LCD-7, for readability at call sites. */
#define CH422G_EXIO_TP_RST   1   /* GT911 touch controller reset (active low) */
#define CH422G_EXIO_LCD_BL   2   /* LCD backlight enable (active high) */
#define CH422G_EXIO_LCD_RST  3   /* RGB LCD panel reset (active low) - confirmed
                                     against a working ESPHome config for this
                                     board (rpi_dpi_rgb's reset_pin, wired to
                                     ch422g io_ex EXIO3). esp_lcd's RGB panel
                                     driver has no reset_gpio_num field of its
                                     own, so this must be pulsed manually
                                     before the panel is used - without it,
                                     the panel never came out of reset and
                                     fell back to its own built-in colour-bar
                                     test pattern regardless of RGB timing. */
#define CH422G_EXIO_SD_CS    4   /* TF card SPI chip-select - not used by this
                                     firmware, see README for why SD support
                                     was left out */
#define CH422G_EXIO_USB_SEL  5   /* USB/CAN pin-mux select - not used */

typedef struct ch422g_dev_t *ch422g_handle_t;

/* Attaches the CH422G to an already-initialized I2C master bus and puts
 * EXIO0-7 into push-pull output mode. */
esp_err_t ch422g_init(i2c_master_bus_handle_t bus, ch422g_handle_t *out_handle);

/* Sets a single EXIO0-7 pin high/low. Read-modify-write against a cached
 * shadow register (the CH422G output register can't be read back
 * reliably while in output mode), so this is safe to call repeatedly
 * without disturbing other pins. */
esp_err_t ch422g_set_level(ch422g_handle_t handle, int exio_pin, bool level);

#ifdef __cplusplus
}
#endif
