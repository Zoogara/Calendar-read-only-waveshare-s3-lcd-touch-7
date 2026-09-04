#pragma once
/*
 * TF/SD card driver for the Waveshare ESP32-S3-Touch-LCD-7.
 *
 * The slot is wired for SPI/MMC: GPIO11=MOSI, GPIO12=SCK, GPIO13=MISO
 * (dedicated bus - nothing else on this board shares those three pins).
 * SD_CS is NOT a native ESP32 GPIO: it's EXIO4 on the CH422G I2C IO
 * expander (CH422G_EXIO_SD_CS in ch422g.h), same expander that already
 * handles the LCD reset, touch reset, and backlight enable.
 *
 * Because the SPI bus is dedicated solely to the card, CS is asserted
 * once at mount time and left low for the whole session, rather than
 * toggled per SPI transaction - see sd_card_init()'s comment for why.
 */

#include "esp_err.h"
#include "ch422g.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CARD_MOUNT_POINT "/sdcard"

/* Mounts the TF card as a FAT filesystem at SD_CARD_MOUNT_POINT.
 *
 * `expander` must be the same CH422G handle bsp_display_init() created
 * (get it via bsp_get_expander(), after bsp_display_init() has
 * succeeded) - re-initializing a second CH422G handle on the same I2C
 * bus/address would fail, since i2c_master_bus_add_device() refuses a
 * duplicate device at an address already registered.
 *
 * CS handling: the sdspi driver normally toggles chip-select itself via
 * a native GPIO on every SPI transaction. SD_CS here lives on the
 * CH422G instead, which the SPI peripheral can't drive directly, and
 * round-tripping over I2C for every single SD transfer would be far
 * slower than the SPI bus itself. Since GPIO11/12/13 are dedicated to
 * this card alone (no other device to conflict with), sd_card_init()
 * asserts SD_CS low once via the CH422G and then tells the sdspi driver
 * "no CS line for you to manage" (gpio_cs = GPIO_NUM_NC) - the same
 * mechanism ESP-IDF documents for boards where CS is hardwired to
 * ground. CS is only released (driven high) again in sd_card_deinit().
 *
 * Call after bsp_display_init(). Not thread-safe with itself - call
 * once from app_main-level init code. */
esp_err_t sd_card_init(ch422g_handle_t expander);

/* Unmounts the filesystem, releases SD_CS (drives it high via the
 * CH422G), and frees the SPI bus. Safe to call even if sd_card_init()
 * failed partway through, or was never called. */
void sd_card_deinit(void);

#ifdef __cplusplus
}
#endif
