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

/* Starts the AP + web form and BLOCKS until the user submits it, then
 * saves and calls esp_restart(). Never returns normally. */
void provisioning_run_portal(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
