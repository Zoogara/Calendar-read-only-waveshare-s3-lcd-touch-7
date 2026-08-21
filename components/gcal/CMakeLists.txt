idf_component_register(
    SRCS "jwt_auth.c" "gcal_client.c" "event_store.c" "ics_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls mbedtls json app_common heap
)
