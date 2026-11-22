#pragma once
/* Stand-in: the renderer has no network, and ui_radar.c's loader never runs there. Every call fails. */
#include <stdint.h>
#include "esp_err.h"
typedef struct { const char *url; int timeout_ms; int buffer_size; int buffer_size_tx; } esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) { (void)c; return 0; }
static inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t h, const char *k, const char *v) { (void)h; (void)k; (void)v; return ESP_FAIL; }
static inline esp_err_t esp_http_client_open(esp_http_client_handle_t h, int w) { (void)h; (void)w; return ESP_FAIL; }
static inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) { (void)h; return -1; }
static inline int esp_http_client_get_status_code(esp_http_client_handle_t h) { (void)h; return 0; }
static inline int esp_http_client_read(esp_http_client_handle_t h, char *b, int n) { (void)h; (void)b; (void)n; return -1; }
static inline esp_err_t esp_http_client_close(esp_http_client_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { (void)h; return ESP_OK; }
