#pragma once
/*
 * Runtime configuration web server - everything configurable except
 * Wi-Fi, reachable over the LAN while the device is up and running
 * normally. Distinct from provisioning.h's AP-mode setup portal, which
 * only ever runs before Wi-Fi is configured at all and is the only place
 * Wi-Fi credentials themselves can be set.
 *
 * Password-protected via HTTP Basic Auth - only the password half of the
 * challenge is checked (any username, including none, is accepted), and
 * the password itself is set on-device (gear icon -> Display, see
 * ui_settings_dialog.c), never over the web form. If no password is
 * configured, the server refuses to start at all rather than running
 * unauthenticated.
 */

#include "app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the config web server on the STA interface (call once Wi-Fi is
 * connected). cfg is kept by reference and read/written live as the form
 * is submitted - the caller must keep it valid for the lifetime of the
 * device (main.c's s_cfg already is). No-op if cfg->config_web_password
 * is empty, or if already started. */
void config_web_start(app_settings_t *cfg);

#ifdef __cplusplus
}
#endif
