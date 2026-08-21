idf_component_register(
    SRCS "calendar_ui.c" "ui_time.c" "ui_month.c" "ui_week.c" "ui_day.c" "ui_upnext.c" "ui_screensaver.c" "ui_settings_dialog.c" "gcal_font_14.c" "gcal_font_20.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl app_common gcal board_bsp provisioning ota_update
)
