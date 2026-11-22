# The radar page's base map: radar_base.local.rgb565 (git-ignored: your own map, see tools/make_radar_base.py) when it exists,
# else the fictional radar_base.rgb565 the repository ships.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/radar_base.local.rgb565")
    set(RADAR_BASE_FILE "radar_base.local.rgb565")
    set(RADAR_BASE_SYM "_binary_radar_base_local_rgb565_start")
else()
    set(RADAR_BASE_FILE "radar_base.rgb565")
    set(RADAR_BASE_SYM "_binary_radar_base_rgb565_start")
endif()

idf_component_register(
    SRCS "calendar_ui.c" "ui_time.c" "ui_month.c" "ui_week.c" "ui_day.c" "ui_upnext.c" "ui_screensaver.c" "ui_clock.c" "ui_reminder.c" "ui_settings_dialog.c" "ui_lock.c" "ui_history.c" "ui_feed.c" "ui_weather.c" "ui_detail.c" "ui_radar.c" "gcal_font_14.c" "gcal_font_20.c" "gcal_font_clock.c" "gcal_font_icon_clock.c" "gcal_font_icon_bell.c" "gcal_font_icon_radar.c" "gcal_font_weather.c" "gcal_font_weather_48.c" "gcal_font_weather_24.c" "gcal_font_ampm.c" "time_fmt.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "${RADAR_BASE_FILE}"
    REQUIRES esp_http_client json lvgl app_common gcal board_bsp esp_app_format esp_timer heap provisioning ota_update presence_sensor light_sensor ha_weather onthisday rss_feed
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RADAR_BASE_SYM="${RADAR_BASE_SYM}")
