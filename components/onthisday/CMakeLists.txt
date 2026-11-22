idf_component_register(
    SRCS "onthisday.c"
    INCLUDE_DIRS "include"
    REQUIRES readlater esp_http_client esp-tls mbedtls json heap esp_timer
)
