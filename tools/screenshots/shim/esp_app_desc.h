#pragma once
/* A stand-in for ESP-IDF's app descriptor: the settings menu shows its version. */
typedef struct { const char *version; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { "v1.0-0-gdemo" };
    return &d;
}
