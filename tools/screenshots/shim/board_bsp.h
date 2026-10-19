#pragma once
/* Stand-in for components/board_bsp: no hardware here, so the display lock is a no-op. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
bool bsp_lvgl_lock(uint32_t timeout_ms);
void bsp_lvgl_unlock(void);
esp_err_t bsp_display_set_brightness_permille(uint16_t permille);
esp_err_t bsp_display_fade_brightness_permille(uint16_t permille, uint32_t fade_ms);
esp_err_t bsp_display_backlight(bool on);
void *bsp_get_i2c_bus(void);
