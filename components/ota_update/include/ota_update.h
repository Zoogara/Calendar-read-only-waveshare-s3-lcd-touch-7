#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Downloads the firmware .bin at `url` over HTTPS and writes it to the
 * inactive OTA partition. On ESP_OK, the new image is already set as the
 * boot partition for the next restart - the caller is responsible for
 * calling esp_restart() when it's ready to. Blocking; expect this to take
 * tens of seconds. No version check - always re-downloads and flashes
 * whatever is at the URL, so only call this when you know something new
 * has actually been published there.
 *
 * Must be called from a task with an internal-RAM stack, not a PSRAM one -
 * the flash write this performs needs the flash cache briefly disabled,
 * which asserts if the calling task's own stack isn't reachable during
 * that window. Same constraint as reconfigure_task in calendar_ui.c. */
esp_err_t ota_update_check_and_apply(const char *url);

#ifdef __cplusplus
}
#endif
