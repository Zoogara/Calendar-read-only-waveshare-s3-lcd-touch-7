#pragma once
#include "esp_http_client.h"
/* What the next esp_http_client_perform() "downloads". */
void fake_http_serve(const char *body, int status, esp_err_t result);
esp_http_client_handle_t fake_http_client(void);
