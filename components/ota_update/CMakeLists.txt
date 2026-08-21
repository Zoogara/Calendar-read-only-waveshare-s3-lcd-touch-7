idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_ota esp_http_client esp-tls mbedtls esp_partition app_update
)
