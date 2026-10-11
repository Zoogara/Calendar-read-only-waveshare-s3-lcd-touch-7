idf_component_register(
    SRCS "rss_parse.c" "rss_feed.c"
    INCLUDE_DIRS "include"
    REQUIRES app_common esp_http_client esp-tls mbedtls heap esp_timer
)
