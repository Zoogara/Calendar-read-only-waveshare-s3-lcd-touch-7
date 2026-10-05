idf_component_register(
    SRCS "ha_weather.c" "ha_weather_details.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json app_common esp_timer heap
)
