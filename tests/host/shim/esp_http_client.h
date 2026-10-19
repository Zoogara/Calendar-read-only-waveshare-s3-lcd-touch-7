#pragma once
/* A stand-in for the ESP-IDF HTTP client: fake_http.c serves canned text. */
#include "esp_err.h"
typedef struct fake_http_client *esp_http_client_handle_t;
typedef enum { HTTP_METHOD_GET = 0 } esp_http_client_method_t;
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *data);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
