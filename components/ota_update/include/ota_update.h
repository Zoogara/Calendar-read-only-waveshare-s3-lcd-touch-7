#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True if `url` is a plain http:// address (anything not https://). */
bool ota_update_url_is_insecure(const char *url);

/* True only in a development build (sdkconfig.dev, built with tools/dev_build.sh), which
 * can accept a plain-http firmware URL - and then only from ota_update_start_background(),
 * i.e. the authenticated /ota trigger. The default build is https-only everywhere. */
bool ota_update_http_allowed_in_build(void);

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

/* Runs the update on its own internal-RAM-stack task and restarts into the new image on
 * success; returns at once (ESP_OK once the task is started). `url` is copied. Used by the
 * config web server's /ota endpoint, so a build can be pushed without touching the device.
 * Plain http:// is accepted here - and only here - in a development build; returns
 * ESP_ERR_NOT_SUPPORTED for an http:// URL otherwise, and ESP_ERR_NO_MEM when there isn't
 * room for the task right now (the caller can retry). */
esp_err_t ota_update_start_background(const char *url);

/* With app rollback enabled, a freshly updated image only stays the boot
 * image once it's marked valid - otherwise the bootloader reverts to the
 * previous one on the next reset. Marks it valid `seconds` after this call,
 * so an image that crashes during start-up (or hangs the display for good)
 * rolls itself back, while a power cycle later with Wi-Fi down doesn't. */
void ota_update_confirm_boot_after(uint32_t seconds);

#ifdef __cplusplus
}
#endif
