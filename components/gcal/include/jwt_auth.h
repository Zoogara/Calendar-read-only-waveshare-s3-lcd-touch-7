#pragma once
/*
 * Google service-account auth (RFC 7523 JWT bearer flow), the
 * "server-to-server" OAuth path: no browser/consent step is needed at
 * runtime because the user grants access once, out of band, by sharing
 * each calendar with the service account's email address (see README).
 *
 * The device signs its own JWT with the service account's RSA private
 * key (mbedtls, hardware-accelerated SHA/MPI on the S3) and trades it
 * for a short-lived OAuth access token.
 */

#include <stddef.h>
#include "esp_err.h"
#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a valid (not-yet-expired) Bearer access token in out_token,
 * fetching/refreshing a new one from Google if the cached one is missing
 * or about to expire. Thread-safe. */
esp_err_t jwt_auth_get_token(const app_settings_t *cfg, char *out_token, size_t out_sz);

#ifdef __cplusplus
}
#endif
