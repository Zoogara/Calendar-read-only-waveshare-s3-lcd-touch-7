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

/* Drops the current association so the station re-scans and reconnects,
 * picking the strongest access point for the SSID - for when the link is
 * still "up" but carrying no traffic (see the comment in wifi_sta.c).
 * Returns immediately; reconnection happens in the background via the
 * existing disconnect handler. No-op if wifi_sta_connect() hasn't brought
 * the driver up yet. */
void wifi_sta_force_reconnect(void);

#ifdef __cplusplus
}
#endif
