#pragma once
/* Normal-operation Wi-Fi station connect, using credentials already
 * loaded into an app_settings_t (via provisioning_load). Separate from
 * provisioning.h because this runs every boot, not just first boot. */

#include <stdint.h>
#include "esp_err.h"
#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up esp_netif/event loop/Wi-Fi driver in STA mode, connects, and
 * blocks (with retries) until an IP address is obtained or
 * timeout_ms elapses. */
esp_err_t wifi_sta_connect(const app_settings_t *cfg, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
