idf_component_register(
    SRCS "calendar_ui.c" "ui_time.c" "ui_month.c" "ui_week.c" "ui_day.c" "ui_upnext.c" "ui_screensaver.c" "ui_clock.c" "ui_reminder.c" "ui_settings_dialog.c" "ui_lock.c" "ui_history.c" "ui_weather.c" "gcal_font_14.c" "gcal_font_20.c" "gcal_font_clock.c" "gcal_font_icon_clock.c" "gcal_font_icon_bell.c" "gcal_font_weather.c" "gcal_font_weather_48.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl app_common gcal board_bsp provisioning ota_update presence_sensor light_sensor ha_weather onthisday
)
