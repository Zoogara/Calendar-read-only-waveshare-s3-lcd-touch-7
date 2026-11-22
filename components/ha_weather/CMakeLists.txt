idf_component_register(
    SRCS "ha_weather.c" "ha_weather_details.c" "ha_history.c" "ha_wind_calc.c" "ha_radar_pack.c" "ha_radar_data.c" "ha_radar_png.c" "ha_radar_inflate_rom.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_rom esp_http_client json app_common esp_timer heap
)
