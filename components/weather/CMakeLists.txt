idf_component_register(
    SRCS "weather.c" "weather_series.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json app_common esp_timer heap
)
