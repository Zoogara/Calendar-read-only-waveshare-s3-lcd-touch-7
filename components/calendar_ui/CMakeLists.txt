# The radar page's three base maps (zoom 5 = 1/2x, 6 = 1x, 7 = 2x): radar_base_zN.local.rgb565 (git-ignored: your own map,
# see tools/make_radar_base.py) when it exists, else the fictional radar_base_zN.rgb565 the repository ships.
set(RADAR_BASE_FILES "")
foreach(Z 5 6 7)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/radar_base_z${Z}.local.rgb565")
        set(F "radar_base_z${Z}.local.rgb565")
        set(SYM_${Z} "_binary_radar_base_z${Z}_local_rgb565_start")
    else()
        set(F "radar_base_z${Z}.rgb565")
        set(SYM_${Z} "_binary_radar_base_z${Z}_rgb565_start")
    endif()
    list(APPEND RADAR_BASE_FILES "${F}")
endforeach()

idf_component_register(
    SRCS "calendar_ui.c" "ui_time.c" "ui_month.c" "ui_week.c" "ui_day.c" "ui_upnext.c" "ui_screensaver.c" "ui_clock.c" "ui_reminder.c" "ui_settings_dialog.c" "ui_lock.c" "ui_history.c" "ui_feed.c" "ui_weather.c" "ui_detail.c" "ui_link.c" "ui_radar.c" "gcal_font_14.c" "gcal_font_20.c" "gcal_font_clock.c" "gcal_font_icon_clock.c" "gcal_font_icon_bell.c" "gcal_font_icon_radar.c" "gcal_font_weather.c" "gcal_font_weather_48.c" "gcal_font_weather_24.c" "gcal_font_ampm.c" "time_fmt.c"
    INCLUDE_DIRS "include"
    EMBED_FILES ${RADAR_BASE_FILES}
    REQUIRES readlater esp_http_client json lvgl app_common gcal board_bsp esp_app_format esp_timer heap provisioning ota_update presence_sensor light_sensor ha_weather onthisday rss_feed
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RADAR_BASE_SYM_5="${SYM_5}" RADAR_BASE_SYM_6="${SYM_6}" RADAR_BASE_SYM_7="${SYM_7}")
