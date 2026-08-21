idf_component_register(
    SRCS "board_bsp.c" "ch422g.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd driver esp_timer lvgl esp_lvgl_port esp_lcd_touch_gt911
)
