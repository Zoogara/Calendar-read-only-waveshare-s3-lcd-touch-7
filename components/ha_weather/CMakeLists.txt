idf_component_register(
    SRCS "ha_weather.c" "ha_weather_details.c" "ha_history.c" "ha_wind_calc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json app_common esp_timer heap
)
