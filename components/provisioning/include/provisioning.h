#pragma once
/*
 * First-boot setup: if no valid config is found in NVS, we bring the
 * ESP32 up as a Wi-Fi access point ("GCal-Display-Setup") serving a
 * single HTML form (SSID/password, Google service-account details,
 * calendar list + colours, timezone). Submitting the form saves to NVS
 * and reboots into normal station-mode operation.
 *
 * This mirrors a plain "Wi-Fi AP + browser" setup flow rather than
 * anything Bluetooth-based - simplest thing that works reliably from any
 * phone/laptop with no companion app.
 */

#include "esp_err.h"
#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROVISIONING_AP_SSID   "GCal-Display-Setup"
/* Open AP (no password) since it's only reachable by someone standing
 * next to the device during first-time setup; change PROVISIONING_AP_PSK
 * below (8+ chars) if you'd rather it be protected. */
#define PROVISIONING_AP_PSK    ""

/* Loads settings previously saved by the portal. Returns ESP_ERR_NOT_FOUND
 * (with *out.valid == false) if nothing has been configured yet. */
esp_err_t provisioning_load(app_settings_t *out);

/* Persists settings to NVS. */
esp_err_t provisioning_save(const app_settings_t *cfg);

/* Wipes any saved configuration (e.g. to force re-running the portal). */
esp_err_t provisioning_clear(void);

/* --- SD card config backup (components/provisioning/prov_store.c) ---
 *
 * A secondary copy of the config, written as JSON to the TF card at
 * PROVISIONING_SD_DIR, independent of NVS. The point is surviving
 * flashing other/test firmware onto the board and then reflashing this
 * one: unlike NVS (which another firmware image might reuse, overwrite,
 * or never touch in a way that leaves it stale), the SD card is untouched
 * by whatever else got flashed, so this file acts as a stable "known
 * good" config to fall back on without re-running the setup portal.
 *
 * provisioning_save() (NVS) does NOT also call provisioning_save_sd() -
 * the card is only mounted in a narrow window early in main.c's boot
 * sequence (deinited right after, to free its internal-RAM/SPI-bus
 * footprint for the rest of that boot's uptime), so by the time any real
 * caller of provisioning_save() runs - the settings dialog, the LAN
 * config web server, the setup portal, all well after boot - the card is
 * already unmounted and a write would just fail silently. Confirmed
 * on-device 2026-09-08 as exactly why an added calendar "disappeared":
 * the NVS save succeeded, the (then-attempted) SD save didn't, silently.
 * main.c instead refreshes this file unconditionally on every boot, while
 * the card is mounted anyway - every existing save path already calls
 * esp_restart() right after a successful save, so this keeps the two in
 * sync within seconds of any real change, without mounting the card again
 * mid-session (see the settling-delay comment in main.c for why that
 * matters).
 *
 * All of these require the SD card to already be mounted at
 * SD_CARD_MOUNT_POINT (sd_card_init() in components/sd_card) - they
 * return an error (or false) rather than crashing if it isn't, but the
 * caller is expected to check sd_card_init()'s own result first rather
 * than relying on that. */

#define PROVISIONING_SD_DIR   "/sdcard/gcal"

/* Creates PROVISIONING_SD_DIR on the SD card if it doesn't already exist.
 * Safe to call every boot; a no-op once the directory exists. */
esp_err_t provisioning_sd_ensure_dir(void);

/* True if a config backup file already exists at PROVISIONING_SD_DIR. */
bool provisioning_sd_config_exists(void);

/* Loads settings from the SD card backup file. Returns ESP_ERR_NOT_FOUND
 * (with *out.valid == false) if the file is missing, unreadable, or
 * doesn't parse into a valid config - same contract as provisioning_load(). */
esp_err_t provisioning_load_sd(app_settings_t *out);

/* Writes cfg to the SD card backup file, creating or overwriting it.
 * main.c calls this unconditionally on every boot to keep the backup in
 * sync with NVS - see the file-level comment above for why. */
esp_err_t provisioning_save_sd(const app_settings_t *cfg);

/* Starts the AP + web form and BLOCKS until the user submits it, then
 * saves and calls esp_restart(). Never returns normally. */
void provisioning_run_portal(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
