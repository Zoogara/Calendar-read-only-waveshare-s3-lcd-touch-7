#pragma once
/* Just enough of ESP-IDF for the host tests. */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_EAGAIN 0x7001
static inline const char *esp_err_to_name(esp_err_t e) { return e == 0 ? "ESP_OK" : "ESP_ERR"; }
